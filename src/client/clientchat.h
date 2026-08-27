// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 ClientChat
//
// A global, cross-server chat channel for MineBoostV2 players, completely
// independent of whatever Luanti server the player happens to be connected
// to at the moment -- it never touches that server's own chat/network
// protocol at all, so a server admin has no way to see it (same reasoning
// as MineBoostPresence's HTTP method, see mineboost_presence.h). Backed by
// a small HTTP service of the player's own choosing
// ("clientchat_server_url", e.g. a Node app on Replit), reused for both
// login and messaging.
//
// Login is a "device code" flow -- the same shape Steam/YouTube-on-TV/etc.
// use for devices that can't easily run a full OAuth browser redirect
// in-process:
//
//   1. POST <url>/api/pair {} ->
//        {"code":"AB12CD","poll_token":"<uuid>","verify_url":"...","expires_in":300}
//      The code/verify_url are shown in the ClientChat window (see
//      src/gui/guiClientChat.h). The player opens verify_url in a real
//      browser and logs in with Discord there -- entirely server-side,
//      this client never sees a Discord token or handles OAuth itself.
//   2. POST <url>/api/pair/poll {"poll_token":"..."} , repeated every
//      PAIR_POLL_INTERVAL_SECONDS until the server reports the code was
//      confirmed:
//        not yet: {"linked":false}
//        done:    {"linked":true,"session_token":"...","discord_id":"...","discord_name":"..."}
//      session_token is cached in g_settings ("clientchat_session_token",
//      not exposed in settingtypes.txt/the settings menu -- same
//      not-a-real-setting treatment as the hud_color_* ones) so this only
//      needs to happen once per Discord account, not once per launch.
//
// Once logged in:
//   3. POST <url>/api/send
//        {"session_token":"...","player_name":"<mt name>","text":"...","to_id":null|"<discord id>"}
//      to_id omitted/null = public message to every logged-in
//      MineBoostV2 player; set = private message to that one Discord
//      user specifically (see sendMessage() below).
//   4. POST <url>/api/poll {"session_token":"...","since_id":<last seen>} ,
//      repeated every MSG_POLL_INTERVAL_SECONDS:
//        {"messages":[{"id":1,"from_id":"...","from_name":"...",
//                       "to_id":null|"...","text":"...","ts":169...}, ...],
//         "online":["Name1","Name2"]}
//      "online" is every other currently-logged-in player's Discord
//      display name, shown in the ClientChat window's sidebar.
//
// ClientChat friends (separate from the FriendESP friend list in
// src/client/friendlist.h, which is keyed by Minetest player name):
// added by Discord user id specifically (see addFriendById() below), not
// by name -- Discord display names can change, ids don't, and the online
// list this class tracks is keyed by id anyway (see getOnlineUsers()).
// Persisted in g_settings ("clientchat_friend_ids", comma-separated),
// same not-a-real-setting treatment as the session cache above.
//
// See tools/clientchat-server-example/ for a reference implementation of
// this whole protocol (Node/Express + Discord OAuth2).

#pragma once

#include <deque>
#include <string>
#include <vector>

#include "irrlichttypes.h"

class ClientChat
{
public:
	static ClientChat &get();
	~ClientChat();

	struct Message
	{
		u64 id = 0;
		std::string from_id;
		std::string from_name;
		std::string to_id;   // empty = public
		std::string text;
		u64 timestamp = 0;   // unix seconds, server-supplied
		bool is_private = false;
	};

	enum class AuthState
	{
		LoggedOut,
		Pairing,
		LoggedIn,
	};

	// Call every client step (see Client::step() in src/client/client.cpp).
	// player_name is only used for the "from_name" the server records on
	// outgoing messages if it doesn't already know a Discord display name
	// -- purely cosmetic, never used for authentication.
	void step(float dtime, const std::string &player_name);

	// Begins the device-code login flow (see the file comment above).
	// No-op if already Pairing or LoggedIn. Not currently wired to any
	// button -- see getDiscordUrl()/isDiscordUrlConfigured() above and
	// GUIClientChat::openDiscordLink() in src/gui/guiClientChat.cpp for
	// what "Login with Discord" actually does now (just opens a link, no
	// request to clientchat_server_url involved). Kept here as the
	// server-side protocol this class still speaks for LoggedIn-gated
	// messaging (sendMessage() etc.) once a session exists.
	void startLogin();

