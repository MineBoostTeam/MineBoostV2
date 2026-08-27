// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 ClientChat window -- see src/client/clientchat.h for the
// networking/auth side this displays.

#include "gui/guiClientChat.h"

#include <IGUIEditBox.h>
#include "client/clientchat.h"
#include "client/fontengine.h"
#include "client/renderingengine.h"
#include "gui/custom_menu/ModernUI.h"
#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/string.h"

GUIClientChat *GUIClientChat::s_instance = nullptr;

GUIClientChat::GUIClientChat(gui::IGUIEnvironment *env, gui::IGUIElement *parent,
		s32 id, IMenuManager *menumgr):
	IGUIElement(gui::EGUIET_ELEMENT, env, parent, id, core::rect<s32>(0, 0, 0, 0)),
	m_menumgr(menumgr)
{
	s_instance = this;

	m_login_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Login with Discord");
	m_login_button.setColor(video::SColor(220, 40, 60, 220));
	m_login_button.setOnClick([this]() { openDiscordLink(); });
	m_login_button.setVisible(false);

	m_logout_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Log out");
	m_logout_button.setColor(video::SColor(180, 20, 20, 20));
	m_logout_button.setOnClick([]() { ClientChat::get().logout(); });
	m_logout_button.setVisible(false);

	m_close_button.addButton(core::rect<s32>(0, 0, 10, 10), L"X");
	m_close_button.setColor(video::SColor(180, 20, 20, 20));
	m_close_button.setOnClick([this]() { closeChat(); });
	m_close_button.setVisible(false);
}

GUIClientChat::~GUIClientChat()
{
	if (s_instance == this)
		s_instance = nullptr;
}

core::rect<s32> GUIClientChat::getPanelRect() const
{
	s32 sw = Environment->getVideoDriver()->getScreenSize().Width;
	s32 sh = Environment->getVideoDriver()->getScreenSize().Height;
	s32 w = 720, h = 480;
	// Both axes centered on the current screen size, recomputed fresh on
	// every call (not cached) -- "clientchat_y" used to pin this near the
	// top of the screen instead (default "1"), with nothing anywhere
	// actually writing to that setting to move it, so in practice it was
	// just permanently stuck there rather than centered.
	s32 x = (sw - w) / 2;
	s32 y = (sh - h) / 2;
	return core::rect<s32>(x, y, x + w, y + h);
}

void GUIClientChat::layout()
{
	core::rect<s32> panel = getPanelRect();

	m_close_button.addButton(
		core::rect<s32>(panel.LowerRightCorner.X - 34, panel.UpperLeftCorner.Y + 6,
			panel.LowerRightCorner.X - 6, panel.UpperLeftCorner.Y + 30),
		L"X");

	m_logout_button.addButton(
		core::rect<s32>(panel.LowerRightCorner.X - 120, panel.UpperLeftCorner.Y + 6,
			panel.LowerRightCorner.X - 40, panel.UpperLeftCorner.Y + 30),
		L"Log out");

	m_login_button.addButton(
		core::rect<s32>(panel.UpperLeftCorner.X + (panel.getWidth() - 220) / 2,
			panel.UpperLeftCorner.Y + panel.getHeight() / 2 + 20,
			panel.UpperLeftCorner.X + (panel.getWidth() - 220) / 2 + 220,
			panel.UpperLeftCorner.Y + panel.getHeight() / 2 + 52),
		L"Login with Discord");

	core::rect<s32> input_rect(panel.UpperLeftCorner.X + 12, panel.LowerRightCorner.Y - 40,
		panel.LowerRightCorner.X - 12, panel.LowerRightCorner.Y - 12);
	if (!m_input) {
		m_input = Environment->addEditBox(L"", input_rect, true, this, -1);
	} else {
		m_input->setRelativePosition(input_rect);
	}

	// This element itself had a zero-size rect (0,0,0,0) from the
	// constructor -- meaning Irrlicht's own click hit-testing
	// (CGUIEnvironment::updateHoveredElement(), rect-based) could never
	// actually land on it, so every click silently stole focus away to
	// whatever real element WAS under the cursor instead (see
	// EFF_SET_ON_LMOUSE_DOWN handling in postEventFromUser() --
	// Hovered != Focus on every click when Hovered can never be this).
	// That's what made every button in this window unresponsive and
	// closing it impossible. Sized to the full screen (not just the
	// visible panel) so every click while open gets caught regardless of
	// where it lands -- OnEvent() below already swallows every click
	// inside getPanelRect() and no-ops everywhere else, so full coverage
	// here doesn't change that behavior. Kept anchored at (0,0)
	// specifically: every child rect in this file (m_input, the
	// buttons) is computed in absolute screen coordinates, which only
	// lines up correctly because this element's own origin is (0,0) --
	// moving it would shift every child's position by the same delta.
	setRelativePosition(core::rect<s32>(0, 0,
		(s32)Environment->getVideoDriver()->getScreenSize().Width,
		(s32)Environment->getVideoDriver()->getScreenSize().Height));
}

