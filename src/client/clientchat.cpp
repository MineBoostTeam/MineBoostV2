// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 ClientChat -- see clientchat.h for the full protocol.

#include "client/clientchat.h"

#include <algorithm>
#include <json/json.h>
#include <memory>
#include <sstream>

#include "convert_json.h"
#include "httpfetch.h"
#include "log.h"
#include "porting.h"
#include "settings.h"

ClientChat &ClientChat::get()
{
	static ClientChat instance;
	return instance;
}

ClientChat::~ClientChat()
{
	// Same reasoning as MineBoostPresence's destructor (see
	// mineboost_presence.cpp) -- this only ever runs once, at process
	// exit, so the "can be expensive" warning on httpfetch_caller_free()
	// doesn't matter here.
	if (m_http_caller != 0)
		httpfetch_caller_free(m_http_caller);
}

std::string ClientChat::getServerUrl() const
{
	return g_settings->get("clientchat_server_url");
}

std::string ClientChat::getDiscordUrl() const
{
	// exists() first, not a bare get(): this is read directly from a
	// button click handler (GUIClientChat::openDiscordLink(), src/gui/
	// guiClientChat.cpp) with nothing upstream catching an exception --
	// g_settings->get() throws SettingNotFoundException for any setting
	// missing both a saved value and a registered default, which would
	// otherwise crash the whole client on click (see the identical
	// reasoning for safeGetString() in src/gui/custom_menu/Menu.cpp).
	// "" either way is exactly what an actually-missing-and-unset
	// "clientchat_discord_url" should read as, same as its own
	// setDefault() in src/defaultsettings.cpp already provides.
	return g_settings->exists("clientchat_discord_url") ?
		g_settings->get("clientchat_discord_url") : "";
}

void ClientChat::loadCachedSession()
{
	if (g_settings->exists("clientchat_session_token")) {
		m_session_token = g_settings->get("clientchat_session_token");
		m_discord_id = g_settings->get("clientchat_discord_id");
		m_discord_name = g_settings->get("clientchat_discord_name");
		if (!m_session_token.empty())
			m_auth_state = AuthState::LoggedIn;
	}
	loadFriendIds();
}

void ClientChat::saveCachedSession()
{
	g_settings->set("clientchat_session_token", m_session_token);
	g_settings->set("clientchat_discord_id", m_discord_id);
	g_settings->set("clientchat_discord_name", m_discord_name);
}

void ClientChat::clearCachedSession()
{
	g_settings->remove("clientchat_session_token");
	g_settings->remove("clientchat_discord_id");
	g_settings->remove("clientchat_discord_name");
}

void ClientChat::loadFriendIds()
{
	m_friend_ids.clear();
	if (!g_settings->exists("clientchat_friend_ids"))
		return;
	std::string raw = g_settings->get("clientchat_friend_ids");
	std::stringstream ss(raw);
	std::string id;
	while (std::getline(ss, id, ',')) {
		if (!id.empty())
			m_friend_ids.push_back(id);
	}
}

void ClientChat::saveFriendIds()
{
	std::string joined;
	for (size_t i = 0; i < m_friend_ids.size(); i++) {
		if (i)
			joined += ",";
		joined += m_friend_ids[i];
	}
	g_settings->set("clientchat_friend_ids", joined);
}

void ClientChat::addFriendById(const std::string &discord_id)
{
	if (discord_id.empty() || isFriendId(discord_id))
		return;
	m_friend_ids.push_back(discord_id);
	saveFriendIds();
}

void ClientChat::removeFriendById(const std::string &discord_id)
{
	auto it = std::find(m_friend_ids.begin(), m_friend_ids.end(), discord_id);
	if (it == m_friend_ids.end())
		return;
	m_friend_ids.erase(it);
	saveFriendIds();
}

bool ClientChat::isFriendId(const std::string &discord_id) const
{
	return std::find(m_friend_ids.begin(), m_friend_ids.end(), discord_id)
		!= m_friend_ids.end();
}

void ClientChat::startLogin()
{
	if (m_auth_state != AuthState::LoggedOut)
		return;
	if (!isServerConfigured()) {
		m_last_error = "clientchat_server_url is not set";
		return;
	}
	m_auth_state = AuthState::Pairing;
	m_pair_code.clear();
	m_verify_url.clear();
	m_poll_token.clear();
	m_last_error.clear();
	startPairRequest();
}

void ClientChat::logout()
{
	if (m_auth_state == AuthState::LoggedIn && m_http_caller != 0 && !m_request_pending) {
		Json::Value req_body;
		req_body["session_token"] = m_session_token;

		HTTPFetchRequest req;
		req.url = getServerUrl() + "/api/logout";
		req.caller = m_http_caller;
		req.request_id = REQUEST_LOGOUT;
		req.method = HTTP_POST;
		req.raw_data = fastWriteJson(req_body);
		req.extra_headers.emplace_back("Content-Type: application/json");
		req.timeout = 8000;
		req.connect_timeout = 8000;
		httpfetch_async(req);
		// Deliberately not tracked via m_request_pending -- logging out
		// locally (below) shouldn't have to wait on this round-trip, and
		// there's nothing meaningful to do with its result either way.
	}

	m_auth_state = AuthState::LoggedOut;
	m_session_token.clear();
	m_discord_id.clear();
	m_discord_name.clear();
	m_messages.clear();
	m_online_users.clear();
	m_last_message_id = 0;
	clearCachedSession();
}

