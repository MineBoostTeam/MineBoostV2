// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 settings menu -- implementation. See ImGuiMineBoostMenu.h
// for the design notes this was built around.

#include "ImGuiMineBoostMenu.h"

#include "imgui.h"
#include "client/client.h"
#include "client/keycode.h"
#include "client/photohud.h"
#include "filesys.h"
#include "gui/ImGuiManager.h"
#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/string.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

// ---- Small safe-settings helpers (same exists()-guarded pattern used
// throughout MineBoostV2 -- see e.g. src/client/photohud.cpp) ----------

bool safeGetBool(const std::string &name, bool fallback)
{
	return g_settings->exists(name) ? g_settings->getBool(name) : fallback;
}

std::string safeGetString(const std::string &name, const std::string &fallback)
{
	return g_settings->exists(name) ? g_settings->get(name) : fallback;
}

float safeGetFloat(const std::string &name, float fallback)
{
	return g_settings->exists(name) ? g_settings->getFloat(name) : fallback;
}

s32 safeGetS32(const std::string &name, s32 fallback)
{
	return g_settings->exists(name) ? g_settings->getS32(name) : fallback;
}

// ---- Bind capture (converting a raw key/mouse event into a stored
// "TOKEN1,TOKEN2" bind string) -- same format/semantics the old Menu
// used, so existing saved binds keep working unchanged. ----------------

bool eventToBindToken(const irr::SEvent &event, std::string &token_out, bool &cancel_out)
{
	token_out.clear();
	cancel_out = false;

	if (event.EventType == irr::EET_KEY_INPUT_EVENT) {
		if (!event.KeyInput.PressedDown)
			return false;
		if (event.KeyInput.Key == irr::KEY_ESCAPE) {
			cancel_out = true;
			return true;
		}
		KeyPress kp(event.KeyInput, false);
		const char *sym = kp.sym();
		if (!sym || !sym[0])
			return false;
		token_out = sym;
		return true;
	}

	if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
		switch (event.MouseInput.Event) {
		case irr::EMIE_RMOUSE_PRESSED_DOWN:
			if (event.MouseInput.Shift) {
				// Shift+RMB is the dedicated "cancel/clear this bind"
				// gesture. Plain LMB/RMB used to cancel unconditionally
				// (matching plain Escape), which meant LMB and RMB could
				// never actually be captured as a bind token themselves --
				// clicking either one to "record" it just cancelled the
				// capture instead. Gating cancel behind Shift+RMB frees
				// plain LMB/RMB up to be bindable like every other mouse
				// button below.
				cancel_out = true;
				return true;
			}
			token_out = "MOUSE_RIGHT";
			return true;
		case irr::EMIE_LMOUSE_PRESSED_DOWN:
			token_out = "MOUSE_LEFT";
			return true;
		case irr::EMIE_MMOUSE_PRESSED_DOWN:
			token_out = "MOUSE_MIDDLE";
			return true;
		case irr::EMIE_XBUTTON1_PRESSED_DOWN:
			token_out = "MOUSE_X1";
			return true;
		case irr::EMIE_XBUTTON2_PRESSED_DOWN:
			token_out = "MOUSE_X2";
			return true;
		case irr::EMIE_MOUSE_WHEEL:
			token_out = event.MouseInput.Wheel > 0 ? "WHEEL_UP" : "WHEEL_DOWN";
			return true;
		default:
			return false;
		}
	}

	return false;
}

std::string bindTokenDisplayName(const std::string &token)
{
	if (token.empty())
		return "";
	if (token == "WHEEL_UP") return "MWheel Up";
	if (token == "WHEEL_DOWN") return "MWheel Down";
	if (token == "MOUSE_LEFT") return "LMB";
	if (token == "MOUSE_RIGHT") return "RMB";
	if (token == "MOUSE_MIDDLE") return "MMB";
	if (token == "MOUSE_X1") return "Mouse4";
	if (token == "MOUSE_X2") return "Mouse5";

	KeyPress kp(token.c_str());
	const char *nm = kp.name();
	return (nm && nm[0]) ? std::string(nm) : token;
}

void getBindTokens(const std::string &setting_name, std::string &slot1, std::string &slot2)
{
	std::string key = "bind_" + setting_name;
	std::string raw = safeGetString(key, "");
	size_t comma = raw.find(',');
	if (comma == std::string::npos) {
		slot1 = raw;
		slot2.clear();
	} else {
		slot1 = raw.substr(0, comma);
		slot2 = raw.substr(comma + 1);
	}
}

void setBindTokens(const std::string &setting_name, const std::string &slot1, const std::string &slot2)
{
	g_settings->set("bind_" + setting_name, slot1 + "," + slot2);
}

std::string getBindDisplayString(const std::string &setting_name)
{
	std::string s1, s2;
	getBindTokens(setting_name, s1, s2);
	if (s1.empty() && s2.empty())
		return "--";
	std::string out = bindTokenDisplayName(s1);
	if (!s2.empty())
		out += ", " + bindTokenDisplayName(s2);
	return out;
}

// ---- Settings table -- one row per toggle tile the old Menu drew,
// grouped into the same 3 categories (Scrollbars is handled separately,
// see drawScrollbarsSection() -- it's sliders, not toggles). Data-driven
// so adding a new toggle later is one line here, no other code changes.
// ------------------------------------------------------------------

