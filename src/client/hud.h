// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>
// Copyright (C) 2017 red-001 <red-001@outlook.ie>

#pragma once

#include <vector>
#include <IGUIFont.h>
#include <SMaterial.h>
#include <SMeshBuffer.h>
#include "irr_ptr.h"
#include "irr_aabb3d.h"
#include "../hud.h"
#include "nowplaying.h"
#include "photohud.h"

class Client;
class ITextureSource;
class Inventory;
class InventoryList;
class LocalPlayer;
struct ItemStack;

namespace irr::scene
{
	class IMesh;
}

namespace irr::video
{
	class ITexture;
	class IVideoDriver;
}

class Hud
{
public:
	enum BlockBoundsMode
	{
		BLOCK_BOUNDS_OFF,
		BLOCK_BOUNDS_CURRENT,
		BLOCK_BOUNDS_NEAR,
	} m_block_bounds_mode = BLOCK_BOUNDS_OFF;

	video::SColor crosshair_argb;
	video::SColor selectionbox_argb;

	bool use_crosshair_image = false;
	bool use_object_crosshair_image = false;
	std::string hotbar_image = "";
	bool use_hotbar_image = false;
	std::string hotbar_selected_image = "";
	bool use_hotbar_selected_image = false;

	bool pointing_at_object = false;
	bool target_is_player = false;

	// TargetHUD: small name+HP panel shown only while directly aiming at
	// a player (unlike target_is_player above, which also stays true for
	// the locked-target-but-not-aimed-at case used for particles).
	bool target_hud_active = false;
	std::string target_hud_name;
	std::string target_hud_skin; // skin texture name, used for the face avatar
	u16 target_hud_hp = 0;
	u16 target_hud_hp_max = 20;

	// Macro Wheel: radial menu shown while the wheel key (default Tab)
	// is held. Game::processMacroWheel() (src/client/game.cpp) owns the
	// open/select logic and just writes these two; drawMacroWheel()
	// below only reads them (plus MacroList) to render.
	bool macro_wheel_open = false;
	int macro_wheel_selected = 0;

	Hud(Client *client, LocalPlayer *player,
			Inventory *inventory);
	void readScalingSetting();
	~Hud();

	enum BlockBoundsMode toggleBlockBounds();
	void disableBlockBounds();
	void drawBlockBounds();

	void drawHotbar(const v2s32 &pos, const v2f &offset, u16 direction, const v2f &align);
	void resizeHotbar();
	void drawCrosshair();
	void drawSelectionMesh();
	void drawTargetHud();
	void drawMusicHud();
	void drawShowRp();
	void drawConsumptionHud();
	void drawPhotoHud();
	void drawCoordsHud();
	void drawFpsHud();
	void drawPingHud();
	void drawKeyStrokerHud();
	void drawCpsHud();
	void drawMacroWheel();
	void drawInventoryHud();
	void drawCraftHud();
	// void drawArmorHud(); // ArmorHUD temporarily disabled
	void updateSelectionMesh(const v3s16 &camera_offset);

	std::vector<aabb3f> *getSelectionBoxes() { return &m_selection_boxes; }

	void setSelectionPos(const v3f &pos, const v3s16 &camera_offset);

	v3f getSelectionPos() const { return m_selection_pos; }

	void setSelectionRotation(v3f rotation) { m_selection_rotation = rotation; }

	v3f getSelectionRotation() const { return m_selection_rotation; }

	void setSelectionMeshColor(const video::SColor &color)
	{
		m_selection_mesh_color = color;
	}

	void setSelectedFaceNormal(const v3f &face_normal)
	{
		m_selected_face_normal = face_normal;
	}

	bool hasElementOfType(HudElementType type);

	void drawLuaElements(const v3s16 &camera_offset);

private:
	bool calculateScreenPos(const v3s16 &camera_offset, HudElement *e, v2s32 *pos);
	NowPlayingProvider m_now_playing;
	video::ITexture *m_music_thumbnail_texture = nullptr;
	unsigned long long m_music_thumbnail_id = 0;
	void updateMusicThumbnail(const NowPlayingInfo &info);
	// Cache for drawMusicHud()'s utf8_to_wide() conversions of
	// info.source/title/artist -- NowPlayingProvider::poll() already only
	// refreshes that data ~once/second, but without this the 3 conversions
	// (plus the "[source]" concatenation) were redone from scratch every
	// single rendered frame regardless, for text that's almost always
	// identical to the previous frame's. Keyed on the plain (non-wide)
	// strings actually read from `info`, not the wide/formatted output.
	std::string m_music_wcache_source, m_music_wcache_title, m_music_wcache_artist;
	std::wstring m_music_wline1, m_music_wline2, m_music_wline3;

