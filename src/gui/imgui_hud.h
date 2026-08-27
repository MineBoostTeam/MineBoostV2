// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- ImGui-based rewrite of several "GUI" section HUD
// elements (see the task this was written for). Ground-up rewrite of
// the equivalent Hud::drawX() Irrlicht drawing code in src/client/
// hud.cpp for: NowPlaying (MusicHUD), ShowRP, ConsumptionHUD, TargetHUD,
// InventoryHUD, CraftHUD.
//
// Known limitation, InventoryHUD/CraftHUD only: item slots are drawn as
// a flat 2D icon (ItemCellState::icon_texture, via the same texture
// bridge NowPlaying/ShowRP use for their images -- see
// imgui_texture_bridge.h) using ItemStack::getInventoryImage(), same as
// the vast majority of items already show in the vanilla inventory
// formspec. The old Irrlicht drawItemStack() (still used by the
// vanilla formspec and the hotbar -- see drawItems()/drawItem() further
// up in hud.cpp) has a *second* code path for items with animations
// enabled or no flat inventory image at all: rendering an actual 3D
// wielditem mesh into the slot (camera/projection/view matrices, mesh
// buffers, the works). That's a real 3D scene render, not something a
// 2D immediate-mode GUI library has any way to do -- ImGui has no
// concept of a mesh, camera, or viewport. Items that would hit that
// path here just show no icon in an ImGui-drawn slot (nothing crashes;
// the slot square/count/wear-bar-if-applicable/text still draw
// normally) rather than attempting to fake or skip the mesh render.
// Genuinely uncommon in practice (most items -- all plain nodes, tools,
// etc. -- have a flat inventory image) but real, and worth knowing
// about rather than discovering by noticing a blank slot.
//
// Split into two phases, not one draw() call, because of when each part
// of the data is actually available: Hud::drawMusicHud()/drawShowRp()/
// drawConsumptionHud() (still the entry points -- see hud.cpp) already
// gather everything needed (poll NowPlaying, load/cache the texture-pack
// screenshot, read PerfMonitor stats, ...) at the point in the frame
// IrrlichtMt's own 2D HUD pass runs, which is *before* ImGuiManager::
// renderFrame() (src/gui/ImGuiManager.cpp) calls ImGui::NewFrame() for
// this frame -- and no ImGui:: function can be called before NewFrame()
// (see the crash ImGuiMineBoostMenu::checkGlobalBinds() hit from
// exactly this ordering hazard, now fixed -- same class of bug this
// two-phase split avoids by construction rather than another ad hoc
// guard). So: updateX() is called from the corresponding Hud::drawX()
// wrapper (unchanged timing), just stores what was gathered instead of
// drawing it; render() is called once from ImGuiManager::renderFrame(),
// after NewFrame(), and does the actual ImGui::Begin()/Image()/etc.
// calls using whatever was stored most recently.
//
// A singleton (same convention as ImGuiManager/ImGuiMineBoostMenu) since
// there's always at most one active Hud, and this needs to be reachable
// from ImGuiManager::renderFrame() without Hud needing to be plumbed
// through to it.
#pragma once

#include "irrlichttypes_extrabloated.h"
#include "imgui_texture_bridge.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace irr { namespace video { class ITexture; } }

class ImGuiHud
{
public:
	static ImGuiHud &get();

	struct MusicHudState
	{
		bool visible = false;
		std::string source, title, artist;
		irr::video::ITexture *art_texture = nullptr;
		bool has_progress = false;
		int position_seconds = 0;
		int duration_seconds = 0;
		float hud_size = 1.0f;
	};
	void updateMusicHud(const MusicHudState &state) { m_music = state; }

	struct ShowRpState
	{
		bool visible = false;
		std::string title, author;
		irr::video::ITexture *screenshot_texture = nullptr;
		float hud_size = 1.0f;
	};
	void updateShowRp(const ShowRpState &state) { m_rp = state; }

	struct ConsumptionHudState
	{
		bool visible = false;
		std::wstring text;
		float hud_size = 1.0f;
	};
	void updateConsumptionHud(const ConsumptionHudState &state) { m_consumption = state; }

