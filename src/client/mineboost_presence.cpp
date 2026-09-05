// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 presence system

#include "client/mineboost_presence.h"
#include "httpfetch.h"
#include "settings.h"
#include "convert_json.h"
#include "porting.h"
#include "log.h"
#include <json/json.h>
#include <memory>
#include <algorithm>

MineBoostPresence &MineBoostPresence::get()
{
	static MineBoostPresence instance;
	return instance;
}

MineBoostPresence::~MineBoostPresence()
{
	// Also cancels any in-flight request for this caller -- see the
	// warning on httpfetch_caller_free() in httpfetch.h ("can be
	// expensive"), but this only ever runs once, at process exit
	// (MineBoostPresence is a function-local static, see get() above),
	// so that's not a concern here.
	if (m_http_caller != 0)
		httpfetch_caller_free(m_http_caller);
}

void MineBoostPresence::noteSeen(const std::string &name, unsigned long long now_ms)
{
	if (name.empty())
		return;
	m_last_seen_ms[name] = now_ms;
}

bool MineBoostPresence::isMineBoostUser(const std::string &name, unsigned long long now_ms) const
{
	auto it = m_last_seen_ms.find(name);
	if (it == m_last_seen_ms.end())
		return false;
	return (now_ms - it->second) <= EXPIRY_MS;
}

void MineBoostPresence::reset()
{
	m_last_seen_ms.clear();
}

void MineBoostPresence::step(float dtime, const std::string &own_name)
{
	std::string url = g_settings->get("mineboost_presence_server_url");
	if (url.empty())
		return;

	// Drain whatever the last request came back with before considering
	// firing a new one, regardless of whether the timer's due yet -- the
	// response could otherwise sit in the queue for up to a full
	// HTTP_POLL_INTERVAL_SECONDS before being picked up.
	if (m_http_caller != 0) {
		HTTPFetchResult result;
		while (httpfetch_async_get(m_http_caller, result)) {
			m_http_request_pending = false;
			// result.succeeded only means curl completed the HTTP
			// transaction -- it's still true for a 404/500/etc, so a
			// dead or misconfigured route would otherwise sail straight
			// into handleServerResponse() below and get logged there as
			// "not a JSON array" instead of what it actually is: an HTTP
			// error. Treat any non-2xx the same as a hard failure.
			bool http_ok = result.succeeded && result.response_code >= 200 &&
				result.response_code < 300;
			if (http_ok) {
				m_consecutive_failures = 0;
				handleServerResponse(result.data);
			} else {
				m_consecutive_failures++;
				static bool warned_once = false;
				if (!warned_once) {
					warned_once = true;
					warningstream << "MineBoostPresence: HTTP presence server "
						"request to \"" << url << "\" failed (code "
						<< result.response_code << "); badges from it will "
						"be stale until a request succeeds. Backing off to "
						"avoid hammering it every " << HTTP_POLL_INTERVAL_SECONDS
						<< "s while it's down. This won't be logged again "
						"this session." << std::endl;
				}
			}
		}
	}

	// One request in flight at a time -- no point queuing more before
	// the last one's even come back.
	if (m_http_request_pending)
		return;

	m_http_timer += dtime;
	// Doubles per consecutive failure (15s, 30s, 60s, ... capped at
	// HTTP_POLL_INTERVAL_MAX_SECONDS) so a dead/sleeping presence server
	// gets polled far less often than every 15s, but still gets retried
	// eventually in case it comes back.
	float interval = HTTP_POLL_INTERVAL_SECONDS;
	for (int i = 0; i < m_consecutive_failures && interval < HTTP_POLL_INTERVAL_MAX_SECONDS; i++)
		interval *= 2.0f;
	interval = std::min(interval, HTTP_POLL_INTERVAL_MAX_SECONDS);
	if (m_http_timer < interval)
		return;
	m_http_timer = 0.0f;

	if (own_name.empty())
		return;

	if (m_http_caller == 0)
		m_http_caller = httpfetch_caller_alloc();

	Json::Value req_body;
	req_body["name"] = own_name;

	HTTPFetchRequest req;
	req.url = url + "/presence";
	req.caller = m_http_caller;
	req.method = HTTP_POST;
	req.raw_data = fastWriteJson(req_body);
	req.extra_headers.emplace_back("Content-Type: application/json");
	// Generous but bounded -- a slow/asleep free-tier host (e.g. Replit
	// spinning up from idle) shouldn't hang this longer than a couple of
	// poll intervals' worth, and a stuck request would otherwise block
	// every future poll forever (see the "one request in flight" check
	// above).
	req.timeout = 8000;
	req.connect_timeout = 8000;
	httpfetch_async(req);
	m_http_request_pending = true;
}

void MineBoostPresence::handleServerResponse(const std::string &body)
{
	Json::Value root;
	{
		Json::CharReaderBuilder builder;
		builder.settings_["collectComments"] = false;
		const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
		std::string errs;
		if (!reader->parse(body.data(), body.data() + body.size(), &root, &errs)
				|| !root.isArray()) {
			static bool warned_once = false;
			if (!warned_once) {
				warned_once = true;
				warningstream << "MineBoostPresence: HTTP presence server at \""
					<< g_settings->get("mineboost_presence_server_url")
					<< "\" returned something that isn't a JSON array of "
					"player names; ignoring it. This won't be logged again "
					"this session." << std::endl;
			}
			return;
		}
	}

	unsigned long long now_ms = porting::getTimeMs();
	for (const Json::Value &entry : root) {
		if (entry.isString())
			noteSeen(entry.asString(), now_ms);
	}
}