enum class Category { GUI, RENDER, MISC };

struct SettingEntry
{
	const char *label;
	const char *setting;
	Category category;
};

constexpr std::array<SettingEntry, 22> kSettings = {{
	{"KeyStroker", "show_keys", Category::GUI},
	{"ShowCPS", "show_cps", Category::GUI},
	{"ShowCoords", "show_coords", Category::GUI},
	{"ShowFPS", "show_fps", Category::GUI},
	{"ShowPing", "show_ping", Category::GUI},
	{"NowPlaying", "music_hud", Category::GUI},
	{"ShowRP", "show_rp", Category::GUI},
	{"ConsumptionHUD", "consumption_hud", Category::GUI},
	{"InventoryHUD", "inventory_hud", Category::GUI},
	{"CraftHUD", "craft_hud", Category::GUI},
	{"TargetHUD", "target_hud", Category::GUI},

	{"Fullbright", "fullbright", Category::RENDER},
	{"Water Effect", "small_post_effect_color", Category::RENDER},
	{"Node illumination", "node_illumination", Category::RENDER},
	{"Display sunrise", "display_sunrise", Category::RENDER},
	{"Disable stars", "disable_stars", Category::RENDER},
	{"CustomFog", "use_custom_fog_color", Category::RENDER},
	{"Sky color", "use_custom_sky_color", Category::RENDER},
	{"Particles", "particles", Category::RENDER},
	{"TargetESP", "target_highlight_particles", Category::RENDER},

	{"Fast place", "fast_place", Category::MISC},
	{"NoFriend Damage", "no_friend_damage", Category::MISC},
}};

// PhotoHUD ("photo_hud") and HandView ("handview_enabled") are enable
// toggles too, but drawn inline next to their own "..." settings button
// (see drawMainWindow()) rather than in the generic table above, same
// as the old Menu treated them specially.

// ---- Color targets -- one row per hud_color_*/mineboost_gui_color
// setting the old Colors panel exposed. ---------------------------------

struct ColorTarget
{
	const char *label;
	const char *setting;
	const char *alpha_setting; // nullptr if this target has no alpha
};

constexpr std::array<ColorTarget, 14> kColorTargets = {{
	{"MineBoost GUI", "mineboost_gui_color", nullptr},
	{"Coords", "hud_color_coords", nullptr},
	{"FPS", "hud_color_fps", nullptr},
	{"Ping", "hud_color_ping", nullptr},
	{"NowPlaying", "hud_color_music", nullptr},
	{"ShowRP", "hud_color_rp", nullptr},
	{"ConsumptionHUD", "hud_color_consumption", nullptr},
	{"InventoryHUD", "hud_color_inventory", nullptr},
	{"CraftHUD", "hud_color_craft", nullptr},
	{"TargetHUD", "hud_color_target", nullptr},
	{"PhotoHUD", "hud_color_photo", nullptr},
	{"KeyStroker Outline", "hud_color_keystroker_border", nullptr},
	{"CPS Outline", "hud_color_cps_border", nullptr},
	{"Preview Outline", "hud_preview_border_color", nullptr},
}};

// ---- HandView animation styles -- data-driven, same reasoning as
// src/client/photohud.h's PhotoHudBuiltinImages table. ------------------

struct HandViewStyle
{
	const char *label;
	const char *settings_key;
};

constexpr std::array<HandViewStyle, 8> kHandViewStyles = {{
	{"Vanilla", "vanilla"},
	{"Static", "static"},
	{"Fast", "fast"},
	{"Sway", "sway"},
	{"Chime", "chime"},
	{"Old", "old"},
	{"Punch", "punch"},
	{"Tilt", "tilt"},
}};

// ---- "Move HUD" draggable elements -- one entry per built-in HUD
// element the old Menu::editMode system let you drag, ground-up rewrite
// as a data table (see drawHudEditOverlay()) instead of ~300 lines of
// per-element raw Irrlicht mouse-event handling. `default_w`/
// `default_h` are the drag hit-box size, not necessarily identical to
// the exact size the real HUD element draws itself at (which usually
// depends on its own "*_size" setting) -- close enough to grab
// comfortably is what matters here, not pixel-perfect matching; the
// real element still sizes itself correctly once drawn (see
// src/client/hud.cpp).
// ------------------------------------------------------------------

struct DraggableElement
{
	const char *label;
	// true: position stored as one v2f setting (x_setting only, via
	// g_settings->getV2F()/setV2F() -- same format Coords/FPS/Ping used
	// before). false: two separate settings (x_setting/y_setting).
	bool combined_v2f;
	const char *x_setting;
	const char *y_setting; // unused (may be nullptr) if combined_v2f
	s32 default_w, default_h;
	// Per-element size setting -- same "*_size" settings src/client/hud.cpp
	// already reads and multiplies with the global "hud_size" (see e.g.
	// Hud::drawCoordsHud()'s "coords_size", Hud::drawTargetHud()'s
	// "target_hud_size", ...) and the old Irrlicht Menu (Menu.cpp,
	// EMIE_MOUSE_WHEEL case) let you change via mouse wheel while hovering
	// an element in "Move HUD"/edit mode -- that scroll-to-resize
	// interaction was never ported when this ImGui menu replaced Menu.cpp,
	// even though the settings themselves were always fully wired up on
	// the drawing side. nullptr = not individually resizable (Chat has no
	// "*_size" setting in hud.cpp -- see the note on ClientChat being out
	// of scope elsewhere in this project).
	const char *size_setting;
	// true: size_setting stores raw pixels, stepped by ±20px per notch,
	// clamped [40,600] -- PhotoHUD only (see "photo_hud_size" in
	// src/client/photohud.cpp). false: size_setting stores a 0.5-2.5
	// multiplier, stepped by ±0.1 per notch -- every other element.
	bool size_in_pixels = false;
};