void ClientChat::sendMessage(const std::string &text, const std::string &to_id)
{
	if (m_auth_state != AuthState::LoggedIn || text.empty())
		return;
	m_send_queue.push_back({text, to_id});
}

void ClientChat::step(float dtime, const std::string &player_name)
{
	m_player_name = player_name;

	static bool loaded_once = false;
	if (!loaded_once) {
		loaded_once = true;
		loadCachedSession();
	}

	if (!isServerConfigured())
		return;

	if (m_http_caller == 0)
		m_http_caller = httpfetch_caller_alloc();

	// Drain whatever the last request came back with before considering
	// firing a new one -- same "one request in flight" discipline as
	// MineBoostPresence::step().
	if (m_request_pending) {
		HTTPFetchResult result;
		while (httpfetch_async_get(m_http_caller, result)) {
			if (result.request_id != m_pending_request_id)
				continue;
			m_request_pending = false;
			handleResult(result.request_id, result.succeeded, result.data);
		}
		if (m_request_pending)
			return;
	}

	switch (m_auth_state) {
	case AuthState::LoggedOut:
		return;

	case AuthState::Pairing:
		m_pair_poll_timer += dtime;
		m_pair_expires_in -= dtime;
		if (m_pair_expires_in <= 0.0f) {
			m_auth_state = AuthState::LoggedOut;
			m_last_error = "Pairing code expired, try again";
			return;
		}
		if (m_pair_poll_timer >= PAIR_POLL_INTERVAL_SECONDS) {
			m_pair_poll_timer = 0.0f;
			pollPairStatus();
		}
		return;

	case AuthState::LoggedIn:
		if (!m_send_queue.empty()) {
			flushSendQueue();
			return;
		}
		m_msg_poll_timer += dtime;
		if (m_msg_poll_timer >= MSG_POLL_INTERVAL_SECONDS) {
			m_msg_poll_timer = 0.0f;
			pollMessages();
		}
		return;
	}
}

void ClientChat::startPairRequest()
{
	Json::Value req_body(Json::objectValue);

	HTTPFetchRequest req;
	req.url = getServerUrl() + "/api/pair";
	req.caller = m_http_caller;
	req.request_id = REQUEST_PAIR;
	req.method = HTTP_POST;
	req.raw_data = fastWriteJson(req_body);
	req.extra_headers.emplace_back("Content-Type: application/json");
	req.timeout = 8000;
	req.connect_timeout = 8000;
	httpfetch_async(req);
	m_request_pending = true;
	m_pending_request_id = REQUEST_PAIR;
}

void ClientChat::pollPairStatus()
{
	Json::Value req_body;
	req_body["poll_token"] = m_poll_token;

	HTTPFetchRequest req;
	req.url = getServerUrl() + "/api/pair/poll";
	req.caller = m_http_caller;
	req.request_id = REQUEST_PAIR_POLL;
	req.method = HTTP_POST;
	req.raw_data = fastWriteJson(req_body);
	req.extra_headers.emplace_back("Content-Type: application/json");
	req.timeout = 8000;
	req.connect_timeout = 8000;
	httpfetch_async(req);
	m_request_pending = true;
	m_pending_request_id = REQUEST_PAIR_POLL;
}

void ClientChat::flushSendQueue()
{
	if (m_send_queue.empty())
		return;
	QueuedMessage msg = m_send_queue.front();
	m_send_queue.pop_front();

	Json::Value req_body;
	req_body["session_token"] = m_session_token;
	req_body["player_name"] = m_player_name;
	req_body["text"] = msg.text;
	if (!msg.to_id.empty())
		req_body["to_id"] = msg.to_id;

	HTTPFetchRequest req;
	req.url = getServerUrl() + "/api/send";
	req.caller = m_http_caller;
	req.request_id = REQUEST_SEND;
	req.method = HTTP_POST;
	req.raw_data = fastWriteJson(req_body);
	req.extra_headers.emplace_back("Content-Type: application/json");
	req.timeout = 8000;
	req.connect_timeout = 8000;
	httpfetch_async(req);
	m_request_pending = true;
	m_pending_request_id = REQUEST_SEND;
}

void ClientChat::pollMessages()
{
	Json::Value req_body;
	req_body["session_token"] = m_session_token;
	req_body["since_id"] = (Json::UInt64)m_last_message_id;

	HTTPFetchRequest req;
	req.url = getServerUrl() + "/api/poll";
	req.caller = m_http_caller;
	req.request_id = REQUEST_MSG_POLL;
	req.method = HTTP_POST;
	req.raw_data = fastWriteJson(req_body);
	req.extra_headers.emplace_back("Content-Type: application/json");
	req.timeout = 8000;
	req.connect_timeout = 8000;
	httpfetch_async(req);
	m_request_pending = true;
	m_pending_request_id = REQUEST_MSG_POLL;
}

