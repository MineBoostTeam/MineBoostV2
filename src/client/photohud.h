// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 PhotoHUD -- ground-up rewrite (see the task this was
// written for; the previous implementation, both the on-screen element
// and its settings panel, has been fully replaced, not patched).
//
// This is the actual on-screen HUD element: a small user-chosen image
// (one of 5 built-in photos, or a custom file from disk) drawn over the
// game/GUI. Hud::drawPhotoHud() (src/client/hud.cpp) is now just a thin
// forwarding call into PhotoHud::draw() below -- every bit of PhotoHUD's
// own state and behavior lives in this class instead of being inlined
// into Hud's own code, so it can be reasoned about (and extended)
// completely independently of the other ~15 HUD elements Hud owns, and
// independently of unrelated systems like ClientChat.
//
// The settings *menu* (the picker panel opened from the GUI category) is
// a separate class -- see src/gui/custom_menu/PhotoHudSettingsMenu.h/.cpp
// -- which only ever writes to g_settings; it has no reference to this
// class or vice versa. The two stay in sync purely through g_settings's
// own change-notification mechanism (see init() below), the same way any
// other independent piece of MineBoostV2 reacts to a setting changing
// out from under it (hand-edited minetest.conf, a script, ...).
//
// Design principles this class is built around (see the task this was
// written for):
//  - Cheap every frame: draw() does zero settings reads, zero filesystem
//    access, and zero texture-source lookups. All of that only happens
//    inside refreshFromSettings(), which only runs at init() and again
//    whenever one of the settings this cares about actually changes (via
//    the g_settings callback below) -- not once per frame regardless of
//    whether anything changed.
//  - Never crashes on bad/missing configuration: every settings read
//    goes through exists()-guarded local helpers (see photohud.cpp) with
//    a sane fallback, never a throwing g_settings->get()/getBool()/
//    getS32() directly, and custom-image loading is wrapped in try/catch
//    with every failure mode (empty path, missing file, directory,
//    decode failure) treated as "draw nothing" rather than propagating.
//  - No dangling references: the one non-owned resource this caches
//    across frames -- a video::ITexture* for the current image -- is
//    either owned by ITextureSource (the 5 built-ins; never released
//    here, since other code may reference the same named texture) or
//    loaded directly via IVideoDriver::getTexture() for a custom path
//    (this class's own responsibility to release via
//    IVideoDriver::removeTexture(), same convention already used for
//    MineBoostPresence's screenshot texture -- see
//    m_rp_screenshot_texture in hud.h/.cpp). See releaseCustomTexture().

#pragma once

#include "irrlichttypes_extrabloated.h"
#include <array>
#include <string>

class ITextureSource;
namespace irr { namespace video { class IVideoDriver; class ITexture; } }

// One entry per built-in photo (textures/base/pack/*.png) -- the single
// source of truth for "what built-in photos exist", shared between this
// class (resolving the currently-selected one to an actual texture) and
// PhotoHudSettingsMenu (building one picker button per entry). Adding a
// 6th built-in photo later is exactly one line here; nothing else needs
// to change.
struct PhotoHudBuiltinImage
{
	// Value stored in the "photo_hud_image" setting for this image.
	const char *settings_key;
	// Filename under textures/base/pack/.
	const char *texture_filename;
	// Label for this image's picker button in the settings menu.
	const wchar_t *button_label;
};

// Number of built-in photos -- kept as its own named constant (rather
// than callers doing PhotoHudBuiltinImages.size()) so it's usable
// wherever a compile-time array size is needed (e.g.
// PhotoHudSettingsMenu.h's std::array<Button, N> of picker buttons)
// without relying on evaluating the size of an `extern`, not-yet-defined
// array at that point.
inline constexpr std::size_t PhotoHudBuiltinImageCount = 5;

extern const std::array<PhotoHudBuiltinImage, PhotoHudBuiltinImageCount> PhotoHudBuiltinImages;

class PhotoHud
{
public:
	PhotoHud() = default;
	~PhotoHud();
	PhotoHud(const PhotoHud &) = delete;
	PhotoHud &operator=(const PhotoHud &) = delete;

	// Must be called exactly once, after `driver`/`tsrc` both exist (see
	// Hud::Hud() in src/client/hud.cpp, which is the only caller) --
	// registers this instance for "photo_hud_*" settings-changed
	// callbacks and does the first refreshFromSettings(). Every other
	// method below is a safe no-op if called before init() (draw() just
	// won't draw anything; the destructor still cleans up correctly
	// either way).
	void init(irr::video::IVideoDriver *driver, ITextureSource *tsrc);

	// Draws the element if enabled and there's something to draw --
	// cheap, see the class comment above for exactly why. Called once
	// per frame from Hud::drawPhotoHud().
	//   screensize  current screen size, for centering when no saved
	//               position exists yet.
	//   gui_is_open whether some GUI (inventory, chest, pause menu,
	//               MineBoost settings, ...) is currently open -- see
	//               Hud::isMenuActive().
	// Reads the shared "hud_size" global HUD-scale slider itself (a
	// cheap settings lookup, not filesystem/texture work, so this
	// doesn't need caching -- same live-read pattern every other
	// MineBoost HUD element's own draw function already uses, see e.g.
	// Hud::drawMusicHud() in src/client/hud.cpp) rather than depending
	// on Hud's *different*, vanilla-Luanti m_hud_scaling member.
	void draw(const irr::core::dimension2du &screensize, bool gui_is_open) const;

private:
	static void onSettingChanged(const std::string &name, void *data);
	void refreshFromSettings();
	void releaseCustomTexture();

	irr::video::IVideoDriver *m_driver = nullptr;
	ITextureSource *m_tsrc = nullptr;
	bool m_initialized = false;

	// ---- Cached state -- only ever written by refreshFromSettings() ----
	bool m_enabled = false;
	bool m_show_in_game = false;
	s32 m_size = 200;
	s32 m_pos_x = -1;
	s32 m_pos_y = -1;

	// Resolved texture for whatever "photo_hud_image" (+
	// "photo_hud_custom_path" if that's "custom") currently says.
	// nullptr means "nothing to draw" -- missing/invalid custom file, no
	// texture source available, etc. -- draw() treats that as a normal,
	// silent no-op, not an error.
	irr::video::ITexture *m_texture = nullptr;
	// True only when m_texture was loaded directly via
	// IVideoDriver::getTexture(path) (a custom image) rather than
	// ITextureSource::getTexture(name) (one of the 5 built-ins) -- see
	// releaseCustomTexture()'s comment for why this distinction matters.
	bool m_texture_is_custom = false;
};