constexpr std::array<DraggableElement, 13> kDraggableElements = {{
	{"Coords", true, "coords_sprite", nullptr, 140, 30, "coords_size"},
	{"FPS", true, "fov_coords", nullptr, 140, 30, "fps_size"},
	{"Ping", true, "ping_coords", nullptr, 140, 30, "ping_size"},
	{"Chat", false, "chat_x", "chat_y", 300, 140, nullptr},
	{"KeyStroker", false, "keys_x", "keys_y", 160, 160, "keys_size"},
	{"CPS", false, "cps_x", "cps_y", 180, 54, "cps_size"},
	{"NowPlaying", false, "music_hud_x", "music_hud_y", 220, 50, "music_hud_size"},
	{"ShowRP", false, "rp_hud_x", "rp_hud_y", 220, 50, "rp_hud_size"},
	{"ConsumptionHUD", false, "consumption_hud_x", "consumption_hud_y", 320, 40, "consumption_hud_size"},
	{"TargetHUD", false, "target_hud_x", "target_hud_y", 160, 160, "target_hud_size"},
	{"InventoryHUD", false, "inventory_hud_x", "inventory_hud_y", 440, 220, "inventory_hud_size"},
	{"CraftHUD", false, "craft_hud_x", "craft_hud_y", 220, 220, "craft_hud_size"},
	{"PhotoHUD", false, "photo_hud_x", "photo_hud_y", 200, 200, "photo_hud_size", true},
}};

} // namespace

ImGuiMineBoostMenu &ImGuiMineBoostMenu::get()
{
	static ImGuiMineBoostMenu instance;
	return instance;
}

void ImGuiMineBoostMenu::close()
{
	m_open = false;
	// Sub-windows/overlays are only meaningful while the main window is
	// open -- closing the whole menu also drops any in-progress bind
	// capture and turns off "Move HUD" edit mode, same as the old Menu
	// did (see its own close()).
	m_show_colors = false;
	m_show_handview = false;
	m_show_photohud = false;
	m_show_sky = false;
	m_hud_edit_mode = false;
	m_bind_capture_setting.clear();
}

bool ImGuiMineBoostMenu::processEvent(const irr::SEvent &event)
{
	if (m_bind_capture_setting.empty())
		return false;
	if (event.EventType != irr::EET_KEY_INPUT_EVENT && event.EventType != irr::EET_MOUSE_INPUT_EVENT)
		return false;

	std::string token;
	bool cancel = false;
	if (!eventToBindToken(event, token, cancel))
		return false;

	if (!cancel) {
		std::string s1, s2;
		getBindTokens(m_bind_capture_setting, s1, s2);
		if (m_bind_capture_slot == 1)
			s1 = token;
		else
			s2 = token;
		if (!s1.empty() && s1 == s2) {
			// Same token in both slots is pointless -- drop whichever
			// one we didn't just set.
			if (m_bind_capture_slot == 1) s2.clear(); else s1.clear();
		}
		setBindTokens(m_bind_capture_setting, s1, s2);
	}
	m_bind_capture_setting.clear();
	return true;
}

void ImGuiMineBoostMenu::checkOpenKeybind(const irr::SEvent &event)
{
	if (event.EventType != irr::EET_KEY_INPUT_EVENT || !event.KeyInput.PressedDown)
		return;
	if (!(KeyPress(event.KeyInput) == getKeySetting("keymap_menu")))
		return;
	toggle();
}

void ImGuiMineBoostMenu::checkGlobalBinds(const irr::SEvent &event)
{
	// While actively waiting for a bind assignment, the very next key/
	// mouse press is being captured as the new bind token in
	// processEvent() instead -- skip the toggle here so assigning "F" as
	// a new bind doesn't also immediately toggle whatever "F" already
	// happened to be bound to.
	if (!m_bind_capture_setting.empty())
		return;

	// Every event this whole class handles is fed unconditionally from
	// MyEventReceiver::OnEvent() (src/client/inputhandler.cpp) from very
	// early in startup -- including, on at least some platforms/drivers,
	// synthetic events (e.g. an initial resize/focus event) generated as
	// a side effect of creating the render window itself, well before
	// ClientLauncher::run() reaches the point where it calls
	// ImGuiManager::get().init() (src/client/clientlauncher.cpp). Calling
	// any ImGui:: function -- including just ImGui::GetIO() below --
	// before ImGui::CreateContext() has run is an immediate hard crash
	// (ImGui asserts on it), so this has to check first rather than
	// assume a live context always exists by the time an event arrives.
	if (!ImGuiManager::get().isInitialized())
		return;

	// Skipped while an ImGui widget currently wants keyboard input
	// (typing in a text field), so typing doesn't accidentally trigger a
	// bind -- same intent as the old code's "skip while an Irrlicht edit
	// box has focus" check, just via ImGui's own IO flag since this
	// class doesn't use Irrlicht GUI elements at all. In practice this
	// branch is rarely even reached while typing: ImGuiManager::
	// processEvent() (src/gui/ImGuiManager.cpp) already swallows and
	// returns before MyEventReceiver::OnEvent() gets this far -- this is
	// a second, explicit guard rather than relying solely on call order.
	if (ImGui::GetIO().WantCaptureKeyboard)
		return;

	std::string token;
	bool cancel = false;
	if (!eventToBindToken(event, token, cancel) || cancel || token.empty())
		return;

	auto applyIfBound = [&](const char *setting) {
		std::string s1, s2;
		getBindTokens(setting, s1, s2);
		if ((s1 == token || s2 == token) && g_settings->exists(setting))
			g_settings->setBool(setting, !g_settings->getBool(setting));
	};

	for (const auto &s : kSettings)
		applyIfBound(s.setting);
	// PhotoHUD/HandView aren't in kSettings (drawn specially, see
	// drawMainWindow()) but were included in the old Menu::getSettings()
	// list checkGlobalBinds() iterated -- covered explicitly here so a
	// bound key for either still works exactly as before.
	applyIfBound("photo_hud");
	applyIfBound("handview_enabled");
}

