// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "nowplaying.h"
#include "porting.h"
#include "log.h"
#include <set>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <memory>

namespace {
	// Poll at most this often; MPRIS/D-Bus calls (and even window
	// enumeration) are cheap but there's no reason to hit them every frame.
	constexpr unsigned long long POLL_INTERVAL_MS = 1000;

	std::string guessSourceFromIdentity(const std::string &identity)
	{
		std::string lower = identity;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return std::tolower(c); });

		if (lower.find("spotify") != std::string::npos)
			return "Spotify";
		if (lower.find("youtube music") != std::string::npos || lower.find("music.youtube") != std::string::npos)
			return "YouTube Music";
		if (lower.find("soundcloud") != std::string::npos)
			return "SoundCloud";
		// zxcloli666/SoundCloud-Flutter (Dart+Flutter/Rust SoundCloud
		// client, Linux-only, MPRIS via its Rust core) -- its binary is
		// named "sc_desktop" and its MPRIS identity/bus name may just be
		// that, without spelling out "SoundCloud".
		if (lower.find("sc_desktop") != std::string::npos || lower.find("sc desktop") != std::string::npos)
			return "SoundCloud";
		return identity;
	}

	// Browsers expose the page URL of whatever's playing via the
	// MPRIS "xesam:url" metadata field, which is a much more reliable way
	// to tell "a browser tab playing YouTube Music" from "a browser tab
	// playing SoundCloud" than the browser's own (generic) app identity.
	std::string guessSourceFromUrl(const std::string &url)
	{
		std::string lower = url;
		std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) { return std::tolower(c); });

		if (lower.find("music.youtube.com") != std::string::npos)
			return "YouTube Music";
		if (lower.find("soundcloud.com") != std::string::npos)
			return "SoundCloud";
		if (lower.find("open.spotify.com") != std::string::npos)
			return "Spotify";
		return "";
	}

	// Refines a generic browser identity ("Chrome", "Firefox") into a
	// specific service name, preferring the page URL when we have one
	// (most reliable), falling back to the app identity otherwise.
	std::string guessSourceFromTitle(const std::string &identity, const std::string &url)
	{
		std::string from_url = guessSourceFromUrl(url);
		if (!from_url.empty())
			return from_url;

		std::string guess = guessSourceFromIdentity(identity);
		if (guess != identity)
			return guess; // already specific (Spotify/etc from the app itself)
		// Fall back: browsers just say "Chrome"/"Firefox"/etc, and we have
		// no URL to go on, so just label it with the player's own name.
		return identity.empty() ? "Media" : identity;
	}
}

#if defined(HAVE_DBUS)

#include <dbus/dbus.h>
#include <vector>

class DBusMprisPoller
{
public:
	DBusMprisPoller()
	{
		DBusError err;
		dbus_error_init(&err);
		m_conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
		if (dbus_error_is_set(&err)) {
			dbus_error_free(&err);
			m_conn = nullptr;
		}
	}

	~DBusMprisPoller()
	{
		// dbus_bus_get() returns a shared connection; do not close it,
		// just drop our reference.
		if (m_conn)
			dbus_connection_unref(m_conn);
	}

	bool poll(NowPlayingInfo *out)
	{
		if (!m_conn)
			return false;

		std::vector<std::string> names = listMprisNames();
		for (const std::string &name : names) {
			std::string status = getPlayerStringProperty(name, "PlaybackStatus");
			if (status != "Playing")
				continue;

			std::string title, artist, url;
			getMetadata(name, &title, &artist, &url);
			if (title.empty())
				continue;

			std::string identity = getIdentity(name);
			out->active = true;
			out->title = title;
			out->artist = artist;
			out->source = guessSourceFromTitle(identity, url);
			return true;
		}
		return false;
	}

private:
	DBusConnection *m_conn = nullptr;

	std::vector<std::string> listMprisNames()
	{
		std::vector<std::string> result;
		DBusMessage *msg = dbus_message_new_method_call(
			"org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "ListNames");
		if (!msg)
			return result;

		DBusMessage *reply = dbus_connection_send_with_reply_and_block(
			m_conn, msg, 200, nullptr);
		dbus_message_unref(msg);
		if (!reply)
			return result;

		DBusMessageIter iter, arr;
		if (dbus_message_iter_init(reply, &iter) &&
				dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_ARRAY) {
			dbus_message_iter_recurse(&iter, &arr);
			while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
				const char *name = nullptr;
				dbus_message_iter_get_basic(&arr, &name);
				if (name && std::string(name).rfind("org.mpris.MediaPlayer2.", 0) == 0)
					result.emplace_back(name);
				dbus_message_iter_next(&arr);
			}
		}
		dbus_message_unref(reply);
		return result;
	}

