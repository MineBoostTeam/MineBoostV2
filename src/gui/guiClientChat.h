// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 ClientChat window -- see src/client/clientchat.h for the
// networking/auth side this displays.

#pragma once

#include <irrlicht.h>
#include "gui/modalMenu.h"
#include "gui/custom_menu/Button.h"

using namespace irr;

class GUIClientChat : public gui::IGUIElement
{
public:
	GUIClientChat(gui::IGUIEnvironment *env, gui::IGUIElement *parent, s32 id,
		IMenuManager *menumgr);
	~GUIClientChat();

	// Opens/closes the window. Unlike src/gui/custom_menu/Menu.cpp's
	// settings menu, this never needs to be reachable from the title
	// screen, so it doesn't need that class's raw-input-receiver keybind
	// trick -- toggled from the ordinary in-game wasKeyDown(KeyType::
	// CLIENTCHAT) polling in Game::processKeyInput() (src/client/game.cpp),
	// same as the settings menu's own in-game keybind.
	void openChat();
	void closeChat();
	bool isChatOpen() const { return m_open; }

	virtual void draw();
	virtual bool OnEvent(const irr::SEvent &event);

	// Explicit, unconditional forwarding entry point -- called directly
	// from MyEventReceiver::OnEvent() in src/client/inputhandler.cpp, the
	// same proven pattern Menu::checkGlobalBinds() in
	// src/gui/custom_menu/Menu.h/.cpp already uses for the exact same
	// reason: don't rely on Irrlicht's own Focus-based
	// CGUIEnvironment::postEventFromUser() dispatch reaching this window.
	// Returns true if the event was consumed (window is open), so the
	// caller can short-circuit immediately instead of also letting the
	// event fall through to the normal dispatch chain and risk
	// double-handling the same click.
	static bool forwardEvent(const irr::SEvent &event)
	{
		return s_instance && s_instance->m_open && s_instance->OnEvent(event);
	}

private:
	// There's only ever one GUIClientChat instance (created once in
	// Game::Game(), src/client/game.cpp), same as Menu::s_instance.
	static GUIClientChat *s_instance;
	core::rect<s32> getPanelRect() const;
	void submitInput();
	void layout();

	// "Login with Discord" now just opens ClientChat::getDiscordUrl() in
	// the player's real browser (porting::open_url(), the same function
	// every other external link in this engine already uses -- see
	// guiChatConsole.cpp/guiHyperText.cpp/guiFormSpecMenu.cpp) instead of
	// calling ClientChat::startLogin(), which used to kick off the
	// in-client device-code pairing HTTP request (POST .../api/pair, see
	// src/client/clientchat.h/.cpp) directly from the click handler. That
	// request has no timeout margin for a slow/unreachable/moved
	// clientchat_server_url (see e.g. a Replit app's domain changing) and
	// nothing about it needs to block the click -- opening a link doesn't
	// need any of that round-trip. Sets m_link_status with the outcome for
	// draw() to show, exactly like the old Pairing-state status line did.
	void openDiscordLink();
	std::wstring m_link_status;

	IMenuManager *m_menumgr;
	bool m_open = false;

	gui::IGUIEditBox *m_input = nullptr;
	Button m_login_button;
	Button m_logout_button;
	Button m_close_button;

	// Simple auto-scroll: how many of the most recent messages are
	// skipped from the top of the visible list. 0 = pinned to the
	// newest message (the common case -- only changes if the player
	// scrolls up to read back).
	int m_scroll_offset = 0;
};