void ImGuiMineBoostMenu::drawSettingToggle(const char *label, const char *setting)
{
	bool value = safeGetBool(setting, false);
	ImGui::PushID(setting);
	if (ImGui::Checkbox(label, &value))
		g_settings->setBool(setting, value);

	// Bind-capture button, right-aligned in the same row -- click to
	// (re)bind, same "middle-click to bind, click again to add a second
	// slot, empty to clear" semantics processEvent() implements.
	ImGui::SameLine(220);
	std::string bind_label = "Bind: " + getBindDisplayString(setting);
	bool capturing = (m_bind_capture_setting == setting);
	if (capturing)
		bind_label = "Press a key...";
	if (ImGui::SmallButton(bind_label.c_str())) {
		if (capturing) {
			m_bind_capture_setting.clear();
		} else {
			std::string s1, s2;
			getBindTokens(setting, s1, s2);
			if (!s1.empty() && !s2.empty()) {
				setBindTokens(setting, "", ""); // both full -> start over
				s1.clear(); // so the slot pick below lands on slot 1, not 2
			}
			m_bind_capture_setting = setting;
			m_bind_capture_slot = s1.empty() ? 1 : 2;
		}
	}
	// Shift+RMB directly on the Bind button clears it immediately -- no
	// need to click "Bind:" first to enter capture mode just to cancel
	// out of it. This is the actual always-available "hold Shift, press
	// RMB to clear" gesture; it's separate from (and works alongside) the
	// Shift+RMB handling inside eventToBindToken(), which only cancels an
	// *in-progress* capture and requires having clicked to start one.
	if (ImGui::IsItemHovered() && ImGui::GetIO().KeyShift &&
			ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
		setBindTokens(setting, "", "");
		if (capturing)
			m_bind_capture_setting.clear();
	}
	ImGui::PopID();
}

void ImGuiMineBoostMenu::drawScrollbarsSection()
{
	// Ranges/setting names match the old Menu's scrollbars exactly (see
	// its updateFpsScrollBarPosition() and friends) -- only the widget
	// implementation changed.
	float fov = safeGetFloat("fov_custom.data", 90.0f);
	if (ImGui::SliderFloat("FOV", &fov, 75.0f, 160.0f, "%.0f"))
		g_settings->setFloat("fov_custom.data", fov);

	int fps_max = safeGetS32("fps_max", 60);
	if (ImGui::SliderInt("FPS limit", &fps_max, 1, 1000))
		g_settings->setU16("fps_max", (u16)rangelim(fps_max, 1, 1000));

	int hit_particles = safeGetS32("hit_particle_amount", 1);
	if (ImGui::SliderInt("Hit Particles", &hit_particles, 1, 300))
		g_settings->setS32("hit_particle_amount", hit_particles);

	int target_particles = safeGetS32("target_highlight_particle_amount", 1);
	if (ImGui::SliderInt("Target Particles", &target_particles, 1, 50))
		g_settings->setS32("target_highlight_particle_amount", target_particles);

	float hud_size_pct = rangelim(safeGetFloat("hud_size", 1.0f), 0.5f, 2.5f) * 100.0f;
	if (ImGui::SliderFloat("HUD Size", &hud_size_pct, 50.0f, 250.0f, "%.0f%%"))
		g_settings->setFloat("hud_size", hud_size_pct / 100.0f);
}

void ImGuiMineBoostMenu::drawColorsWindow()
{
	if (!m_show_colors)
		return;

	ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoost Colors", &m_show_colors)) {
		for (const auto &target : kColorTargets) {
			ImGui::PushID(target.setting);
			v3f c = g_settings->getV3F(target.setting).value_or(v3f(255, 255, 255));
			float col[4] = {
				rangelim(c.X, 0.0f, 255.0f) / 255.0f,
				rangelim(c.Y, 0.0f, 255.0f) / 255.0f,
				rangelim(c.Z, 0.0f, 255.0f) / 255.0f,
				target.alpha_setting ? safeGetS32(target.alpha_setting, 255) / 255.0f : 1.0f,
			};
			bool changed = target.alpha_setting ?
				ImGui::ColorEdit4(target.label, col, ImGuiColorEditFlags_AlphaBar) :
				ImGui::ColorEdit3(target.label, col);
			if (changed) {
				g_settings->setV3F(target.setting, v3f(col[0] * 255.0f, col[1] * 255.0f, col[2] * 255.0f));
				if (target.alpha_setting)
					g_settings->setS32(target.alpha_setting, (s32)rangelim(myround(col[3] * 255.0f), 0, 255));
			}
			ImGui::PopID();
		}
	}
	ImGui::End();
}