	// Reads a Properties.Get() reply that's known to be a plain string.
	std::string getPlayerStringProperty(const std::string &bus_name, const std::string &prop)
	{
		return getProperty(bus_name, "org.mpris.MediaPlayer2.Player", prop);
	}

	std::string getIdentity(const std::string &bus_name)
	{
		return getProperty(bus_name, "org.mpris.MediaPlayer2", "Identity");
	}

	std::string getProperty(const std::string &bus_name, const std::string &iface, const std::string &prop)
	{
		DBusMessage *msg = dbus_message_new_method_call(
			bus_name.c_str(), "/org/mpris/MediaPlayer2",
			"org.freedesktop.DBus.Properties", "Get");
		if (!msg)
			return "";

		const char *iface_c = iface.c_str();
		const char *prop_c = prop.c_str();
		dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &iface_c,
			DBUS_TYPE_STRING, &prop_c,
			DBUS_TYPE_INVALID);

		DBusMessage *reply = dbus_connection_send_with_reply_and_block(
			m_conn, msg, 200, nullptr);
		dbus_message_unref(msg);
		if (!reply)
			return "";

		std::string result;
		DBusMessageIter iter, variant;
		if (dbus_message_iter_init(reply, &iter) &&
				dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&iter, &variant);
			if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
				const char *s = nullptr;
				dbus_message_iter_get_basic(&variant, &s);
				if (s)
					result = s;
			}
		}
		dbus_message_unref(reply);
		return result;
	}

	// Metadata is a{sv}; we care about xesam:title (string), xesam:artist
	// (array of strings, we take the first one), and xesam:url (string,
	// used to identify which website a browser tab is playing from).
	void getMetadata(const std::string &bus_name, std::string *title, std::string *artist, std::string *url)
	{
		DBusMessage *msg = dbus_message_new_method_call(
			bus_name.c_str(), "/org/mpris/MediaPlayer2",
			"org.freedesktop.DBus.Properties", "Get");
		if (!msg)
			return;

		const char *iface_c = "org.mpris.MediaPlayer2.Player";
		const char *prop_c = "Metadata";
		dbus_message_append_args(msg,
			DBUS_TYPE_STRING, &iface_c,
			DBUS_TYPE_STRING, &prop_c,
			DBUS_TYPE_INVALID);

		DBusMessage *reply = dbus_connection_send_with_reply_and_block(
			m_conn, msg, 200, nullptr);
		dbus_message_unref(msg);
		if (!reply)
			return;

		DBusMessageIter iter, variant, dict;
		if (!dbus_message_iter_init(reply, &iter) ||
				dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
			dbus_message_unref(reply);
			return;
		}
		dbus_message_iter_recurse(&iter, &variant);
		if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
			dbus_message_unref(reply);
			return;
		}
		dbus_message_iter_recurse(&variant, &dict);

		while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter entry, value;
			dbus_message_iter_recurse(&dict, &entry);

			const char *key = nullptr;
			if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&entry, &key);
			dbus_message_iter_next(&entry);

			if (key && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
				dbus_message_iter_recurse(&entry, &value);
				std::string k = key;
				if (k == "xesam:title" &&
						dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
					const char *s = nullptr;
					dbus_message_iter_get_basic(&value, &s);
					if (s)
						*title = s;
				} else if (k == "xesam:artist" &&
						dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_ARRAY) {
					DBusMessageIter artist_arr;
					dbus_message_iter_recurse(&value, &artist_arr);
					if (dbus_message_iter_get_arg_type(&artist_arr) == DBUS_TYPE_STRING) {
						const char *s = nullptr;
						dbus_message_iter_get_basic(&artist_arr, &s);
						if (s)
							*artist = s;
					}
				} else if (k == "xesam:url" &&
						dbus_message_iter_get_arg_type(&value) == DBUS_TYPE_STRING) {
					const char *s = nullptr;
					dbus_message_iter_get_basic(&value, &s);
					if (s)
						*url = s;
				}
			}
			dbus_message_iter_next(&dict);
		}
		dbus_message_unref(reply);
	}
};