	// Coords/FPS/Ping -- previously drawn as background-panel-only here
	// (GameUI, src/client/gameui.cpp, drew the actual text via its own
	// Irrlicht IGUIStaticText elements). Now fully self-contained: text
	// generation moved into the corresponding Hud::drawXHud() wrapper
	// (src/client/hud.cpp), and GameUI's text elements are permanently
	// kept empty (see the comment on that in gameui.cpp) rather than
	// drawing anything themselves, so this is the only thing actually
	// showing these three now.
	struct SimpleTextHudState
	{
		bool visible = false;
		std::string text;
		float hud_size = 1.0f;
		std::string color_setting;
		// Position storage: either one combined v2f setting
		// (pos_setting, combined_v2f=true) or two separate S32 settings
		// (x_setting/y_setting, combined_v2f=false) -- has to match
		// whichever format "Move HUD" edit mode already uses for this
		// element (see kDraggableElements in src/gui/custom_menu/
		// ImGuiMineBoostMenu.cpp), or dragging would silently write to a
		// setting this never reads. Coords/FPS/Ping all use the combined
		// v2f format (same "coords_sprite"/"fov_coords"/"ping_coords"
		// settings from before this rewrite).
		bool combined_v2f = false;
		std::string pos_setting;
		std::string x_setting, y_setting;
		float default_y_offset = 0.0f; // stacks Coords/FPS/Ping above each other by default, see renderSimpleTextHud()
	};
	void updateCoordsHud(const SimpleTextHudState &state) { m_coords = state; }
	void updateFpsHud(const SimpleTextHudState &state) { m_fps = state; }
	void updatePingHud(const SimpleTextHudState &state) { m_ping = state; }

	// KeyStroker -- previously a Lua-side HUD (builtin/client/
	// keystroker.lua) with hud.cpp only drawing the background panel
	// behind it. Now fully native: key state gathered directly from
	// LocalPlayer::getPlayerControl() (see Hud::drawKeyStrokerHud() in
	// src/client/hud.cpp) instead of Lua, which has had its own
	// equivalent HUD elements disabled (see the "if false then" wrapping
	// keystroker.lua's own hud_add() calls, with a comment explaining
	// why and how to restore it).
	struct KeyStrokerState
	{
		bool visible = false;
		bool up = false, down = false, left = false, right = false;
		bool jump = false, aux1 = false, sneak = false, dig = false, place = false;
		float hud_size = 1.0f;
	};
	void updateKeyStrokerHud(const KeyStrokerState &state) { m_keystroker = state; }

	// ShowCPS -- same story as KeyStroker above, including the LMB/RMB
	// click-per-second counting itself now happening natively (same
	// rising-edge-detect-then-reset-every-1s algorithm keystroker.lua's
	// track_lmb_clicks()/track_rmb_clicks() used).
	struct CpsState
	{
		bool visible = false;
		int lmb_cps = 0;
		int rmb_cps = 0;
		float hud_size = 1.0f;
	};
	void updateCpsHud(const CpsState &state) { m_cps = state; }

	struct TargetHudState
	{
		bool visible = false;
		std::string name;
		irr::video::ITexture *avatar_texture = nullptr;
		// Normalized (0..1) UV rects, precomputed by the gathering side
		// (Hud::drawTargetHud(), src/client/hud.cpp) since only it knows
		// the skin texture's own resolution -- see ImGui::Image()'s
		// uv0/uv1 parameters, which take normalized coordinates directly
		// rather than pixel rects the way IVideoDriver::draw2DImage()'s
		// source rect does.
		float avatar_uv0_x = 0, avatar_uv0_y = 0, avatar_uv1_x = 1, avatar_uv1_y = 1;
		bool avatar_has_overlay = false;
		float overlay_uv0_x = 0, overlay_uv0_y = 0, overlay_uv1_x = 1, overlay_uv1_y = 1;
		int hp = 0;
		int hp_max = 0;
		float hud_size = 1.0f;
	};
	void updateTargetHud(const TargetHudState &state) { m_target = state; }