void ImGuiMineBoostMenu::drawHandViewWindow()
{
	if (!m_show_handview)
		return;

	ImGui::SetNextWindowSize(ImVec2(420, 360), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoost HandView", &m_show_handview)) {
		std::string selected = safeGetString("hand_anim_style", "vanilla");
		ImGui::TextUnformatted("Animation style:");
		for (size_t i = 0; i < kHandViewStyles.size(); ++i) {
			const auto &style = kHandViewStyles[i];
			bool is_selected = (selected == style.settings_key);
			if (ImGui::RadioButton(style.label, is_selected))
				g_settings->set("hand_anim_style", style.settings_key);
			// 4 per row, matching the old Menu's row1/row2 layout.
			if (i % 4 != 3 && i + 1 < kHandViewStyles.size())
				ImGui::SameLine();
		}

		ImGui::Separator();
		bool left_hand = safeGetBool("left_hand", false);
		if (ImGui::Checkbox("Left Hand", &left_hand))
			g_settings->setBool("left_hand", left_hand);
		ImGui::SameLine();
		bool no_view_bob = safeGetBool("no_view_bob", false);
		if (ImGui::Checkbox("NoViewBob", &no_view_bob))
			g_settings->setBool("no_view_bob", no_view_bob);

		ImGui::Separator();
		float offset_x = safeGetFloat("handview_offset_x", 0.0f);
		if (ImGui::SliderFloat("Offset X", &offset_x, -100.0f, 100.0f))
			g_settings->setFloat("handview_offset_x", offset_x);
		float offset_y = safeGetFloat("handview_offset_y", 0.0f);
		if (ImGui::SliderFloat("Offset Y", &offset_y, -100.0f, 100.0f))
			g_settings->setFloat("handview_offset_y", offset_y);
		float offset_z = safeGetFloat("handview_offset_z", 0.0f);
		if (ImGui::SliderFloat("Offset Z", &offset_z, -100.0f, 100.0f))
			g_settings->setFloat("handview_offset_z", offset_z);
		float scale_pct = rangelim(safeGetFloat("handview_scale", 1.0f), 0.3f, 3.0f) * 100.0f;
		if (ImGui::SliderFloat("Scale", &scale_pct, 30.0f, 300.0f, "%.0f%%"))
			g_settings->setFloat("handview_scale", scale_pct / 100.0f);
	}
	ImGui::End();
}

