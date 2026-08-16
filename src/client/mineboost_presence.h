// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 presence system
//
// Lets MineBoostV2 clients recognize each other on the same server, so
// nametags can show a small badge next to players who are also using
// MineBoostV2 (see Camera::drawMineBoostBadges()).
//
// Two independent detection methods feed the same badge, and either one
// alone is enough to light it up for a given player:
//
// 1. Mod-channel heartbeat (see CHANNEL/HEARTBEAT_INTERVAL_SECONDS
//    below). Needs no server-side mod: it piggybacks on the engine's
//    built-in mod-channel relay (TOSERVER_MODCHANNEL_JOIN/MSG), which
//    any Luanti server can relay purely at the C++ engine level,
//    regardless of what Lua mods are installed. The one requirement is
//    that the server admin has `enable_mod_channels = true` in
//    minetest.conf -- it defaults to false, so this silently does
//    nothing (no badges shown, no errors) on servers where it isn't
//    enabled. Only sees other MineBoostV2 clients on the SAME server.
//
// 2. HTTP presence server (see reportToServer() below). Points at a
//    URL of the player's own choosing ("mineboost_presence_server_url"
//    in minetest.conf) -- e.g. a small custom backend the player runs
//    themselves (Replit or otherwise). Every
//    HTTP_POLL_INTERVAL_SECONDS, POSTs {"name": "<own player name>"} to
//    "<url>/presence" and expects the response body to be a JSON array
//    of every player name that service currently considers online, e.g.
//    ["Alice","Bob"]. Every name in that array gets noteSeen() the same
//    as a mod-channel heartbeat would. Off by default (empty URL); does
//    nothing until the player sets that setting. Unlike the mod-channel
//    method, this isn't tied to any particular Luanti server -- it's
//    whatever the backend at that URL decides "online" means, so it's
//    the player's own responsibility to keep that URL trustworthy (this
//    stays badge-only either way: nothing here changes any gameplay
//    behavior).

#pragma once

#include <string>
#include <unordered_map>

#include "irrlichttypes.h"

class MineBoostPresence
{
public:
	static MineBoostPresence &get();
	~MineBoostPresence();

	// Dedicated mod channel used purely as a presence heartbeat; the
	// message content doesn't matter; hearing anything at all on this
	// channel from a given sender is itself the signal.
	static constexpr const char *CHANNEL = "mineboostv2:presence";

	// How often each client (re-)broadcasts its heartbeat.
	static constexpr float HEARTBEAT_INTERVAL_SECONDS = 5.0f;

	// How often reportToServer() below fires a new HTTP request when
	// things are working. Longer than the mod-channel interval since
	// this is a real network round-trip to a server of the player's own
	// choosing, not just an engine-relayed message. Backs off from here
	// (see m_consecutive_failures below) if the server keeps failing, so
	// a dead/misconfigured/sleeping presence server doesn't get hit
	// every 15s forever.
	static constexpr float HTTP_POLL_INTERVAL_SECONDS = 15.0f;
	static constexpr float HTTP_POLL_INTERVAL_MAX_SECONDS = 300.0f;

	// How long a badge keeps showing after the last heartbeat, before
	// we assume that player disconnected or switched clients. Several
	// multiples of HEARTBEAT_INTERVAL_SECONDS so a couple of dropped
	// packets don't flicker the badge on and off.
	static constexpr unsigned long long EXPIRY_MS = 20000;

	// Called when a heartbeat is received from another client on CHANNEL.
	void noteSeen(const std::string &name, unsigned long long now_ms);

	// True if we've heard from this name recently enough to still trust it.
	bool isMineBoostUser(const std::string &name, unsigned long long now_ms) const;

	// Cheap check for "is there any point scanning active objects for
	// badges at all" -- lets Camera::drawMineBoostBadges() skip its
	// per-frame ActiveObjectMgr::getAllActiveObjects() scan entirely
	// (which rebuilds a fresh unordered_map of every loaded entity/
	// player every single frame) on the very common case of nobody
	// having been seen yet: presence disabled, mod-channel not enabled
	// server-side, or simply no other MineBoost players nearby. Doesn't
	// account for individual entries having expired past EXPIRY_MS --
	// that's still checked per-player by isMineBoostUser() -- so this
	// can occasionally be true for a few extra seconds after the last
	// player with a badge leaves, which is harmless (just one wasted
	// scan that finds nothing to draw).
	bool empty() const { return m_last_seen_ms.empty(); }

	// Forgets everyone. Called on disconnect/reconnect so a badge seen
	// on one server can't linger into the next one.
	void reset();

	// Call every client step (see Client::step() in
	// src/client/client.cpp). own_name is the local player's name, sent
	// as this client's own heartbeat; harmless to call even if
	// "mineboost_presence_server_url" is unset (it just does nothing
	// every call in that case, one cheap settings lookup).
	void step(float dtime, const std::string &own_name);

private:
	MineBoostPresence() = default;

	// Parses a JSON array of player-name strings from an HTTP presence
	// server response body and noteSeen()s each one. Malformed/
	// unexpected responses (not an array of strings, or plain unparsable
	// junk) are logged once and otherwise ignored -- a flaky or
	// misbehaving presence server should never be able to do more than
	// leave badges stale, not crash or spam the log every poll.
	void handleServerResponse(const std::string &body);

	std::unordered_map<std::string, unsigned long long> m_last_seen_ms;

	float m_http_timer = 0.0f;
	u64 m_http_caller = 0;
	bool m_http_request_pending = false;
	// Consecutive failed polls (HTTP error, non-2xx response, or
	// malformed body) since the last success. Doubles the effective poll
	// interval per failure, capped at HTTP_POLL_INTERVAL_MAX_SECONDS --
	// see step() in mineboost_presence.cpp.
	int m_consecutive_failures = 0;
};