static bool parseJson(const std::string &body, Json::Value &root)
{
	Json::CharReaderBuilder builder;
	builder.settings_["collectComments"] = false;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	std::string errs;
	return reader->parse(body.data(), body.data() + body.size(), &root, &errs);
}

void ClientChat::handleResult(u64 request_id, bool succeeded, const std::string &body)
{
	if (!succeeded) {
		static bool warned_once = false;
		if (!warned_once) {
			warned_once = true;
			warningstream << "ClientChat: HTTP request to \"" << getServerUrl()
				<< "\" failed. This won't be logged again this session."
				<< std::endl;
		}
		m_last_error = "Connection to ClientChat server failed";
		// A failed poll/send shouldn't drop the session -- just try
		// again next interval. Only pairing gives up (see the
		// expires_in countdown in step()).
		return;
	}

	switch (request_id) {
	case REQUEST_PAIR:
		handlePairResponse(body);
		break;
	case REQUEST_PAIR_POLL:
		handlePairPollResponse(body);
		break;
	case REQUEST_SEND:
		handleSendResponse(body);
		break;
	case REQUEST_MSG_POLL:
		handleMessagePollResponse(body);
		break;
	case REQUEST_LOGOUT:
		// Fire-and-forget (see logout() above) -- nothing to do with
		// the result either way.
		break;
	default:
		break;
	}
}

void ClientChat::handlePairResponse(const std::string &body)
{
	Json::Value root;
	if (!parseJson(body, root) || !root.isObject()) {
		m_auth_state = AuthState::LoggedOut;
		m_last_error = "ClientChat server sent back something unexpected";
		return;
	}
	m_pair_code = root.get("code", "").asString();
	m_poll_token = root.get("poll_token", "").asString();
	m_verify_url = root.get("verify_url", "").asString();
	m_pair_expires_in = (float)root.get("expires_in", 300).asInt();
	if (m_pair_code.empty() || m_poll_token.empty()) {
		m_auth_state = AuthState::LoggedOut;
		m_last_error = "ClientChat server didn't return a pairing code";
	}
}

void ClientChat::handlePairPollResponse(const std::string &body)
{
	Json::Value root;
	if (!parseJson(body, root) || !root.isObject())
		return; // Transient hiccup -- just try again next interval.

	if (!root.get("linked", false).asBool())
		return;

	m_session_token = root.get("session_token", "").asString();
	m_discord_id = root.get("discord_id", "").asString();
	m_discord_name = root.get("discord_name", "").asString();
	if (m_session_token.empty()) {
		m_auth_state = AuthState::LoggedOut;
		m_last_error = "ClientChat server confirmed pairing but sent no session";
		return;
	}

	m_auth_state = AuthState::LoggedIn;
	m_last_error.clear();
	saveCachedSession();
}

void ClientChat::handleSendResponse(const std::string &body)
{
	Json::Value root;
	if (!parseJson(body, root) || !root.isObject())
		return;
	if (!root.get("ok", false).asBool())
		m_last_error = root.get("error", "ClientChat message failed to send").asString();
}

void ClientChat::handleMessagePollResponse(const std::string &body)
{
	Json::Value root;
	if (!parseJson(body, root) || !root.isObject())
		return;

	// A 401-ish "your session isn't valid anymore" from the server --
	// distinct from a transient network failure, this one really does
	// mean logged out (e.g. the player revoked access on Discord's side,
	// or the server's session store was reset).
	if (root.isMember("error") && root["error"].asString() == "invalid_session") {
		logout();
		m_last_error = "ClientChat session expired, please log in again";
		return;
	}

	const Json::Value &messages = root["messages"];
	if (messages.isArray()) {
		for (const Json::Value &m : messages) {
			Message msg;
			msg.id = m.get("id", 0).asUInt64();
			msg.from_id = m.get("from_id", "").asString();
			msg.from_name = m.get("from_name", "?").asString();
			msg.to_id = m.get("to_id", "").asString();
			msg.text = m.get("text", "").asString();
			msg.timestamp = m.get("ts", 0).asUInt64();
			msg.is_private = !msg.to_id.empty();

			if (msg.id > m_last_message_id)
				m_last_message_id = msg.id;

			m_messages.push_back(msg);
		}
		// Keep the in-memory scrollback bounded -- this is a live chat
		// window, not a full history browser.
		while (m_messages.size() > 500)
			m_messages.pop_front();
	}

	const Json::Value &online = root["online"];
	if (online.isArray()) {
		m_online_users.clear();
		for (const Json::Value &u : online) {
			if (u.isObject()) {
				m_online_users.emplace_back(
					u.get("id", "").asString(), u.get("name", "?").asString());
			}
		}
	}
}