// Runs DBusMprisPoller on a dedicated background thread instead of the
// render thread. dbus_connection_send_with_reply_and_block() blocks for up
// to its 200ms timeout per call, and a single poll cycle above can make
// several such calls (ListNames, then PlaybackStatus/Metadata/Identity per
// candidate player) -- calling that directly from Hud::drawMusicHud() is
// exactly what caused the periodic frame-time spikes ("FPS drops to ~30 and
// immediately recovers"): once a second (see POLL_INTERVAL_MS below), the
// render thread would stall for however long that round trip took. Same
// fix shape as WinRtSmtcProvider on Windows further down this file --
// poll() (render thread) never blocks, it just reads the latest snapshot
// the worker thread produced.
class NowPlayingProviderImpl
{
public:
	NowPlayingProviderImpl()
	{
		m_worker = std::thread(&NowPlayingProviderImpl::workerMain, this);
	}

	~NowPlayingProviderImpl()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_worker.joinable())
			m_worker.join();
	}

	bool poll(NowPlayingInfo *out)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_ready || !m_latest.active)
			return false;
		*out = m_latest;
		return true;
	}

private:
	std::unique_ptr<DBusMprisPoller> m_poller; // created on, and only ever touched from, the worker thread
	std::thread m_worker;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_stop = false;
	bool m_ready = false;
	NowPlayingInfo m_latest;

	void workerMain()
	{
		m_poller = std::make_unique<DBusMprisPoller>();

		std::unique_lock<std::mutex> lock(m_mutex);
		while (!m_stop) {
			lock.unlock();
			NowPlayingInfo info;
			bool got = m_poller->poll(&info);
			lock.lock();
			m_latest = got ? info : NowPlayingInfo();
			m_ready = true;
			if (m_stop)
				break;
			m_cv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return m_stop; });
		}
	}
};

#elif defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef HAVE_WINRT_SMTC
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

namespace {
	// Windows' System Media Transport Controls -- the same OS-level "now
	// playing" mechanism behind the volume flyout's media controls, and
	// the Windows equivalent of Linux's MPRIS. Any app that integrates
	// with it (Spotify's desktop client does) shows up here with title,
	// artist, album art, and playback position, without needing to guess
	// anything from a window title.
	//
	// All WinRT calls happen on a dedicated background thread, never on
	// the caller's (render) thread. Two reasons:
	//   1. Irrlicht's window/input handling (and OLE drag-and-drop,
	//      clipboard, etc.) already puts the render thread into a
	//      single-threaded apartment (STA) at startup. IAsyncOperation
	//      ::get() blocking-waits are documented by Microsoft as unsafe
	//      on an STA thread -- they can deadlock waiting on a completion
	//      callback that never gets delivered, which is exactly what
	//      caused the client to hang at the "Готово!"/"Done!" loading
	//      screen once SMTC polling actually started running.
	//   2. A brand new thread has no COM state of its own, so
	//      initializing a genuine multi-threaded apartment (MTA) there
	//      never conflicts with whatever the render thread already set
	//      up, and blocking .get() calls are safe there since that
	//      thread has nothing else to do but wait on them.
	// poll() (called from the render thread) never touches WinRT itself;
	// it just reads the latest snapshot the worker thread produced.
	class WinRtSmtcProvider
	{
	public:
		WinRtSmtcProvider()
		{
			m_worker = std::thread(&WinRtSmtcProvider::workerMain, this);
		}