	// One item slot, already resolved to a *flat 2D* icon (no 3D mesh
	// rendering -- see the note on this limitation in the class comment
	// below) -- shared between InventoryHud and CraftHud, which are
	// otherwise identical "grid of item sections" layouts (see
	// ItemGridState below).
	struct ItemCellState
	{
		irr::video::ITexture *icon_texture = nullptr; // nullptr = empty slot or no flat icon available
		irr::video::ITexture *overlay_texture = nullptr;
		std::string count_text; // empty = no count badge
		bool has_wear = false;
		float wear_fraction = 0.0f; // 0 = new, 1 = fully worn -- matches item.wear / 65535.0f
		unsigned int wear_color = 0; // ImU32, computed by the gathering side (matches the old drawItemStack()'s gradient)
	};
	struct ItemGridSection
	{
		std::string title;
		int cols = 0;
		int rows = 0;
		std::vector<ItemCellState> cells; // cells.size() may be < cols*rows for a partial last row
	};
	// InventoryHud and CraftHud share this exact shape -- see
	// renderItemGrid() in the .cpp, used by both.
	struct ItemGridState
	{
		bool visible = false;
		std::vector<ItemGridSection> sections;
		float hud_size = 1.0f;
		std::string color_setting; // "hud_color_inventory" or "hud_color_craft"
		std::string x_setting, y_setting;
		float default_x_offset = 0.0f; // added to the centered default X (CraftHud sits right of center; InventoryHud is exactly centered)
	};
	void updateInventoryHud(const ItemGridState &state) { m_inventory = state; }
	void updateCraftHud(const ItemGridState &state) { m_craft = state; }

	// See the class comment above for exactly when/why this needs to
	// run separately from the updateX() calls. A no-op (cheap, just a
	// handful of bool checks) for whichever of the 3 elements above are
	// currently disabled/have nothing to show.
	void render();

private:
	ImGuiHud() = default;
	ImGuiHud(const ImGuiHud &) = delete;
	ImGuiHud &operator=(const ImGuiHud &) = delete;

	void renderMusicHud();
	void renderShowRp();
	void renderConsumptionHud();
	void renderTargetHud();
	// Shared by Coords/FPS/Ping -- identical panel+single-line-of-text
	// layout, just different state/settings/default position.
	void renderSimpleTextHud(const char *canvas_id, const SimpleTextHudState &state);
	void renderKeyStrokerHud();
	void renderCpsHud();
	// Shared by InventoryHud and CraftHud (identical layout, see
	// ItemGridState above) -- `canvas_id` just needs to be distinct
	// between the two calls.
	void renderItemGrid(const char *canvas_id, const ItemGridState &state);

	// Item icons need one texture-cache slot *per distinct icon
	// currently visible* (unlike the single-image elements above, which
	// only ever show one image at a time) -- ITextureSource already
	// shares/caches textures by name, so multiple slots holding the same
	// item type share one ITexture* and thus one cache entry here too.
	// Never pruned: even a full inventory only has on the order of a few
	// dozen distinct item types, and IrrlichtMt's own ITextureSource
	// never evicts its cache either -- consistent, bounded, not worth
	// the complexity of tracking "still in use".
	void *getIconTextureId(irr::video::ITexture *tex, int *out_w, int *out_h);
	std::unordered_map<const irr::video::ITexture *, ImGuiTextureCache> m_icon_cache;

	SimpleTextHudState m_coords, m_fps, m_ping;
	KeyStrokerState m_keystroker;
	CpsState m_cps;
	MusicHudState m_music;
	ShowRpState m_rp;
	ConsumptionHudState m_consumption;
	TargetHudState m_target;
	ItemGridState m_inventory;
	ItemGridState m_craft;

	// One ImGuiTextureCache per single-image element -- see
	// imgui_texture_bridge.h for why each caller owns its own rather
	// than sharing a global cache.
	ImGuiTextureCache m_music_art_cache;
	ImGuiTextureCache m_rp_art_cache;
};
