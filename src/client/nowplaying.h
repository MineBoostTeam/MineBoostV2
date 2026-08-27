// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// MusicHud "now playing" backend.
//
// On Linux (when built with dbus-1 available), reads currently playing
// media from any app that exposes the standard MPRIS2 D-Bus interface.
// This covers the Spotify desktop client directly, and covers YouTube
// Music / SoundCloud (or anything else) played in a tab of a modern
// Chromium- or Firefox-based browser, since those browsers register an
// MPRIS player for whichever tab is currently playing audio/video.
//
// On Windows, when built with the mingw-w64 cppwinrt package available
// (pacman -S mingw-w64-clang-x86_64-cppwinrt on MSYS2 CLANG64), reads
// from the System Media Transport Controls (SMTC) -- the same OS-level
// "now playing" mechanism that powers the volume flyout's media
// controls. This is the Windows equivalent of MPRIS: any app that
// integrates with it (Spotify's desktop client does) shows up here,
// including title, artist, album art, and playback position/duration.
// If the cppwinrt headers aren't available at build time, this falls
// back to reading the Spotify desktop window's title bar
// ("Artist - Title") plus a couple of known third-party desktop
// wrappers, which doesn't provide art or progress.
//
// On any other platform, this is a harmless no-op stub.

#pragma once

#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

struct NowPlayingInfo
{
	bool active = false;
	std::string title;
	std::string artist;
	// Human-readable source label, e.g. "Spotify", "YouTube Music",
	// "SoundCloud", or the underlying player/browser name if unrecognized.
	std::string source;

	// Playback position, when the backend can provide it (currently
	// Windows SMTC and macOS).
	bool has_progress = false;
	int position_seconds = 0;
	int duration_seconds = 0;

	// Raw encoded album art bytes (PNG/JPEG, whatever the source app
	// provided), when available (currently Windows SMTC and macOS).
	// thumbnail_id changes any time the artwork changes, so a caller
	// that's cached a decoded texture can cheaply tell "is this still
	// the same image" without comparing thumbnail_data itself.
	bool has_thumbnail = false;
	std::string thumbnail_data;
	unsigned long long thumbnail_id = 0;
};

class NowPlayingProviderImpl;

// Polls whatever is currently playing on the system. Construction/destruction
// may allocate platform resources (e.g. a D-Bus connection); polling itself
// is internally throttled so it's cheap to call every frame.
class NowPlayingProvider
{
public:
	NowPlayingProvider();
	~NowPlayingProvider();

	NowPlayingProvider(const NowPlayingProvider &) = delete;
	NowPlayingProvider &operator=(const NowPlayingProvider &) = delete;

	// Returns the last known now-playing info, refreshing it at most a
	// few times per second internally.
	const NowPlayingInfo &poll();

private:
	NowPlayingProviderImpl *m_impl;
	unsigned long long m_last_poll_ms = 0;
	NowPlayingInfo m_last;
};