		~WinRtSmtcProvider()
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_stop = true;
			}
			m_cv.notify_all();
			if (m_worker.joinable())
				m_worker.join();
		}

		// Called from the render thread. Never blocks on WinRT -- just
		// returns whatever the background thread last produced.
		bool poll(NowPlayingInfo *out)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_ready || !m_latest.active)
				return false;
			*out = m_latest;
			return true;
		}

	private:
		std::thread m_worker;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		bool m_stop = false;
		bool m_ready = false;
		NowPlayingInfo m_latest;

		void workerMain()
		{
			bool initialized = false;
			try {
				winrt::init_apartment(winrt::apartment_type::multi_threaded);
				initialized = true;
				infostream << "MusicHud: WinRT worker thread apartment initialized OK" << std::endl;
			} catch (const winrt::hresult_error &e) {
				warningstream << "MusicHud: WinRT worker thread init_apartment failed: 0x"
					<< std::hex << (unsigned int)e.code() << std::dec
					<< " " << narrow(std::wstring(e.message().c_str())) << std::endl;
			} catch (...) {
				warningstream << "MusicHud: WinRT worker thread init_apartment failed "
					"(unknown exception)" << std::endl;
			}

			if (!initialized)
				return;

			std::unique_lock<std::mutex> lock(m_mutex);
			while (!m_stop) {
				lock.unlock();
				NowPlayingInfo info;
				bool got = pollOnce(&info);
				lock.lock();
				m_latest = got ? info : NowPlayingInfo();
				m_ready = true;
				if (m_stop)
					break;
				m_cv.wait_for(lock, std::chrono::milliseconds(1000), [this] { return m_stop; });
			}
			lock.unlock();

			winrt::uninit_apartment();
		}

		// Only ever called from workerMain() on the dedicated thread.
		bool pollOnce(NowPlayingInfo *out)
		{
			using namespace winrt::Windows::Media::Control;

			try {
				auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				auto session = manager.GetCurrentSession();
				if (!session) {
					logOnce("MusicHud: SMTC has no current session");
					return false;
				}

				auto playback_info = session.GetPlaybackInfo();
				if (playback_info.PlaybackStatus() !=
						GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
					logOnce("MusicHud: SMTC session found but not in Playing state");
					return false;
				}

				auto props = session.TryGetMediaPropertiesAsync().get();
				std::wstring wtitle(props.Title());
				if (wtitle.empty()) {
					logOnce("MusicHud: SMTC session Playing but title is empty");
					return false;
				}

				out->active = true;
				out->title = narrow(wtitle);
				out->artist = narrow(std::wstring(props.Artist()));
				out->source = guessSource(narrow(std::wstring(session.SourceAppUserModelId())));

				fillProgress(session, out);
				fillThumbnail(props, out);

				return true;
			} catch (const winrt::hresult_error &e) {
				std::ostringstream oss;
				oss << "MusicHud: SMTC poll threw hresult_error: 0x" << std::hex
					<< (unsigned int)e.code() << " " << narrow(std::wstring(e.message().c_str()));
				logOnce(oss.str());
				return false;
			} catch (...) {
				logOnce("MusicHud: SMTC poll threw an unknown exception");
				return false;
			}
		}

		// Only spam the log the first time a given failure message is
		// seen, since the worker loop runs continuously -- avoids
		// flooding debug.txt while still surfacing the reason to the
		// player.
		static void logOnce(const std::string &msg)
		{
			static std::set<std::string> seen;
			if (seen.insert(msg).second)
				warningstream << msg << std::endl;
		}

		static void fillProgress(
				const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession &session,
				NowPlayingInfo *out)
		{
			auto timeline = session.GetTimelineProperties();
			long long position_ticks = timeline.Position().count();
			long long end_ticks = timeline.EndTime().count();
			// TimeSpan ticks are 100ns units; 10,000,000 ticks per second.
			long long duration_s = end_ticks / 10000000LL;
			if (duration_s <= 0)
				return;

			out->has_progress = true;
			out->position_seconds = (int)(position_ticks / 10000000LL);
			out->duration_seconds = (int)duration_s;
		}

		static void fillThumbnail(
				const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties &props,
				NowPlayingInfo *out)
		{
			auto thumb_ref = props.Thumbnail();
			if (!thumb_ref)
				return;

			auto stream = thumb_ref.OpenReadAsync().get();
			uint32_t size = (uint32_t)stream.Size();
			// Sanity bound -- real album art is a few dozen KB at most.
			if (size == 0 || size > 8 * 1024 * 1024)
				return;

			winrt::Windows::Storage::Streams::DataReader reader(stream);
			reader.LoadAsync(size).get();

			std::string bytes;
			bytes.resize(size);
			reader.ReadBytes(winrt::array_view<uint8_t>(
				reinterpret_cast<uint8_t *>(bytes.data()),
				reinterpret_cast<uint8_t *>(bytes.data()) + size));

			out->has_thumbnail = true;
			out->thumbnail_data = std::move(bytes);
			// Cheap "did the art change" signal for callers that cache a
			// decoded texture: hash the (small) byte buffer rather than
			// storing/comparing it wholesale.
			unsigned long long hash = 1469598103934665603ULL; // FNV-1a offset basis
			for (unsigned char c : out->thumbnail_data) {
				hash ^= c;
				hash *= 1099511628211ULL; // FNV-1a prime
			}
			out->thumbnail_id = hash;
		}

		static std::string guessSource(const std::string &aumid)
		{
			std::string lower = aumid;
			std::transform(lower.begin(), lower.end(), lower.begin(),
				[](unsigned char c) { return std::tolower(c); });

			if (lower.find("spotify") != std::string::npos)
				return "Spotify";
			if (lower.find("soundcloud") != std::string::npos)
				return "SoundCloud";
			if (lower.find("yandex") != std::string::npos)
				return "Yandex Music";
			if (lower.find("youtube") != std::string::npos)
				return "YouTube Music";
			// Browsers (Edge, Chrome, Firefox, ...) show up under their own
			// AUMID here regardless of which site's tab is playing -- SMTC
			// doesn't expose the page URL the way MPRIS's xesam:url does,
			// so we can't tell YouTube Music from SoundCloud from a
			// browser tab this way. Label it generically.
			if (lower.find("msedge") != std::string::npos ||
					lower.find("chrome") != std::string::npos ||
					lower.find("firefox") != std::string::npos)
				return "Media";
			return aumid.empty() ? "Media" : aumid;
		}

		static std::string narrow(const std::wstring &s)
		{
			if (s.empty())
				return "";
			int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
				nullptr, 0, nullptr, nullptr);
			std::string out(size, '\0');
			WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
				out.data(), size, nullptr, nullptr);
			return out;
		}
	};
}
#endif // HAVE_WINRT_SMTC