	// Forgets the cached session (both in memory and in g_settings) and
	// tells the server to invalidate it. Does not block on the server
	// round-trip -- the local session is dropped immediately either way.
	void logout();

	AuthState getAuthState() const { return m_auth_state; }
	const std::string &getPairCode() const { return m_pair_code; }
	const std::string &getVerifyUrl() const { return m_verify_url; }
	const std::string &getLoggedInName() const { return m_discord_name; }
	const std::string &getLastError() const { return m_last_error; }

	// Queues a message to send on the next available request slot (see
	// m_request_pending in the .cpp -- only one ClientChat HTTP request is
	// ever in flight at a time, same as MineBoostPresence). to_id empty =
	// public message; otherwise a private message to that Discord user
	// id (see the "to_id" entries in getOnlineUsers() below).
	void sendMessage(const std::string &text, const std::string &to_id = "");

	const std::deque<Message> &getMessages() const { return m_messages; }

	// {discord_id, discord_display_name} for every other player currently
	// logged into ClientChat, as of the last successful poll -- for a
	// "who's online" sidebar and DM-target picking in the GUI.
	const std::vector<std::pair<std::string, std::string>> &getOnlineUsers() const
	{
		return m_online_users;
	}

	bool isServerConfigured() const { return !getServerUrl().empty(); }

	// Static URL opened by the "Login with Discord" button (see
	// GUIClientChat::openDiscordLink() in src/gui/guiClientChat.cpp) --
	// e.g. a Discord invite, or a login page on the ClientChat server
	// itself. Deliberately just a plain setting, not fetched from the
	// server: opening it is a one-way, no-round-trip action (porting::
	// open_url()), unlike the device-code pairing flow below, which was
	// what used to run when this button was clicked and could hang the
	// window if "clientchat_server_url" was slow/unreachable/moved.
	std::string getDiscordUrl() const;
	bool isDiscordUrlConfigured() const { return !getDiscordUrl().empty(); }

	// ClientChat friends -- see the file comment above for why this is
	// id-based and separate from FriendList (src/client/friendlist.h).
	void addFriendById(const std::string &discord_id);
	void removeFriendById(const std::string &discord_id);
	bool isFriendId(const std::string &discord_id) const;
	const std::vector<std::string> &getFriendIds() const { return m_friend_ids; }

private:
	std::string getServerUrl() const;

	void startPairRequest();
	void pollPairStatus();
	void pollMessages();
	void flushSendQueue();

	void handleResult(u64 request_id, bool succeeded, const std::string &body);
	void handlePairResponse(const std::string &body);
	void handlePairPollResponse(const std::string &body);
	void handleMessagePollResponse(const std::string &body);
	void handleSendResponse(const std::string &body);

	void loadCachedSession();
	void saveCachedSession();
	void clearCachedSession();

	void loadFriendIds();
	void saveFriendIds();

	AuthState m_auth_state = AuthState::LoggedOut;
	std::string m_pair_code;
	std::string m_poll_token;
	std::string m_verify_url;
	float m_pair_expires_in = 0.0f;

	std::string m_session_token;
	std::string m_discord_id;
	std::string m_discord_name;

	std::string m_last_error;

	float m_pair_poll_timer = 0.0f;
	float m_msg_poll_timer = 0.0f;
	u64 m_last_message_id = 0;

	std::deque<Message> m_messages;
	std::vector<std::pair<std::string, std::string>> m_online_users;
	std::vector<std::string> m_friend_ids;

	struct QueuedMessage
	{
		std::string text;
		std::string to_id;
	};
	std::deque<QueuedMessage> m_send_queue;

	std::string m_player_name;

	u64 m_http_caller = 0;
	bool m_request_pending = false;
	u64 m_pending_request_id = 0;

	static constexpr float PAIR_POLL_INTERVAL_SECONDS = 3.0f;
	static constexpr float MSG_POLL_INTERVAL_SECONDS = 2.5f;

	// request_id tags so handleResult() knows which endpoint a given
	// httpfetch_async_get() result belongs to (see the .cpp) -- this
	// class never has more than one request in flight, but still needs
	// to tell apart what kind of request that one was.
	static constexpr u64 REQUEST_PAIR = 1;
	static constexpr u64 REQUEST_PAIR_POLL = 2;
	static constexpr u64 REQUEST_SEND = 3;
	static constexpr u64 REQUEST_MSG_POLL = 4;
	static constexpr u64 REQUEST_LOGOUT = 5;
};