void ImGuiMineBoostMenu::drawPhotoHudWindow()
{
	if (!m_show_photohud)
		return;

	ImGui::SetNextWindowSize(ImVec2(360, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoost PhotoHUD", &m_show_photohud)) {
		std::string selected = safeGetString("photo_hud_image", "face");

		ImGui::TextUnformatted("Which photo should be shown?");
		for (const auto &img : PhotoHudBuiltinImages) {
			bool is_selected = (selected == img.settings_key);
			// PhotoHudBuiltinImage::button_label is a wchar_t* (shared
			// with the old, still-ASCII-only Irrlicht button labels) --
			// every built-in label is plain ASCII, so a straight
			// wchar_t->char narrow is safe here without a full UTF
			// conversion.
			std::string label;
			for (const wchar_t *p = img.button_label; *p; ++p)
				label += (char)*p;
			if (ImGui::RadioButton(label.c_str(), is_selected))
				g_settings->set("photo_hud_image", img.settings_key);
		}

		ImGui::Separator();
		ImGui::TextUnformatted("Custom image (full file path):");
		static char path_buf[512];
		static bool path_buf_loaded = false;
		if (!path_buf_loaded) {
			std::string saved = safeGetString("photo_hud_custom_path", "");
			std::strncpy(path_buf, saved.c_str(), sizeof(path_buf) - 1);
			path_buf[sizeof(path_buf) - 1] = '\0';
			path_buf_loaded = true;
		}
		ImGui::InputText("##photohud_path", path_buf, sizeof(path_buf));

		static std::string status;
		static u64 status_expire_ms = 0;
		if (ImGui::Button("Use")) {
			// Same validation the old PhotoHudSettingsMenu applied --
			// wrapped in try/catch for the same reason (fs::PathExists()/
			// IsDir() are the one part of this that touches the
			// filesystem and could plausibly throw something
			// unanticipated; any failure here just means "don't apply",
			// never a crash).
			try {
				std::string path(path_buf);
				size_t begin = path.find_first_not_of(" \t\r\n");
				size_t end = path.find_last_not_of(" \t\r\n");
				path = (begin == std::string::npos) ? "" : path.substr(begin, end - begin + 1);
				status_expire_ms = porting::getTimeMs() + 4000;

				if (path.empty()) {
					status = "Enter a file path first";
				} else if (!fs::PathExists(path)) {
					status = "File not found";
				} else if (fs::IsDir(path)) {
					status = "That's a folder, not a file";
				} else {
					std::string ext = fs::GetFilenameFromPath(path.c_str());
					size_t dot = ext.find_last_of('.');
					ext = (dot == std::string::npos) ? "" : lowercase(ext.substr(dot + 1));
					if (ext != "png" && ext != "jpg" && ext != "jpeg") {
						status = "Must be a .png or .jpg file";
					} else {
						g_settings->set("photo_hud_custom_path", path);
						g_settings->set("photo_hud_image", "custom");
						status = "Applied";
					}
				}
			} catch (const std::exception &e) {
				errorstream << "[MineBoost] ImGuiMineBoostMenu PhotoHUD "
					"Use failed: " << e.what() << std::endl;
				status = "Couldn't apply that path (see the log)";
				status_expire_ms = porting::getTimeMs() + 4000;
			}
		}
		if (!status.empty() && porting::getTimeMs() < status_expire_ms) {
			ImVec4 color = (status == "Applied") ?
				ImVec4(0.55f, 0.86f, 0.55f, 1.0f) : ImVec4(0.90f, 0.51f, 0.51f, 1.0f);
			ImGui::TextColored(color, "%s", status.c_str());
		}

		ImGui::Separator();
		bool show_in_game = safeGetBool("photo_hud_show_in_game", false);
		if (ImGui::Checkbox("Show in game", &show_in_game))
			g_settings->setBool("photo_hud_show_in_game", show_in_game);
		ImGui::TextWrapped(
			"Otherwise only shown while a GUI (inventory, chest, this "
			"menu, ...) is open.");

		// Live image preview (an actual thumbnail) is intentionally not
		// shown here: bridging an Irrlicht video::ITexture to a raw GL
		// handle for ImGui::Image() needs a proper cross-driver-safe
		// accessor (ITexture itself doesn't expose one -- only the
		// concrete, driver-templated implementation class does), and
		// getting that wrong risks binding an invalid handle. Left as a
		// clearly-scoped follow-up rather than guessing at something
		// that could crash on a driver variant untested here; showing
		// which file is selected is enough to confirm the setting took.
		if (selected == "custom") {
			std::string custom_path = safeGetString("photo_hud_custom_path", "");
			ImGui::TextDisabled("Selected file: %s", custom_path.empty() ?
				"(none)" : custom_path.c_str());
		} else {
			for (const auto &img : PhotoHudBuiltinImages)
				if (selected == img.settings_key)
					ImGui::TextDisabled("Selected file: %s", img.texture_filename);
		}
	}
	ImGui::End();
}

void ImGuiMineBoostMenu::drawSkyTextureWindow()
{
	if (!m_show_sky)
		return;

	// Same 6 faces/order as SkyTextureSettings[] (src/client/sky.h) and
	// SkyTextures[] (src/client/sky.cpp), which loadSkyboxFaceTexture()
	// indexes into -- these have to line up.
	static const struct { const char *label; const char *setting; } kFaces[] = {
		{"Top", "skybox_texture_top"},
		{"Bottom", "skybox_texture_bottom"},
		{"East", "skybox_texture_east"},
		{"West", "skybox_texture_west"},
		{"South", "skybox_texture_south"},
		{"North", "skybox_texture_north"},
	};

	ImGui::SetNextWindowSize(ImVec2(420, 420), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoost Sky Texture", &m_show_sky)) {
		ImGui::TextWrapped("Custom skybox images, one per face (full file "
			"paths). Leave a face blank to keep the built-in texture for "
			"it. Needs \"Custom sky texture\" enabled (main Render tab) "
			"to actually show.");
		ImGui::Separator();

		static char path_buf[6][512];
		static bool loaded = false;
		if (!loaded) {
			for (int i = 0; i < 6; ++i) {
				std::string saved = safeGetString(kFaces[i].setting, "");
				std::strncpy(path_buf[i], saved.c_str(), sizeof(path_buf[i]) - 1);
				path_buf[i][sizeof(path_buf[i]) - 1] = '\0';
			}
			loaded = true;
		}

		static std::string status;
		static u64 status_expire_ms = 0;

		for (int i = 0; i < 6; ++i) {
			ImGui::PushID(i);
			ImGui::TextUnformatted(kFaces[i].label);
			ImGui::SameLine(70);
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##sky_path", path_buf[i], sizeof(path_buf[i]));
			ImGui::PopID();
		}

		if (ImGui::Button("Apply")) {
			// Same validation style as drawPhotoHudWindow()'s "Use" --
			// wrapped in try/catch for the same reason (fs::PathExists()/
			// IsDir() are the one part of this that touches the
			// filesystem). An empty field is valid here (falls back to
			// the built-in face texture, see loadSkyboxFaceTexture()) --
			// only a non-empty field that doesn't resolve to a real,
			// non-directory image file is rejected.
			try {
				bool all_ok = true;
				std::string bad_face;
				for (int i = 0; i < 6 && all_ok; ++i) {
					std::string path(path_buf[i]);
					size_t begin = path.find_first_not_of(" \t\r\n");
					size_t end = path.find_last_not_of(" \t\r\n");
					path = (begin == std::string::npos) ? "" : path.substr(begin, end - begin + 1);
					std::strncpy(path_buf[i], path.c_str(), sizeof(path_buf[i]) - 1);
					path_buf[i][sizeof(path_buf[i]) - 1] = '\0';

					if (path.empty())
						continue;
					if (!fs::PathExists(path) || fs::IsDir(path)) {
						all_ok = false;
						bad_face = kFaces[i].label;
						continue;
					}
					std::string ext = fs::GetFilenameFromPath(path.c_str());
					size_t dot = ext.find_last_of('.');
					ext = (dot == std::string::npos) ? "" : lowercase(ext.substr(dot + 1));
					if (ext != "png" && ext != "jpg" && ext != "jpeg") {
						all_ok = false;
						bad_face = kFaces[i].label;
					}
				}

				status_expire_ms = porting::getTimeMs() + 4000;
				if (!all_ok) {
					status = std::string(bad_face) + ": not a valid .png/.jpg file";
				} else {
					for (int i = 0; i < 6; ++i)
						g_settings->set(kFaces[i].setting, path_buf[i]);
					status = "Applied";
				}
			} catch (const std::exception &e) {
				errorstream << "[MineBoost] ImGuiMineBoostMenu Sky "
					"Texture Apply failed: " << e.what() << std::endl;
				status = "Couldn't apply that path (see the log)";
				status_expire_ms = porting::getTimeMs() + 4000;
			}
		}
		if (!status.empty() && porting::getTimeMs() < status_expire_ms) {
			ImVec4 color = (status == "Applied") ?
				ImVec4(0.55f, 0.86f, 0.55f, 1.0f) : ImVec4(0.90f, 0.51f, 0.51f, 1.0f);
			ImGui::TextColored(color, "%s", status.c_str());
		}
	}
	ImGui::End();
}

void ImGuiMineBoostMenu::drawMainWindow()
{
	if (!m_open)
		return;

	ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoostV2 Settings", &m_open)) {
		if (ImGui::BeginTabBar("categories")) {
			if (ImGui::BeginTabItem("GUI")) {
				for (const auto &s : kSettings)
					if (s.category == Category::GUI)
						drawSettingToggle(s.label, s.setting);

				ImGui::Separator();
				drawSettingToggle("PhotoHUD", "photo_hud");
				ImGui::SameLine();
				if (ImGui::SmallButton("PhotoHUD settings..."))
					m_show_photohud = !m_show_photohud;

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Render")) {
				for (const auto &s : kSettings)
					if (s.category == Category::RENDER)
						drawSettingToggle(s.label, s.setting);

				ImGui::Separator();
				drawSettingToggle("HandView", "handview_enabled");
				ImGui::SameLine();
				if (ImGui::SmallButton("HandView settings..."))
					m_show_handview = !m_show_handview;

				ImGui::Separator();
				drawSettingToggle("Custom sky texture", "force_custom_skybox");
				ImGui::SameLine();
				if (ImGui::SmallButton("Sky texture settings..."))
					m_show_sky = !m_show_sky;

				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Misc")) {
				for (const auto &s : kSettings)
					if (s.category == Category::MISC)
						drawSettingToggle(s.label, s.setting);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Scrollbars")) {
				drawScrollbarsSection();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}

		ImGui::Separator();
		if (ImGui::Button("Colors..."))
			m_show_colors = !m_show_colors;
		ImGui::SameLine();
		if (ImGui::Button(m_hud_edit_mode ? "Done editing HUD" : "Edit HUD"))
			m_hud_edit_mode = !m_hud_edit_mode;
	}
	ImGui::End();

	if (!m_open) {
		// Closed via the window's own [x] this frame -- keep sub-window/
		// overlay state consistent with close() below rather than
		// leaving them dangling open with no way to reach them.
		m_show_colors = false;
		m_show_handview = false;
		m_show_photohud = false;
		m_show_sky = false;
		m_hud_edit_mode = false;
		m_bind_capture_setting.clear();
	}
}

void ImGuiMineBoostMenu::drawHudEditOverlay()
{
	if (!m_hud_edit_mode)
		return;

	ImGuiIO &io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
	ImGui::Begin("##mineboost_hud_edit_overlay", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus |
		ImGuiWindowFlags_NoNav);
	ImDrawList *draw_list = ImGui::GetWindowDrawList();

	// Snap-to-grid while dragging -- same "hud_grid_size" setting the
	// old edit mode used, 0/negative disables snapping entirely (treated
	// as "no grid", not a divide-by-zero -- see the guard below).
	s32 grid = safeGetS32("hud_grid_size", 20);

	// Faint grid lines, purely a visual alignment aid -- only worth the
	// draw calls while snapping is actually in effect.
	if (grid > 0) {
		ImU32 grid_color = IM_COL32(255, 255, 255, 18);
		for (float gx = 0; gx < io.DisplaySize.x; gx += grid)
			draw_list->AddLine(ImVec2(gx, 0), ImVec2(gx, io.DisplaySize.y), grid_color);
		for (float gy = 0; gy < io.DisplaySize.y; gy += grid)
			draw_list->AddLine(ImVec2(0, gy), ImVec2(io.DisplaySize.x, gy), grid_color);
	}

	float global_size = rangelim(safeGetFloat("hud_size", 1.0f), 0.5f, 2.5f);

	for (size_t i = 0; i < kDraggableElements.size(); ++i) {
		const auto &el = kDraggableElements[i];
		ImGui::PushID((int)i);

		float x, y;
		if (el.combined_v2f) {
			v2f pos(-1.0f, -1.0f);
			g_settings->getV2FNoEx(el.x_setting, pos); // leaves pos untouched if missing/invalid
			x = pos.X;
			y = pos.Y;
		} else {
			x = (float)safeGetS32(el.x_setting, -1);
			y = (float)safeGetS32(el.y_setting, -1);
		}
		// Never positioned yet -- lay out in a simple top-left cascade
		// rather than trying to replicate each element's own historical
		// per-element default-position formula (several were relative to
		// other elements' current size/position, which no longer exists
		// here as a fixed layout pass) -- purely a reasonable starting
		// point the player immediately drags to where they actually want
		// it anyway.
		if (x < 0 || y < 0) {
			x = 20.0f + (float)(i % 4) * 240.0f;
			y = 20.0f + (float)(i / 4) * 60.0f;
		}

		// Drag/drop hitbox and outline scale with the same "hud_size" *
		// "*_size" multiplier the real HUD element actually draws at (see
		// the "*_size" reads throughout src/client/hud.cpp) -- previously
		// this always used the table's fixed default_w/default_h, so the
		// edit-mode box drifted out of sync with (and often badly
		// overlapped) the real element the moment its size setting was
		// anything other than 1.0x, which is exactly what made the boxes
		// pile up on each other in edit mode.
		float box_w, box_h;
		if (el.size_setting && el.size_in_pixels) {
			float px = (float)safeGetS32(el.size_setting, 100);
			box_w = px;
			box_h = px;
		} else {
			float mult = global_size *
				(el.size_setting ? rangelim(safeGetFloat(el.size_setting, 1.0f), 0.5f, 2.5f) : 1.0f);
			box_w = el.default_w * mult;
			box_h = el.default_h * mult;
		}

		ImGui::SetCursorScreenPos(ImVec2(x, y));
		ImGui::InvisibleButton("##drag", ImVec2(box_w, box_h));
		bool active = ImGui::IsItemActive();
		bool hovered = ImGui::IsItemHovered();

		if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
			x += io.MouseDelta.x;
			y += io.MouseDelta.y;
			if (grid > 0) {
				x = std::round(x / grid) * grid;
				y = std::round(y / grid) * grid;
			}
			x = rangelim(x, 0.0f, io.DisplaySize.x - box_w);
			y = rangelim(y, 0.0f, io.DisplaySize.y - box_h);

			if (el.combined_v2f) {
				g_settings->setV2F(el.x_setting, v2f(x, y));
			} else {
				g_settings->setS32(el.x_setting, (s32)x);
				g_settings->setS32(el.y_setting, (s32)y);
			}
		}

		// Scroll-to-resize while hovering, in edit mode -- porting the old
		// Irrlicht Menu's EMIE_MOUSE_WHEEL handling (see Menu.cpp) onto
		// ImGui: this was previously not ported over at all, so there was
		// no way to change an individual element's size even though
		// hud.cpp has always read+multiplied by exactly these settings.
		// Scroll up = bigger, down = smaller; PhotoHUD steps in pixels,
		// everything else steps the 0.5-2.5 multiplier by 0.1 per notch.
		std::string hint;
		if (el.size_setting) {
			if (hovered && io.MouseWheel != 0.0f) {
				if (el.size_in_pixels) {
					s32 sz = safeGetS32(el.size_setting, 100) + (s32)(io.MouseWheel * 20.0f);
					g_settings->setS32(el.size_setting, rangelim(sz, 40, 600));
				} else {
					float mult = safeGetFloat(el.size_setting, 1.0f) + io.MouseWheel * 0.1f;
					g_settings->setFloat(el.size_setting, rangelim(mult, 0.5f, 2.5f));
				}
			}
			if (hovered) {
				char buf[48];
				if (el.size_in_pixels)
					porting::mt_snprintf(buf, sizeof(buf), "Scroll to resize (%dpx)",
						safeGetS32(el.size_setting, 100));
				else
					porting::mt_snprintf(buf, sizeof(buf), "Scroll to resize (%.0f%%)",
						rangelim(safeGetFloat(el.size_setting, 1.0f), 0.5f, 2.5f) * 100.0f);
				hint = buf;
			}
		}

		ImU32 box_color = active ? IM_COL32(90, 150, 250, 255) :
			hovered ? IM_COL32(230, 230, 230, 220) : IM_COL32(255, 255, 255, 130);
		draw_list->AddRect(ImVec2(x, y), ImVec2(x + box_w, y + box_h),
			box_color, 4.0f, 0, active ? 3.0f : 2.0f);
		draw_list->AddText(ImVec2(x + 6, y + 6), IM_COL32(255, 255, 255, 255), el.label);
		if (!hint.empty())
			draw_list->AddText(ImVec2(x + 6, y + 6 + ImGui::GetFontSize() + 2.0f),
				IM_COL32(255, 230, 120, 255), hint.c_str());

		ImGui::PopID();
	}

	// Small always-on-top hint + a way out that doesn't require finding
	// the (currently invisible-behind-these-boxes) main settings window
	// again.
	ImGui::SetCursorScreenPos(ImVec2(20, io.DisplaySize.y - 50));
	if (ImGui::Button("Done editing HUD"))
		m_hud_edit_mode = false;

	ImGui::End();
	ImGui::PopStyleColor();
}

void ImGuiMineBoostMenu::draw()
{
	if (!m_open)
		return;

	drawMainWindow();
	drawColorsWindow();
	drawHandViewWindow();
	drawPhotoHudWindow();
	drawSkyTextureWindow();
	drawHudEditOverlay();
}