namespace {
	// Known desktop-app processes we can identify with confidence, each
	// showing "Artist <sep> Title" in its window title while a track is
	// playing. Yandex Music has an official Electron-based Windows client
	// (installed under %localappdata%\Programs\YandexMusic). YouTube Music
	// has no official desktop app, but the popular unofficial th-ch/
	// youtube-music Electron wrapper runs as "YouTube Music.exe" and is
	// common enough to support directly. SoundCloud's old Microsoft Store
	// UWP app (when installed) runs as "SoundCloud.exe". SoundCouch, a
	// popular third-party SoundCloud desktop wrapper, runs as
	// "soundcouch.exe".
	//
	// NOTE: this only catches these services when used through an actual
	// installed desktop app. Playing them in a regular browser tab isn't
	// detectable this way on Windows -- a browser's window title only
	// reflects whichever tab is currently focused, and (unlike the Linux/
	// MPRIS path, which gets a real page URL) there is no reliable way to
	// tell "a Chrome tab playing YouTube Music" apart from any other tab
	// from the title text alone.
	struct KnownApp {
		const wchar_t *process_name;
		const char *source;
	};
	constexpr KnownApp knownApps[] = {
		{ L"Spotify.exe", "Spotify" },
		{ L"Yandex Music.exe", "Yandex Music" },
		{ L"YouTube Music.exe", "YouTube Music" },
		{ L"SoundCloud.exe", "SoundCloud" },
		{ L"soundcouch.exe", "SoundCloud" },
		// zxcloli666/SoundCloud-Flutter Windows build
		// (releases/download/v0.1.0/soundcloud-windows-x64.zip). Flutter
		// names the produced .exe after the project's windows/runner
		// CMakeLists.txt BINARY_NAME, which we can't inspect directly, so
		// a couple of plausible names are covered here.
		{ L"soundcloud_flutter.exe", "SoundCloud" },
		{ L"sc_desktop.exe", "SoundCloud" },
	};

	// UWP/Store apps (e.g. the official SoundCloud app,
	// https://apps.microsoft.com/detail/9N5QR3KW6CMC) don't own their own
	// visible top-level window -- it belongs to the shared system process
	// "ApplicationFrameHost.exe", which hosts the title bar/frame for
	// every UWP app currently running. Matching by that host process's
	// name would match ANY UWP app, not specifically SoundCloud. The
	// actual app content lives in a child window of class
	// "Windows.UI.Core.CoreWindow" that belongs to the app's own process,
	// whose install path reliably contains the app's identity (its
	// package folder under WindowsApps is named after the app), even
	// though the exe name itself isn't guaranteed to be "SoundCloud.exe".
	struct UwpApp {
		const wchar_t *path_substring;
		const char *source;
	};
	constexpr UwpApp uwpApps[] = {
		{ L"soundcloud", "SoundCloud" },
	};