	// ShowRP ("show_rp") -- displays the currently active texture pack's
	// own screenshot.png/texture_pack.conf (title, author), the same
	// metadata/convention ContentDB packages and the main menu's content
	// browser use (see load_texture_packs() in
	// builtin/mainmenu/content/pkgmgr.lua). Re-read only when
	// "texture_path" changes, not every frame -- see drawShowRp() in
	// src/client/hud.cpp.
	std::string m_rp_cached_texture_path;
	bool m_rp_active = false;
	std::string m_rp_title;
	std::string m_rp_author;
	video::ITexture *m_rp_screenshot_texture = nullptr;
	void drawStatbar(v2s32 pos, u16 corner, u16 drawdir,
			const std::string &texture, const std::string& bgtexture,
			s32 count, s32 maxcount, v2s32 offset, v2s32 size = v2s32());

	void drawItems(v2s32 screen_pos, v2s32 screen_offset, s32 itemcount, v2f alignment,
			s32 inv_offset, InventoryList *mainlist, u16 selectitem,
			u16 direction, bool is_hotbar);

	void drawItem(const ItemStack &item, const core::rect<s32> &rect, bool selected,
			bool draw_slot_bg = true);

	void drawCompassTranslate(HudElement *e, video::ITexture *texture,
			const core::rect<s32> &rect, int way);

	void drawCompassRotate(HudElement *e, video::ITexture *texture,
			const core::rect<s32> &rect, int way);

	Client *client = nullptr;
	video::IVideoDriver *driver = nullptr;
	LocalPlayer *player = nullptr;
	Inventory *inventory = nullptr;
	ITextureSource *tsrc = nullptr;

	// See src/client/photohud.h -- owns all of PhotoHUD's own state,
	// initialized in Hud::Hud() once driver/tsrc above are set.
	PhotoHud m_photo_hud;

	// FPS smoothing for drawFpsHud() (src/client/hud.cpp) -- this class
	// doesn't have access to GameUI's own RunStats::dtime_jitter (see
	// the comment on that in drawFpsHud()), so it tracks its own simple
	// exponential moving average of frame-to-frame time instead.
	u64 m_fps_last_time_ms = 0;
	float m_fps_smoothed = 0.0f;

	// LMB/RMB clicks-per-second tracking for drawCpsHud() (src/client/
	// hud.cpp) -- same rising-edge-detect-then-reset-every-1s algorithm
	// builtin/client/keystroker.lua's track_lmb_clicks()/
	// track_rmb_clicks() used, now done natively instead (see the
	// comment on that in drawCpsHud()).
	u64 m_cps_last_time_ms = 0;
	bool m_cps_lmb_was_down = false;
	bool m_cps_rmb_was_down = false;
	int m_cps_lmb_clicks = 0;
	int m_cps_rmb_clicks = 0;
	float m_cps_lmb_timer = 0.0f;
	float m_cps_rmb_timer = 0.0f;

	float m_hud_scaling; // cached minetest setting
	float m_scale_factor;
	v3s16 m_camera_offset;
	v2u32 m_screensize;
	v2s32 m_displaycenter;
	s32 m_hotbar_imagesize; // Takes hud_scaling into account, updated by resizeHotbar()
	s32 m_padding; // Takes hud_scaling into account, updated by resizeHotbar()
	video::SColor hbar_colors[4];

	std::vector<aabb3f> m_selection_boxes;
	std::vector<aabb3f> m_halo_boxes;
	v3f m_selection_pos;
	v3f m_selection_pos_with_offset;
	v3f m_selection_rotation;

	scene::IMesh *m_selection_mesh = nullptr;
	video::SColor m_selection_mesh_color;
	v3f m_selected_face_normal;

	video::SMaterial m_selection_material;
	video::SMaterial m_block_bounds_material;

	irr_ptr<scene::SMeshBuffer> m_rotation_mesh_buffer;

	enum
	{
		HIGHLIGHT_BOX,
		HIGHLIGHT_HALO,
		HIGHLIGHT_NONE
	} m_mode;
};

enum ItemRotationKind
{
	IT_ROT_SELECTED,
	IT_ROT_HOVERED,
	IT_ROT_DRAGGED,
	IT_ROT_OTHER,
	IT_ROT_NONE, // Must be last, also serves as number
};

void drawItemStack(video::IVideoDriver *driver,
		gui::IGUIFont *font,
		const ItemStack &item,
		const core::rect<s32> &rect,
		const core::rect<s32> *clip,
		Client *client,
		ItemRotationKind rotation_kind);

void drawItemStack(
		video::IVideoDriver *driver,
		gui::IGUIFont *font,
		const ItemStack &item,
		const core::rect<s32> &rect,
		const core::rect<s32> *clip,
		Client *client,
		ItemRotationKind rotation_kind,
		const v3s16 &angle,
		const v3s16 &rotation_speed);