void GUIClientChat::openChat()
{
	if (m_open)
		return;
	m_open = true;
	layout();
	IGUIElement::setVisible(true);
	m_close_button.setVisible(true);
	m_menumgr->createdMenu(this);
	// Focus stays on `this`, NOT m_input -- CGUIEnvironment::
	// postEventFromUser() only ever calls whichever single element
	// currently holds focus (or Hovered, but only as a fallback when
	// Focus is null); it never bubbles events up to a parent. Focusing
	// the edit box directly meant every click/Escape/etc. went straight
	// to it and never reached GUIClientChat::OnEvent() at all -- nothing
	// in the window was clickable and it couldn't be closed. Text input
	// is now forwarded to m_input manually from OnEvent() below instead.
	Environment->setFocus(this);
}

void GUIClientChat::closeChat()
{
	if (!m_open)
		return;
	m_open = false;
	IGUIElement::setVisible(false);
	m_login_button.setVisible(false);
	m_logout_button.setVisible(false);
	m_close_button.setVisible(false);
	Environment->removeFocus(m_input);
	Environment->removeFocus(this);
	m_menumgr->deletingMenu(this);
}

void GUIClientChat::openDiscordLink()
{
	// try/catch is defense in depth on top of getDiscordUrl()'s own
	// exists() guard and porting::open_url()'s own bool-return (never-
	// throws) contract -- belt and suspenders specifically because this
	// runs directly from a button click handler with nothing upstream to
	// catch an exception (see Button::isPressed() in src/gui/custom_menu/
	// Button.cpp, which now also guards every button's callback the same
	// way, this one included -- kept here too since this is the one place
	// that actually knows how to turn a failure into a message the player
	// can see via m_link_status, rather than just logging it).
	try {
		const std::string url = ClientChat::get().getDiscordUrl();
		if (url.empty()) {
			m_link_status = L"\"clientchat_discord_url\" isn't set.";
			return;
		}

		// porting::open_url() itself already validates the URL has a
		// http(s) schema and never throws -- it just logs and returns
		// false on any failure (missing schema, no default browser
		// handler registered, posix_spawnp()/ShellExecuteA() failing,
		// ...). Same function every other external link in this engine
		// uses already (guiChatConsole.cpp, guiHyperText.cpp,
		// guiFormSpecMenu.cpp), covering Windows (ShellExecuteA), Android
		// (via porting_android.cpp's JNI call into the launcher
		// activity), Linux (xdg-open), and macOS (open) -- nothing
		// MineBoost-specific to add here.
		if (porting::open_url(url)) {
			m_link_status = L"Opened in your browser.";
		} else {
			m_link_status = L"Couldn't open a browser automatically -- visit: "
				+ utf8_to_wide(url);
		}
	} catch (const std::exception &e) {
		errorstream << "[MineBoost] openDiscordLink() failed: " << e.what() << std::endl;
		m_link_status = L"Couldn't open the Discord link (see the log).";
	} catch (...) {
		errorstream << "[MineBoost] openDiscordLink() failed with a "
			"non-std::exception" << std::endl;
		m_link_status = L"Couldn't open the Discord link (see the log).";
	}
}