	bool pathContainsCaseInsensitive(const std::wstring &haystack, const wchar_t *needle)
	{
		std::wstring lower_haystack = haystack;
		for (wchar_t &c : lower_haystack)
			c = towlower(c);
		std::wstring lower_needle = needle;
		for (wchar_t &c : lower_needle)
			c = towlower(c);
		return lower_haystack.find(lower_needle) != std::wstring::npos;
	}

	// Splits a window title into artist/track. Most apps show
	// "Artist <sep> Title" with a hyphen or dash, but SoundCloud-style
	// players tend to show "Title by Artist" instead, so that ordering
	// is tried too.
	bool splitArtistTitle(const std::wstring &wtitle, std::wstring *artist, std::wstring *track)
	{
		static const wchar_t *dash_seps[] = { L" - ", L" \u2013 ", L" \u2014 " };
		for (const wchar_t *sep : dash_seps) {
			size_t pos = wtitle.find(sep);
			if (pos == std::wstring::npos)
				continue;
			*artist = wtitle.substr(0, pos);
			*track = wtitle.substr(pos + wcslen(sep));
			if (!artist->empty() && !track->empty())
				return true;
		}

		size_t by_pos = wtitle.find(L" by ");
		if (by_pos != std::wstring::npos) {
			*track = wtitle.substr(0, by_pos);
			*artist = wtitle.substr(by_pos + 4);
			if (!artist->empty() && !track->empty())
				return true;
		}

		return false;
	}
}

class NowPlayingProviderImpl
{
public:
	NowPlayingProviderImpl()
	{
#ifdef HAVE_WINRT_SMTC
		infostream << "MusicHud: built with HAVE_WINRT_SMTC (SMTC album art/progress enabled)" << std::endl;
#else
		warningstream << "MusicHud: built WITHOUT HAVE_WINRT_SMTC "
			"(falling back to window-title scraping only -- no art/progress)" << std::endl;
#endif
	}

	bool poll(NowPlayingInfo *out)
	{
#ifdef HAVE_WINRT_SMTC
		if (m_smtc.poll(out))
			return true;
#endif
		m_found = false;
		m_out = out;
		EnumWindows(&NowPlayingProviderImpl::enumProc, reinterpret_cast<LPARAM>(this));
		return m_found;
	}

private:
#ifdef HAVE_WINRT_SMTC
	WinRtSmtcProvider m_smtc;
#endif
	bool m_found = false;
	NowPlayingInfo *m_out = nullptr;

	static BOOL CALLBACK enumProc(HWND hwnd, LPARAM lparam)
	{
		auto *self = reinterpret_cast<NowPlayingProviderImpl*>(lparam);

		if (!IsWindowVisible(hwnd))
			return TRUE;

		// Identify the window by its owning process rather than by window
		// class name -- several of these apps (Spotify included) have
		// changed their main window class over the years (e.g. Spotify
		// used to be "SpotifyMainWindow", then switched to the generic
		// Chromium default "Chrome_WidgetWin_0"), so matching on class
		// name is fragile and breaks silently across updates. The process
		// name is far more stable.
		const char *source = self->matchKnownProcess(hwnd);
		if (!source)
			return TRUE;

		wchar_t title[512];
		int len = GetWindowTextW(hwnd, title, 512);
		if (len <= 0)
			return TRUE;

		std::wstring wtitle(title, len);
		std::wstring wartist, wtrack;
		if (!splitArtistTitle(wtitle, &wartist, &wtrack))
			return TRUE; // no track playing right now

		std::string artist = narrow(wartist);
		std::string track = narrow(wtrack);
		if (artist.empty() || track.empty())
			return TRUE;

		self->m_out->active = true;
		self->m_out->artist = artist;
		self->m_out->title = track;
		self->m_out->source = source;
		self->m_found = true;
		return FALSE; // stop enumerating, we found it
	}

	const char *matchKnownProcess(HWND hwnd)
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(hwnd, &pid);
		if (pid == 0)
			return nullptr;

		std::wstring exe_name = getProcessExeName(pid);
		if (exe_name.empty())
			return nullptr;

		if (_wcsicmp(exe_name.c_str(), L"ApplicationFrameHost.exe") == 0)
			return matchUwpApp(hwnd);

