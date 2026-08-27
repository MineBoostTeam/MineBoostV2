// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 PhotoHUD settings menu -- ground-up rewrite (see the task
// this was written for). This is the picker panel opened by right-
// clicking the "PhotoHUD" tile in Menu's GUI category; it is a pure
// *view* over g_settings -- every button here does nothing but read or
// write a "photo_hud_*" setting. It has no reference to (and no
// awareness of) PhotoHud, the class that actually draws the HUD element
// (src/client/photohud.h/.cpp) -- the two stay in sync purely through
// g_settings's own change-notification mechanism, which PhotoHud
// subscribes to independently. This is a deliberate architectural split
// (view vs. the thing the view configures) rather than the previous
// version's tighter coupling.
//
// See photohud.h's PhotoHudBuiltinImages table for the single shared
// list of built-in photos -- this class builds one picker button per
// entry from that table (std::array<Button, N>, not N hardcoded named
// members), so adding a 6th built-in photo later needs no changes here
// at all.
//
// Same crash-safety principles as everywhere else in this GUI: every
// settings read goes through exists()-guarded local helpers with a sane
// fallback, and the one operation here that can plausibly fail in a way
// this class didn't anticipate (loading/validating a custom image path)
// is wrapped in try/catch and turned into a status message the player
// sees, never propagated.

#pragma once

#include <irrlicht.h>
#include "IGUIEnvironment.h"
#include "IGUIEditBox.h"
#include "IGUIFont.h"
#include "IVideoDriver.h"
#include "irrlichttypes_extrabloated.h"
#include "Button.h"
#include "client/photohud.h" // PhotoHudBuiltinImages
#include <array>
#include <string>

class Client;

using namespace irr;

class PhotoHudSettingsMenu
{
public:
	PhotoHudSettingsMenu() = default;

	// Must be called once, right after construction, before open()/
	// onEvent()/draw() are used -- mirrors Menu's own constructor
	// parameters: this panel needs the same environment/parent to create
	// its edit box in, and the same Client to read a live preview
	// texture from (may be nullptr -- see the title-screen Menu instance
	// in src/client/clientlauncher.cpp; the preview just stays blank
	// without a Client).
	void init(gui::IGUIEnvironment *environment, gui::IGUIElement *parent, Client *client);

	bool isOpen() const { return m_open; }

	// Opens the panel: shows all its buttons/edit box, (re)computes their
	// layout against the current screen size, and refreshes everything
	// from the currently-saved settings. Safe to call again while
	// already open (just re-syncs). Does not do anything about any
	// *other* panel that might also be open -- that's the caller's job
	// (see Menu::openHandViewSettings()/the PhotoHUD tile's right-click
	// handler in Menu.cpp, which keep this mutually exclusive with the
	// HandView panel).
	void open();

	// Hides everything and drops Irrlicht focus back to `parent`. Safe
	// to call even if already closed (a no-op).
	void close();

	// Feed every raw input event to this while isOpen(). Returns true
	// unconditionally whenever the panel is open, same convention as
	// every other MineBoost advanced-settings panel (see
	// Button::isPressed()'s comment on why panels need to agree on this).
	bool onEvent(const irr::SEvent &event);

	// Draws the panel's background, labels, live preview, and buttons.
	// Only actually draws anything while isOpen().
	void draw(video::IVideoDriver *driver, gui::IGUIFont *font);

	// Fixed, centered panel rect this (and the HandView panel, which
	// intentionally shares the same footprint -- see
	// Menu::getHandViewSettingsPanelRect()) is laid out against.
	core::rect<s32> getPanelRect() const;

private:
	void layout();
	void applyCustomPath();
	// Resolves whatever's currently selected to a texture purely for
	// this panel's own live preview -- independent of (and not shared
	// with) PhotoHud's own resolution/caching in photohud.cpp, since
	// this only needs to run while the panel is actually open/visible,
	// not every frame regardless.
	video::ITexture *resolvePreviewTexture(video::IVideoDriver *driver) const;

	gui::IGUIEnvironment *m_env = nullptr;
	gui::IGUIElement *m_parent = nullptr;
	Client *m_client = nullptr;

	bool m_open = false;

	Button m_close_button;
	// One button per src/client/photohud.h's PhotoHudBuiltinImages entry
	// -- see the file comment above for why this is a table instead of
	// individually-named members.
	std::array<Button, PhotoHudBuiltinImageCount> m_pick_buttons;
	Button m_use_custom_button;
	Button m_show_in_game_button;

	// Created lazily on the first open() (needs m_env/m_parent from
	// init()), reused on every later open -- never destroyed manually,
	// its Irrlicht parent owns it, same convention used for scrollbars
	// etc. elsewhere in this GUI.
	gui::IGUIEditBox *m_custom_path_input = nullptr;

	// Feedback from the last "Use" click -- shown under the path field
	// for a few seconds so a bad path doesn't just silently do nothing.
	// Empty = nothing to show.
	std::string m_status;
	u64 m_status_expire_ms = 0;
};