void GUIClientChat::submitInput()
{
	if (!m_input)
		return;
	std::string text = wide_to_utf8(m_input->getText());
	m_input->setText(L"");
	if (text.empty())
		return;

	// "/addfriend <discord id>" / "/removefriend <discord id>" -- by id
	// specifically, not name (see the file comment in clientchat.h: names
	// can change, ids don't, and it works even for someone who isn't
	// online right now). The id is whatever's shown next to their name in
	// the online list below -- not implemented as a click-to-copy there on
	// purpose, to keep that list simple; typing a 17-20 digit number once
	// is a small one-time cost for a friend you'll see highighted from
	// then on.
	if (text.size() > 11 && text.compare(0, 11, "/addfriend ") == 0) {
		ClientChat::get().addFriendById(text.substr(11));
		return;
	}
	if (text.size() > 14 && text.compare(0, 14, "/removefriend ") == 0) {
		ClientChat::get().removeFriendById(text.substr(14));
		return;
	}

	// "/msg <name> <text>" sends a private message instead of a public
	// one -- <name> is matched against the online-users list
	// (ClientChat::getOnlineUsers(), case-insensitive) to resolve it to
	// the Discord id the server actually needs (see "to_id" in
	// ClientChat::sendMessage()). No point-and-click recipient picker on
	// purpose -- this keeps the window simple, and typing a name is no
	// slower than clicking one out of a list.
	if (text.size() > 5 && text.compare(0, 5, "/msg ") == 0) {
		std::string rest = text.substr(5);
		size_t space = rest.find(' ');
		if (space == std::string::npos || space == 0)
			return;
		std::string target_name = rest.substr(0, space);
		std::string body = rest.substr(space + 1);
		if (body.empty())
			return;

		std::string target_name_lower = lowercase(target_name);
		for (const auto &user : ClientChat::get().getOnlineUsers()) {
			if (lowercase(user.second) == target_name_lower) {
				ClientChat::get().sendMessage(body, user.first);
				return;
			}
		}
		// No match -- silently dropped rather than shown as a fake
		// system message, since this window doesn't have its own local-
		// only message type to show it as. Typing a bad name once and
		// getting no reply is a clear enough signal.
		return;
	}

	ClientChat::get().sendMessage(text);
}

bool GUIClientChat::OnEvent(const irr::SEvent &event)
{
	if (!m_open)
		return Parent ? Parent->OnEvent(event) : false;

	if (event.EventType == irr::EET_KEY_INPUT_EVENT && event.KeyInput.PressedDown) {
		if (event.KeyInput.Key == irr::KEY_ESCAPE) {
			closeChat();
			return true;
		}
		if (event.KeyInput.Key == irr::KEY_RETURN) {
			submitInput();
			return true;
		}
		// Everything else (typing, backspace, arrow keys, ...) goes to
		// the input box's own OnEvent -- it's a real IGUIEditBox for its
		// text-editing logic/cursor, but `this` (not it) holds Irrlicht
		// focus (see openChat() above), so it never receives events on
		// its own. Calling its OnEvent directly works fine regardless of
		// which element originally "had" the event.
		if (m_input)
			m_input->OnEvent(event);
		return true;
	}

	if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
		if (event.MouseInput.Event == irr::EMIE_MOUSE_WHEEL) {
			m_scroll_offset += event.MouseInput.Wheel > 0 ? 1 : -1;
			if (m_scroll_offset < 0)
				m_scroll_offset = 0;
			return true;
		}

		if (m_close_button.isPressed(event))
			return true;
		if (ClientChat::get().getAuthState() == ClientChat::AuthState::LoggedOut &&
				m_login_button.isPressed(event))
			return true;
		if (ClientChat::get().getAuthState() == ClientChat::AuthState::LoggedIn &&
				m_logout_button.isPressed(event))
			return true;

		// Clicks landing on the input box (placing the cursor, selecting
		// text, ...) -- same "this holds focus, forward manually" reasoning
		// as the keyboard branch above.
		if (m_input && m_input->getAbsolutePosition().isPointInside(
				core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
			m_input->OnEvent(event);
			return true;
		}

		// Swallow every other click inside the panel so it doesn't fall
		// through to the game world underneath (attacking/placing while
		// clicking around in this window).
		if (getPanelRect().isPointInside(
				core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)))
			return true;
	}

	return true;
}