		for (const KnownApp &app : knownApps) {
			if (_wcsicmp(exe_name.c_str(), app.process_name) == 0)
				return app.source;
		}
		return nullptr;
	}

	// UWP apps host their visible frame in ApplicationFrameHost.exe, but
	// their actual content -- and thus their real, identifiable process --
	// lives in a child "Windows.UI.Core.CoreWindow".
	const char *matchUwpApp(HWND frame_hwnd)
	{
		HWND core_hwnd = FindWindowExW(frame_hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
		if (!core_hwnd)
			return nullptr;

		DWORD pid = 0;
		GetWindowThreadProcessId(core_hwnd, &pid);
		if (pid == 0)
			return nullptr;

		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process)
			return nullptr;

		wchar_t path[MAX_PATH];
		DWORD path_len = MAX_PATH;
		bool ok = QueryFullProcessImageNameW(process, 0, path, &path_len);
		CloseHandle(process);
		if (!ok)
			return nullptr;

		std::wstring wpath(path, path_len);
		for (const UwpApp &app : uwpApps) {
			if (pathContainsCaseInsensitive(wpath, app.path_substring))
				return app.source;
		}
		return nullptr;
	}

	static std::wstring getProcessExeName(DWORD pid)
	{
		HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (!process)
			return L"";

		wchar_t path[MAX_PATH];
		DWORD path_len = MAX_PATH;
		bool ok = QueryFullProcessImageNameW(process, 0, path, &path_len);
		CloseHandle(process);
		if (!ok)
			return L"";

		std::wstring wpath(path, path_len);
		size_t slash = wpath.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? wpath : wpath.substr(slash + 1);
	}

	static std::string narrow(const std::wstring &s)
	{
		if (s.empty())
			return "";
		int size = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
			nullptr, 0, nullptr, nullptr);
		std::string out(size, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
			out.data(), size, nullptr, nullptr);
		return out;
	}
};

#elif defined(__APPLE__)

#include <dlfcn.h>
#include <dispatch/dispatch.h>
#include <CoreFoundation/CoreFoundation.h>

namespace {
	// MediaRemote is a private, undocumented Apple framework -- there is
	// no public API for reading *another* app's now-playing info on
	// macOS (MPNowPlayingInfoCenter only lets an app publish its own).
	// This is the same technique widely-used third-party "now playing"
	// tools (nowplaying-cli, MediaRemoteAdapter, various menu bar apps)
	// rely on: dlopen the framework at its fixed system path and dlsym
	// the handful of C functions it exports, since there's no header to
	// link against normally. Being private/undocumented, this can in
	// principle stop working in a future macOS release; if it ever does,
	// dlopen/dlsym simply fail and MediaRemoteProvider::poll() below
	// always returns false, same as "unsupported platform" -- it doesn't
	// crash or need a code change to keep working everywhere else.
	typedef void (^NowPlayingInfoBlock)(CFDictionaryRef information);
	typedef void (*GetNowPlayingInfoFunc)(dispatch_queue_t queue, NowPlayingInfoBlock handler);

	std::string cfStringToStd(CFStringRef s)
	{
		if (!s)
			return "";
		CFIndex len = CFStringGetLength(s);
		CFIndex max_size = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
		std::string out(max_size, '\0');
		if (!CFStringGetCString(s, out.data(), max_size, kCFStringEncodingUTF8))
			return "";
		out.resize(strlen(out.c_str()));
		return out;
	}
}

// All MediaRemote calls happen on a dedicated dispatch queue via its own
// (callback-based, not synchronous) API -- poll() itself never blocks:
// it kicks off a fresh async request and returns whatever the *previous*
// request produced, so it's always one poll cycle (up to
// POLL_INTERVAL_MS, see the top of this file) behind. Unnoticeable for a
// "what's playing" HUD, and avoids blocking the render thread on an
// inter-process call to a system daemon (mediaremoted).
class MediaRemoteProvider
{
public:
	MediaRemoteProvider()
	{
		m_handle = dlopen(
			"/System/Library/PrivateFrameworks/MediaRemote.framework/MediaRemote",
			RTLD_LAZY);
		if (!m_handle)
			return;
		m_get_info = (GetNowPlayingInfoFunc)dlsym(m_handle, "MRMediaRemoteGetNowPlayingInfo");
		if (!m_get_info)
			return;
		m_queue = dispatch_queue_create("org.mineboostv2.nowplaying", DISPATCH_QUEUE_SERIAL);
	}

	~MediaRemoteProvider()
	{
		if (m_handle)
			dlclose(m_handle);
	}

	bool poll(NowPlayingInfo *out)
	{
		if (!m_get_info)
			return false;

		bool have_result;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			have_result = m_ready && m_latest.active;
			if (have_result)
				*out = m_latest;
		}
		requestUpdate();
		return have_result;
	}

private:
	void requestUpdate()
	{
		if (m_request_pending.exchange(true))
			return; // Previous request hasn't come back yet.

		// __block-less: the handler only reads `this` (a stable pointer
		// for MediaRemoteProvider's whole lifetime), so it's captured by
		// value like any other variable a block closes over.
		MediaRemoteProvider *self = this;
		m_get_info(m_queue, ^(CFDictionaryRef info) {
			self->handleInfo(info);
			self->m_request_pending = false;
		});
	}

	void handleInfo(CFDictionaryRef info)
	{
		NowPlayingInfo result;
		if (info) {
			CFStringRef title = (CFStringRef)CFDictionaryGetValue(
				info, CFSTR("kMRMediaRemoteNowPlayingInfoTitle"));
			CFStringRef artist = (CFStringRef)CFDictionaryGetValue(
				info, CFSTR("kMRMediaRemoteNowPlayingInfoArtist"));

			if (title || artist) {
				// No reliable "is it actually playing vs paused" flag
				// in every macOS version's info dict (the playback-rate
				// key isn't always present) -- if MediaRemote is
				// reporting *any* now-playing info at all, treat that
				// as active. Matches this file's Linux/MPRIS behavior,
				// which does the same when a player's PlaybackStatus
				// property is missing.
				result.active = true;
				result.title = cfStringToStd(title);
				result.artist = cfStringToStd(artist);
				result.source = "Media";

				CFNumberRef elapsed = (CFNumberRef)CFDictionaryGetValue(
					info, CFSTR("kMRMediaRemoteNowPlayingInfoElapsedTime"));
				CFNumberRef duration = (CFNumberRef)CFDictionaryGetValue(
					info, CFSTR("kMRMediaRemoteNowPlayingInfoDuration"));
				double elapsed_s = 0.0, duration_s = 0.0;
				if (elapsed && CFNumberGetValue(elapsed, kCFNumberDoubleType, &elapsed_s) &&
						duration && CFNumberGetValue(duration, kCFNumberDoubleType, &duration_s) &&
						duration_s > 0.0) {
					result.has_progress = true;
					result.position_seconds = (int)elapsed_s;
					result.duration_seconds = (int)duration_s;
				}

				CFDataRef art = (CFDataRef)CFDictionaryGetValue(
					info, CFSTR("kMRMediaRemoteNowPlayingInfoArtworkData"));
				if (art) {
					CFIndex len = CFDataGetLength(art);
					if (len > 0) {
						result.has_thumbnail = true;
						result.thumbnail_data.assign(
							(const char *)CFDataGetBytePtr(art), (size_t)len);
						result.thumbnail_id = (unsigned long long)CFHash(art);
					}
				}
			}
		}

		std::lock_guard<std::mutex> lock(m_mutex);
		m_latest = result;
		m_ready = true;
	}

	void *m_handle = nullptr;
	GetNowPlayingInfoFunc m_get_info = nullptr;
	dispatch_queue_t m_queue = nullptr;
	std::atomic<bool> m_request_pending{false};
	std::mutex m_mutex;
	bool m_ready = false;
	NowPlayingInfo m_latest;
};

class NowPlayingProviderImpl
{
public:
	bool poll(NowPlayingInfo *out) { return m_provider.poll(out); }

private:
	MediaRemoteProvider m_provider;
};

#else

class NowPlayingProviderImpl
{
public:
	bool poll(NowPlayingInfo *) { return false; }
};

#endif

NowPlayingProvider::NowPlayingProvider() : m_impl(new NowPlayingProviderImpl()) {}

NowPlayingProvider::~NowPlayingProvider()
{
	delete m_impl;
}

const NowPlayingInfo &NowPlayingProvider::poll()
{
	unsigned long long now = porting::getTimeMs();
	if (now - m_last_poll_ms < POLL_INTERVAL_MS && m_last_poll_ms != 0)
		return m_last;
	m_last_poll_ms = now;

	NowPlayingInfo info;
	if (m_impl->poll(&info))
		m_last = info;
	else
		m_last = NowPlayingInfo(); // nothing playing (or unsupported platform)

	return m_last;
}