void GUIClientChat::draw()
{
	if (!m_open)
		return;

	video::IVideoDriver *driver = Environment->getVideoDriver();
	gui::IGUIFont *font = g_fontengine->getFont();
	if (!font)
		return;

	core::rect<s32> panel = getPanelRect();
	ModernUI::panel(driver, panel, ModernUI::Radius,
		video::SColor(235, 18, 18, 24), ModernUI::PanelBorder, /*shadow=*/false);

	const s32 pad = 12;
	const s32 line_h = font->getDimension(L"Ay").Height + 4;

	font->draw(L"ClientChat", core::rect<s32>(panel.UpperLeftCorner.X + pad,
		panel.UpperLeftCorner.Y + 8, panel.LowerRightCorner.X - 140,
		panel.UpperLeftCorner.Y + 32), video::SColor(255, 255, 255, 255));

	m_close_button.draw(driver);

	ClientChat &cc = ClientChat::get();

	if (!cc.isServerConfigured()) {
		font->draw(L"\"clientchat_server_url\" isn't set.",
			core::rect<s32>(panel.UpperLeftCorner.X + pad, panel.UpperLeftCorner.Y + 60,
				panel.LowerRightCorner.X - pad, panel.UpperLeftCorner.Y + 84),
			video::SColor(255, 220, 120, 120));
		return;
	}

	switch (cc.getAuthState()) {
	case ClientChat::AuthState::LoggedOut: {
		m_login_button.setVisible(true);
		m_logout_button.setVisible(false);
		m_login_button.draw(driver);

		// m_link_status (set by openDiscordLink() above) reports the
		// outcome of the last click -- whether the browser actually
		// opened, not anything about being "logged in" (this button no
		// longer performs any login of its own -- see openDiscordLink()).
		std::wstring status = m_link_status.empty() ?
			L"Click below to join the MineBoostV2 Discord." : m_link_status;
		core::rect<s32> status_rect(panel.UpperLeftCorner.X,
			panel.UpperLeftCorner.Y + panel.getHeight() / 2 - 20,
			panel.LowerRightCorner.X,
			panel.UpperLeftCorner.Y + panel.getHeight() / 2 + 4);
		font->draw(status.c_str(), status_rect, video::SColor(255, 190, 190, 190),
			true, true);
		break;
	}

	case ClientChat::AuthState::Pairing: {
		m_login_button.setVisible(false);
		m_logout_button.setVisible(false);

		std::wstring line1 = L"Go to:";
		std::wstring line2 = utf8_to_wide(cc.getVerifyUrl());
		std::wstring line3 = L"and log in with Discord. Waiting for confirmation...";

		s32 y = panel.UpperLeftCorner.Y + panel.getHeight() / 2 - 60;
		core::rect<s32> r1(panel.UpperLeftCorner.X, y, panel.LowerRightCorner.X, y + 24);
		font->draw(line1.c_str(), r1, video::SColor(255, 190, 190, 190), true, true);

		y += 28;
		core::rect<s32> r2(panel.UpperLeftCorner.X, y, panel.LowerRightCorner.X, y + 24);
		font->draw(line2.c_str(), r2, video::SColor(255, 120, 180, 255), true, true);

		y += 40;
		core::rect<s32> r3(panel.UpperLeftCorner.X, y, panel.LowerRightCorner.X, y + 24);
		font->draw(line3.c_str(), r3, video::SColor(255, 150, 150, 150), true, true);
		break;
	}

	case ClientChat::AuthState::LoggedIn: {
		m_login_button.setVisible(false);
		m_logout_button.setVisible(true);
		m_logout_button.draw(driver);

		std::wstring header = L"Logged in as " + utf8_to_wide(cc.getLoggedInName());
		font->draw(header.c_str(), core::rect<s32>(panel.UpperLeftCorner.X + pad,
			panel.UpperLeftCorner.Y + 36, panel.LowerRightCorner.X - 130,
			panel.UpperLeftCorner.Y + 36 + line_h),
			video::SColor(255, 170, 220, 170));

		// Online list. Friends (added via "/addfriend <discord id>" --
		// see submitInput() above) are shown in the same green FriendESP
		// uses (src/client/camera.cpp), matched by id, not name.
		s32 online_y = panel.UpperLeftCorner.Y + 36 + line_h;
		s32 online_x = panel.UpperLeftCorner.X + pad;
		const s32 online_right = panel.LowerRightCorner.X - pad;
		const video::SColor online_label_color(255, 140, 140, 160);
		const video::SColor online_friend_color(255, 60, 230, 110);

		core::dimension2d<u32> label_size = font->getDimension(L"Online: ");
		font->draw(L"Online: ", core::rect<s32>(online_x, online_y,
			online_x + (s32)label_size.Width, online_y + line_h), online_label_color);
		online_x += (s32)label_size.Width;

		const auto &users = cc.getOnlineUsers();
		if (users.empty()) {
			font->draw(L"(just you)", core::rect<s32>(online_x, online_y,
				online_right, online_y + line_h), online_label_color);
		} else {
			for (size_t i = 0; i < users.size(); i++) {
				std::wstring name = utf8_to_wide(users[i].second);
				bool is_friend = cc.isFriendId(users[i].first);
				video::SColor color = is_friend ? online_friend_color : online_label_color;

				core::dimension2d<u32> name_size = font->getDimension(name.c_str());
				if (online_x + (s32)name_size.Width > online_right)
					break; // Ran out of room on this line -- just truncate.
				font->draw(name.c_str(), core::rect<s32>(online_x, online_y,
					online_x + (s32)name_size.Width, online_y + line_h), color);
				online_x += (s32)name_size.Width;

				if (i + 1 < users.size()) {
					core::dimension2d<u32> sep_size = font->getDimension(L", ");
					font->draw(L", ", core::rect<s32>(online_x, online_y,
						online_x + (s32)sep_size.Width, online_y + line_h),
						online_label_color);
					online_x += (s32)sep_size.Width;
				}
			}
		}

		// Message scrollback.
		s32 list_top = panel.UpperLeftCorner.Y + 36 + line_h * 2 + 8;
		s32 list_bottom = panel.LowerRightCorner.Y - 52;
		s32 visible_lines = std::max(1, (list_bottom - list_top) / line_h);

		const auto &messages = cc.getMessages();
		int total = (int)messages.size();
		int end = total - m_scroll_offset;
		if (end > total)
			end = total;
		if (end < 0)
			end = 0;
		int start = std::max(0, end - visible_lines);

		s32 y = list_top;
		for (int i = start; i < end; i++) {
			const ClientChat::Message &m = messages[i];
			bool from_me = m.from_id == cc.getLoggedInName() || m.from_id.empty();
			video::SColor color = m.is_private ?
				video::SColor(255, 230, 170, 230) : video::SColor(255, 210, 210, 210);
			std::wstring prefix = m.is_private ?
				(from_me ? L"[DM -> " : L"[DM] ") : L"";
			std::wstring line = prefix + utf8_to_wide(m.from_name) + L": "
				+ utf8_to_wide(m.text);
			core::rect<s32> line_rect(panel.UpperLeftCorner.X + pad, y,
				panel.LowerRightCorner.X - pad, y + line_h);
			font->draw(line.c_str(), line_rect, color, false, false, &line_rect);
			y += line_h;
		}

		const wchar_t *hint = L"Enter to send -- \"/msg <name> <text>\", "
			L"\"/addfriend <discord id>\"";
		font->draw(hint, core::rect<s32>(panel.UpperLeftCorner.X + pad,
			panel.LowerRightCorner.Y - 58, panel.LowerRightCorner.X - pad,
			panel.LowerRightCorner.Y - 42),
			video::SColor(200, 140, 140, 140));
		break;
	}
	}

	if (m_input)
		m_input->setVisible(cc.getAuthState() == ClientChat::AuthState::LoggedIn);
}
