#include "Menu.h"
#include "constants.h"
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <algorithm>
#include "client/localplayer.h"
#include "client/renderingengine.h"
#include "client/texturesource.h"
#include "hud.h"
#include "util/string.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"
#include "util/numeric.h"
#include "client/keycode.h"
#include "gui/custom_menu/ModernUI.h"
#include <IGUIEditBox.h>

// The safeGetString()/safeGetBool() helpers that used to live here were
// only used by the old PhotoHUD advanced-settings panel; PhotoHUD has
// since been rewritten from the ground up as two fully independent
// classes -- PhotoHudSettingsMenu (this panel, src/gui/custom_menu/
// PhotoHudSettingsMenu.h/.cpp) and PhotoHud (the actual on-screen HUD
// element, src/client/photohud.h/.cpp) -- each with their own local
// copies of whatever small helpers they need, so the dead copies that
// used to live here were removed.

// ---- Per-function keybinds --------------------------------------------
//
// Any boolean setting from Menu::getSettings() can have up to 2 binds,
// stored as a single "bind_<setting>" setting containing "TOKEN1,TOKEN2"
// (either half may be empty). Tokens are either a keyboard key's stable
// name (reusing the engine's existing KeyPress::sym(), e.g. "KEY_F6",
// "KEY_KEY_A", "KEY_UP" -- covers the *entire* keyboard, not just
// letters/digits) or one of our own mouse tokens below (wheel up/down,
// middle button, side buttons X1/X2). Left/right mouse buttons are
// deliberately not bindable -- they're needed for clicking/gameplay.
//
// This whole system lives outside the engine's native "keymap_*"
// keybind system (used for movement/interact/etc, see Game::processKeyInput
// in src/client/game.cpp) -- it doesn't touch that, so there's no risk of
// interfering with core controls.

// Reads an event and, if it's something we allow binding to, returns the
// stored token for it. Returns false if the event isn't a "press" at all
// (key-up, mouse move, etc). If the event is a press of something we
// deliberately never allow binding (Escape, LMB, RMB), returns true with
// cancel_out=true and an empty token, so callers can tell "this press
// means abort" apart from "this press just doesn't map to anything".
static bool eventToBindToken(const irr::SEvent &event, std::string &token_out, bool &cancel_out)
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
            case irr::EMIE_LMOUSE_PRESSED_DOWN:
            case irr::EMIE_RMOUSE_PRESSED_DOWN:
                cancel_out = true;
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

// Human-readable label for a stored token, e.g. for "KEY_F6" -> "F6",
// "MOUSE_X1" -> "Mouse4". Falls back to the raw token if unrecognized
// (shouldn't normally happen since we only ever write tokens we produced
// ourselves above).
static std::wstring bindTokenDisplayName(const std::string &token)
{
    if (token.empty())
        return L"";
    if (token == "WHEEL_UP") return L"MWheel Up";
    if (token == "WHEEL_DOWN") return L"MWheel Down";
    if (token == "MOUSE_MIDDLE") return L"MMB";
    if (token == "MOUSE_X1") return L"Mouse4";
    if (token == "MOUSE_X2") return L"Mouse5";

    KeyPress kp(token.c_str());
    const char *nm = kp.name();
    if (nm && nm[0])
        return utf8_to_wide(nm);
    return utf8_to_wide(token);
}

static void getBindTokens(const std::string &setting_name, std::string &slot1, std::string &slot2)
{
    std::string key = "bind_" + setting_name;
    std::string raw = g_settings->exists(key) ? g_settings->get(key) : "";
    size_t comma = raw.find(',');
    if (comma == std::string::npos) {
        slot1 = raw;
        slot2.clear();
    } else {
        slot1 = raw.substr(0, comma);
        slot2 = raw.substr(comma + 1);
    }
}

static void setBindTokens(const std::string &setting_name, const std::string &slot1, const std::string &slot2)
{
    g_settings->set("bind_" + setting_name, slot1 + "," + slot2);
}

// Small combined label for both slots of a setting, e.g. "F6, MWheel Up",
// used when drawing each tile in the settings menu. Deliberately short --
// how to set/clear a bind is explained once, in the legend line drawn
// under the category tabs (see the "MMB: set bind" text in draw()),
// rather than repeated on every tile: the old "(MMB to set)"/"(RMB to
// clear)" suffixes routinely made this wider than a 120px tile and spilled
// into the neighboring one.
static std::wstring getBindDisplayString(const std::string &setting_name)
{
    std::string s1, s2;
    getBindTokens(setting_name, s1, s2);
    if (s1.empty() && s2.empty())
        return L"Bind: --";

    std::wstring out = L"Bind: " + bindTokenDisplayName(s1);
    if (!s2.empty())
        out += L", " + bindTokenDisplayName(s2);
    return out;
}

Sprite_ Menu::coords_sprite = Sprite_();

core::vector2d<s32> offset_f;
Sprite_ fov_sprite = Sprite_();

core::vector2d<s32> offset_ping;
Sprite_ ping_sprite = Sprite_();

Sprite_ chat = Sprite_();
core::vector2d<s32> offset_chp;

Sprite_ keystr = Sprite_();
core::vector2d<s32> offset_keys;

// ShowCPS: split out of KeyStroker into its own independently
// toggleable/movable HUD -- see "show_cps"/"cps_x"/"cps_y"/"cps_size" and
// the real HUD text elements in builtin/client/keystroker.lua.
Sprite_ cps_sprite = Sprite_();
core::vector2d<s32> offset_cps;

Sprite_ music_sprite = Sprite_();
core::vector2d<s32> offset_music;

Sprite_ rp_sprite = Sprite_();
Sprite_ consumption_sprite = Sprite_();
core::vector2d<s32> offset_rp;
core::vector2d<s32> offset_consumption;

Sprite_ target_hud_sprite = Sprite_();
core::vector2d<s32> offset_target_hud;

Sprite_ inventory_hud_sprite = Sprite_();
core::vector2d<s32> offset_inventory_hud;

Sprite_ craft_hud_sprite = Sprite_();
core::vector2d<s32> offset_craft_hud;

Sprite_ photo_sprite = Sprite_();
core::vector2d<s32> offset_photo;

// ArmorHUD temporarily disabled -- sprite globals kept (commented) for
// re-enabling later.
// Sprite_ armor_hud_sprite = Sprite_();
// core::vector2d<s32> offset_armor_hud;

// While in HUD-drag mode (Menu::editMode), each draggable element gets a
// filled preview box with sample text/content instead of a bare outline,
// so the user can see what will actually be shown there without needing
// the real trigger condition (chat messages, aiming at a player, music
// playing, etc.) to be active at that moment.
// Border color for the "Move HUD" edit-mode drag-preview boxes -- see
// "hud_preview_border_color" in src/defaultsettings.cpp and the "Preview
// Outline" entry in getColorTargets() below. Separate from the individual
// per-element hud_color_* fills (KeyStroker/CPS/etc.), which stay as they
// are; this only recolors the box outline itself. Falls back to
// ModernUI::PanelBorder's blue if the setting is ever missing/malformed.
static video::SColor getHudPreviewBorderColor()
{
    v3f c = g_settings->getV3F("hud_preview_border_color").value_or(v3f(90, 150, 250));
    return video::SColor(255,
        rangelim((s32)myround(c.X), 0, 255),
        rangelim((s32)myround(c.Y), 0, 255),
        rangelim((s32)myround(c.Z), 0, 255));
}

// Overall MineBoost GUI/menu accent color -- see "mineboost_gui_color" in
// src/defaultsettings.cpp and the "MineBoost GUI" entry in getColorTargets()
// below. Recolors the interface chrome itself (main settings window, Colors
// panel, bind-capture prompt, Photo HUD/HandView advanced-settings panels),
// as opposed to the individual hud_color_* / hud_preview_border_color
// settings, which only ever recolor in-game HUD elements. Falls back to
// ModernUI::PanelBorder's blue if the setting is ever missing/malformed, same
// as every other color getter here.
static video::SColor getMineBoostGuiColor()
{
    v3f c = g_settings->getV3F("mineboost_gui_color").value_or(v3f(90, 150, 250));
    return video::SColor(255,
        rangelim((s32)myround(c.X), 0, 255),
        rangelim((s32)myround(c.Y), 0, 255),
        rangelim((s32)myround(c.Z), 0, 255));
}

// KeyStroker/CPS preview boxes: keystr.x/cps_sprite.x are the raw anchor
// pixel ("keys_x"/"cps_x" settings) -- that's what gets saved when you drag
// them. builtin/client/keystroker.lua's update_hud_positions()/
// update_cps_hud_position() shift the HUD's own position anchor by
// -KEYS_BG_OFFSET*size/-CPS_BG_OFFSET*size specifically so that this
// cancels back out against keys_bg_hud/cps_bg_hud's own element "offset"
// (which is +KEYS_BG_OFFSET*size/+CPS_BG_OFFSET*size) -- see the comment
// on base_pos there for the full derivation. Net result: the real
// background panel's on-screen top-left lands exactly on (keys_x, keys_y)
// with no further adjustment needed, which is exactly what these two
// functions used to undo by shifting the preview rect by that same offset
// a second time here, moving it ~80-90px away from where the panel
// actually renders. They're identity functions now -- kept (instead of
// just calling s.get_rect() at every call site) so a search for
// "KEYS_BG_OFFSET"/"CPS_BG_OFFSET" still finds every place that offset is
// relevant, including here.
static core::rect<s32> getKeysVisualRect(const Sprite_ &s, f32 size)
{
    (void)size;
    return s.get_rect();
}
static core::rect<s32> getCpsVisualRect(const Sprite_ &s, f32 size)
{
    (void)size;
    return s.get_rect();
}

static void drawHudPreviewBox(video::IVideoDriver *driver, gui::IGUIFont *font,
        const core::rect<s32> &rect, const std::wstring &line1,
        const std::wstring &line2 = L"")
{
    ModernUI::panel(driver, rect, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    if (!font)
        return;

    s32 line_h = font->getDimension(L"Ag").Height;
    core::rect<s32> r1(rect.UpperLeftCorner.X + 4, rect.UpperLeftCorner.Y + 2,
        rect.LowerRightCorner.X - 4, rect.UpperLeftCorner.Y + 2 + line_h);
    font->draw(line1.c_str(), r1, video::SColor(255, 255, 255, 255), false, true);

    if (!line2.empty()) {
        core::rect<s32> r2(rect.UpperLeftCorner.X + 4, r1.LowerRightCorner.Y,
            rect.LowerRightCorner.X - 4, r1.LowerRightCorner.Y + line_h);
        font->draw(line2.c_str(), r2, video::SColor(200, 220, 220, 220), false, true);
    }
}

// Snap-to-grid overlay for "Move HUD" edit mode -- faint grid lines
// across the screen, same idea as CloakV4's HUD editor grid. Dragged
// elements snap to the intersections (see the EMIE_MOUSE_MOVED handling
// in OnEvent()); toggle with G, resize by scrolling over empty space,
// bypass for one drag by holding Alt. Drawn as thin line strips rather
// than a dot at every intersection -- O(screenW/grid + screenH/grid)
// draw calls instead of O((screenW/grid) * (screenH/grid)), which
// matters once the grid is set to something fine like 5-10px.
static void drawSnapGrid(video::IVideoDriver *driver, s32 screenW, s32 screenH, s32 grid_size)
{
    if (grid_size < 1)
        return;

    video::SColor line_color(35, 255, 255, 255);
    for (s32 x = 0; x <= screenW; x += grid_size)
        driver->draw2DRectangle(line_color, core::rect<s32>(x, 0, x + 1, screenH));
    for (s32 y = 0; y <= screenH; y += grid_size)
        driver->draw2DRectangle(line_color, core::rect<s32>(0, y, screenW, y + 1));
}

// Inventory HUD preview: mirrors Hud::drawInventoryHud()'s exact
// section/grid layout formulas (src/client/hud.cpp) -- same padding,
// same slot-size formula (hotbar image size * display density *
// hud_scaling), same per-section title height, same box-auto-sized-to-
// content sizing, and the same "Inventory" + inventory_hud_extra_lists
// section structure -- so edit mode matches real gameplay pixel-for-
// pixel *in layout*.
//
// The one thing this can't mirror exactly: real item *counts* per list,
// since there's no live inventory to read here in the menu (may not
// even be connected to a server yet). Counts below are representative
// placeholders (32 for the main inventory -- the most common default
// across games -- and 9, a classic 3x3 grid, for "craft"); the real HUD
// sizes to your actual inventory/crafting grid, which may be a
// different size on a given server, in which case the real box will be
// a different size than this preview.
static void drawInventoryHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font,
        Sprite_ &sprite)
{
    if (!font) {
        core::rect<s32> fallback = sprite.get_rect();
        ModernUI::panel(driver, fallback, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);
        return;
    }

    struct PreviewSection { std::wstring title; s32 count; };
    std::vector<PreviewSection> sections;
    sections.push_back({L"Inventory", 32});

    for (const std::string &raw_name : str_split(g_settings->get("inventory_hud_extra_lists"), ',')) {
        std::string name(trim(raw_name));
        if (name.empty() || name == "main")
            continue; // "main" is already the first section above
        std::wstring wtitle = utf8_to_wide(name);
        if (!wtitle.empty())
            wtitle[0] = towupper(wtitle[0]);
        s32 count = (name == "craft") ? 9 : 8; // best-effort guess for unknown list names
        sections.push_back({wtitle, count});
    }

    // Same formula as Hud::readScalingSetting() in src/client/hud.cpp,
    // further scaled by this HUD's own independent size multiplier (see
    // "inventory_hud_size" in Menu.cpp -- scroll over this HUD in "Move
    // HUD" edit mode to resize it).
    float inventory_hud_size = std::max(0.5f, std::min(2.5f, g_settings->getFloat("inventory_hud_size")));
    s32 slot = (s32)std::floor(HOTBAR_IMAGE_SIZE * RenderingEngine::getDisplayDensity() + 0.5f);
    slot = (s32)(slot * g_settings->getFloat("hud_scaling", 0.5f, 20.0f) * inventory_hud_size);
    const s32 slot_pad = slot / 12;
    const s32 pad = (s32)(8 * inventory_hud_size);
    const s32 title_h = font->getDimension(L"Ag").Height + pad;
    const s32 cell = slot + slot_pad * 2;

    s32 max_cols = 0;
    s32 box_h = pad * 2;
    for (PreviewSection &sec : sections) {
        s32 cols = std::min<s32>(8, sec.count);
        s32 rows = (sec.count + cols - 1) / cols;
        max_cols = std::max(max_cols, cols);
        box_h += title_h + rows * cell;
    }
    const s32 box_w = max_cols * cell + pad * 2;

    // Keep the drag hit-box in sync with what's actually drawn.
    sprite.width = box_w;
    sprite.height = box_h;
    core::rect<s32> box = sprite.get_rect();

    ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    s32 section_y = box.UpperLeftCorner.Y;
    for (PreviewSection &sec : sections) {
        core::rect<s32> title_rect(box.UpperLeftCorner.X + pad, section_y + pad / 2,
            box.LowerRightCorner.X - pad, section_y + title_h);
        font->draw(sec.title.c_str(), title_rect, video::SColor(255, 255, 255, 255), false, true);

        s32 cols = std::min<s32>(8, sec.count);
        s32 rows = (sec.count + cols - 1) / cols;
        for (s32 i = 0; i < sec.count; i++) {
            s32 col = i % cols;
            s32 row = i / cols;
            core::rect<s32> item_rect(
                box.UpperLeftCorner.X + pad + slot_pad + col * cell,
                section_y + title_h + pad + slot_pad + row * cell,
                box.UpperLeftCorner.X + pad + slot_pad + col * cell + slot,
                section_y + title_h + pad + slot_pad + row * cell + slot);
            ModernUI::panel(driver, item_rect, ModernUI::RadiusSmall, video::SColor(120, 60, 60, 60), video::SColor(160, 120, 120, 120), /*shadow=*/false);
        }

        section_y += title_h + rows * cell;
    }
}

// Craft HUD preview: mirrors Hud::drawCraftHud()'s exact layout formulas
// (src/client/hud.cpp) -- a "Craft" section (3x3, the classic crafting
// grid size) and a "Result" section (1 slot), using placeholder counts
// since there's no live inventory to read here in the menu. The real HUD
// only appears once something is actually in your craft grid; this
// preview always shows both sections so the drag target is visible.
static void drawCraftHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font,
        Sprite_ &sprite)
{
    if (!font) {
        core::rect<s32> fallback = sprite.get_rect();
        ModernUI::panel(driver, fallback, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);
        return;
    }

    struct PreviewSection { std::wstring title; s32 count; };
    std::vector<PreviewSection> sections = {
        {L"Craft", 9},
        {L"Result", 1},
    };

    // Same formula as Hud::readScalingSetting() in src/client/hud.cpp,
    // further scaled by this HUD's own independent size multiplier (see
    // "craft_hud_size" in Menu.cpp -- scroll over this HUD in "Move HUD"
    // edit mode to resize it).
    float craft_hud_size = std::max(0.5f, std::min(2.5f, g_settings->getFloat("craft_hud_size")));
    s32 slot = (s32)std::floor(HOTBAR_IMAGE_SIZE * RenderingEngine::getDisplayDensity() + 0.5f);
    slot = (s32)(slot * g_settings->getFloat("hud_scaling", 0.5f, 20.0f) * craft_hud_size);
    const s32 slot_pad = slot / 12;
    const s32 pad = (s32)(8 * craft_hud_size);
    const s32 title_h = font->getDimension(L"Ag").Height + pad;
    const s32 cell = slot + slot_pad * 2;

    s32 max_cols = 0;
    s32 box_h = pad * 2;
    for (PreviewSection &sec : sections) {
        s32 cols = std::min<s32>(8, sec.count);
        s32 rows = (sec.count + cols - 1) / cols;
        max_cols = std::max(max_cols, cols);
        box_h += title_h + rows * cell;
    }
    const s32 box_w = max_cols * cell + pad * 2;

    // Keep the drag hit-box in sync with what's actually drawn.
    sprite.width = box_w;
    sprite.height = box_h;
    core::rect<s32> box = sprite.get_rect();

    ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    s32 section_y = box.UpperLeftCorner.Y;
    for (PreviewSection &sec : sections) {
        core::rect<s32> title_rect(box.UpperLeftCorner.X + pad, section_y + pad / 2,
            box.LowerRightCorner.X - pad, section_y + title_h);
        font->draw(sec.title.c_str(), title_rect, video::SColor(255, 255, 255, 255), false, true);

        s32 cols = std::min<s32>(8, sec.count);
        s32 rows = (sec.count + cols - 1) / cols;
        for (s32 i = 0; i < sec.count; i++) {
            s32 col = i % cols;
            s32 row = i / cols;
            core::rect<s32> item_rect(
                box.UpperLeftCorner.X + pad + slot_pad + col * cell,
                section_y + title_h + pad + slot_pad + row * cell,
                box.UpperLeftCorner.X + pad + slot_pad + col * cell + slot,
                section_y + title_h + pad + slot_pad + row * cell + slot);
            ModernUI::panel(driver, item_rect, ModernUI::RadiusSmall, video::SColor(120, 60, 60, 60), video::SColor(160, 120, 120, 120), /*shadow=*/false);
        }

        section_y += title_h + rows * cell;
    }
}

// Music HUD preview: mirrors Hud::drawMusicHud()'s exact layout formulas
// (src/client/hud.cpp) -- same padding, art-size, text-column-width, and
// progress-bar-block formulas -- using sample "now playing" data, since
// there's no live track to read here in the menu.
static void drawMusicHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font, Sprite_ &sprite)
{
    // Combined global + per-element size multiplier, same as the real
    // Hud::drawMusicHud() -- see "hud_size"/"music_hud_size" in Menu.cpp.
    float size_mult = std::max(0.5f, std::min(2.5f, g_settings->getFloat("hud_size")))
        * std::max(0.5f, std::min(2.5f, g_settings->getFloat("music_hud_size")));
    gui::IGUIFont *scaled_font = g_fontengine->getFont(
        (unsigned int)(g_fontengine->getDefaultFontSize() * size_mult));
    if (scaled_font)
        font = scaled_font;

    if (!font) {
        core::rect<s32> fallback = sprite.get_rect();
        ModernUI::panel(driver, fallback, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);
        return;
    }

    const wchar_t *wline1 = L"[Spotify]";
    const wchar_t *wline2 = L"Song Title";
    const wchar_t *wline3 = L"Artist Name";
    const int sample_position_s = 41;
    const int sample_duration_s = 178;

    const s32 pad = (s32)(8 * size_mult);
    const s32 line_h = font->getDimension(L"Ag").Height;
    const int num_text_lines = 3;

    const s32 art_size = std::max<s32>(num_text_lines * line_h, 32);
    const s32 art_gap = pad;

    const s32 bar_h = 4;
    const s32 progress_block_h = pad / 2 + bar_h + 2 + line_h;

    const s32 content_h = std::max(art_size, num_text_lines * line_h);
    const s32 text_area_w = line_h * 9;
    const s32 box_w = art_size + art_gap + text_area_w + pad * 2;
    const s32 box_h = pad + content_h + progress_block_h + pad;

    // Keep the drag hit-box in sync with what's actually drawn.
    sprite.width = box_w;
    sprite.height = box_h;
    core::rect<s32> box = sprite.get_rect();

    ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    // Same Spotify-green used by the real HUD for a "Spotify" source.
    const video::SColor source_color(255, 30, 215, 96);
    const video::SColor artist_color(255, 200, 200, 200);

    s32 content_x = box.UpperLeftCorner.X + pad;

    // Placeholder album art square -- the real HUD shows actual fetched
    // album art here; this is just a plain fill at the exact same
    // size/position so the rest of the layout lines up correctly.
    core::rect<s32> art_rect(content_x, box.UpperLeftCorner.Y + pad,
        content_x + art_size, box.UpperLeftCorner.Y + pad + art_size);
    ModernUI::panel(driver, art_rect, ModernUI::Radius, video::SColor(200, 70, 70, 90), video::SColor(160, 140, 140, 160), /*shadow=*/false);
    content_x += art_size + art_gap;

    s32 y = box.UpperLeftCorner.Y + pad;
    s32 text_right = box.LowerRightCorner.X - pad;

    core::rect<s32> rect1(content_x, y, text_right, y + line_h);
    font->draw(wline1, rect1, source_color, false, true);
    y += line_h;

    core::rect<s32> rect2(content_x, y, text_right, y + line_h);
    font->draw(wline2, rect2, source_color, false, true);
    y += line_h;

    core::rect<s32> rect3(content_x, y, text_right, y + line_h);
    font->draw(wline3, rect3, artist_color, false, true);

    s32 bar_y = box.UpperLeftCorner.Y + pad + content_h + pad / 2;
    s32 bar_x = box.UpperLeftCorner.X + pad;
    s32 bar_w = box_w - pad * 2;

    core::rect<s32> bar_bg(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
    ModernUI::roundedRectFilled(driver, bar_bg, bar_h / 2, video::SColor(200, 60, 60, 60));

    f32 ratio = (f32)sample_position_s / (f32)sample_duration_s;
    core::rect<s32> bar_fill(bar_x, bar_y, bar_x + (s32)(bar_w * ratio), bar_y + bar_h);
    ModernUI::roundedRectFilled(driver, bar_fill, bar_h / 2, source_color);

    s32 time_y = bar_y + bar_h + 2;
    wchar_t wpos[16], wdur[16];
    swprintf(wpos, 16, L"%d:%02d", sample_position_s / 60, sample_position_s % 60);
    swprintf(wdur, 16, L"%d:%02d", sample_duration_s / 60, sample_duration_s % 60);
    s32 dur_w = font->getDimension(wdur).Width;

    core::rect<s32> pos_rect(bar_x, time_y, bar_x + bar_w / 2, time_y + line_h);
    font->draw(wpos, pos_rect, video::SColor(255, 190, 190, 190), false, true);

    core::rect<s32> dur_rect(bar_x + bar_w - dur_w, time_y, bar_x + bar_w, time_y + line_h);
    font->draw(wdur, dur_rect, video::SColor(255, 190, 190, 190), false, true);
}

// ShowRP preview: mirrors Hud::drawShowRp() (src/client/hud.cpp) exactly
// -- same padding/art-size/text-column-width formula, and critically the
// same FIXED 2-line layout (title + author always reserve their space,
// regardless of whether the current texture pack's metadata actually has
// them) -- that fixed layout is what keeps this box's size from ever
// drifting out of sync with the real one, the same bug KeyStroker/
// ShowCPS/Coords all had before their own size formulas were made
// exact-match rather than approximate. Sample placeholder text/art, since
// there's no live texture pack context to read here in the menu.
static void drawConsumptionHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font, Sprite_ &sprite)
{
    float size_mult = std::max(0.5f, std::min(2.5f, g_settings->getFloat("hud_size")))
        * std::max(0.5f, std::min(2.5f, g_settings->getFloat("consumption_hud_size")));
    gui::IGUIFont *scaled_font = g_fontengine->getFont(
        (unsigned int)(g_fontengine->getDefaultFontSize() * size_mult));
    if (scaled_font)
        font = scaled_font;

    if (!font) {
        core::rect<s32> fallback = sprite.get_rect();
        ModernUI::panel(driver, fallback, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);
        return;
    }

    // Matches Hud::drawConsumptionHud()'s formula (src/client/hud.cpp)
    // exactly -- fixed sample text at max width (3 segments, all 3 digits)
    // rather than live numbers, so this preview box is always the exact
    // same size as the real panel regardless of current RAM/CPU/GPU
    // usage.
    const wchar_t *wtext = L"RAM: 9999 MB  CPU: 100%  GPU: 100%";
    const s32 pad = (s32)(8 * size_mult);
    const s32 line_h = font->getDimension(L"Ay").Height;
    const s32 text_w = font->getDimension(wtext).Width;
    const s32 box_w = text_w + pad * 2;
    const s32 box_h = line_h + pad * 2;

    // Keep the drag hit-box in sync with what's actually drawn.
    sprite.width = box_w;
    sprite.height = box_h;
    core::rect<s32> box = sprite.get_rect();

    ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    core::rect<s32> text_rect(box.UpperLeftCorner.X + pad, box.UpperLeftCorner.Y + pad,
        box.LowerRightCorner.X - pad, box.LowerRightCorner.Y - pad);
    font->draw(wtext, text_rect, video::SColor(255, 220, 220, 220));
}


static void drawShowRpPreview(video::IVideoDriver *driver, gui::IGUIFont *font, Sprite_ &sprite)
{
    float size_mult = std::max(0.5f, std::min(2.5f, g_settings->getFloat("hud_size")))
        * std::max(0.5f, std::min(2.5f, g_settings->getFloat("rp_hud_size")));
    gui::IGUIFont *scaled_font = g_fontengine->getFont(
        (unsigned int)(g_fontengine->getDefaultFontSize() * size_mult));
    if (scaled_font)
        font = scaled_font;

    if (!font) {
        core::rect<s32> fallback = sprite.get_rect();
        ModernUI::panel(driver, fallback, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);
        return;
    }

    const wchar_t *wline1 = L"MyTexturePack";
    const wchar_t *wline2 = L"by PackAuthor";

    const s32 pad = (s32)(8 * size_mult);
    const s32 line_h = font->getDimension(L"Ag").Height;
    // Matches Hud::drawShowRp()'s formula (src/client/hud.cpp) exactly --
    // 2 lines, no reserved progress-bar space (there's nothing to show
    // progress of here, unlike NowPlaying) -- so this preview box is
    // always the exact same size as the real panel.
    const int num_text_lines = 2;

    const s32 art_size = std::max<s32>(num_text_lines * line_h, 32);
    const s32 art_gap = pad;

    const s32 content_h = std::max(art_size, num_text_lines * line_h);
    const s32 text_area_w = line_h * 9;
    const s32 box_w = art_size + art_gap + text_area_w + pad * 2;
    const s32 box_h = pad + content_h + pad;

    // Keep the drag hit-box in sync with what's actually drawn.
    sprite.width = box_w;
    sprite.height = box_h;
    core::rect<s32> box = sprite.get_rect();

    ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(160, 20, 20, 20), getHudPreviewBorderColor(), /*shadow=*/false);

    const video::SColor title_color(255, 220, 220, 220);
    const video::SColor author_color(255, 170, 170, 170);

    s32 content_x = box.UpperLeftCorner.X + pad;

    // Placeholder screenshot square -- same reasoning as MusicHud's
    // placeholder album art above.
    core::rect<s32> art_rect(content_x, box.UpperLeftCorner.Y + pad,
        content_x + art_size, box.UpperLeftCorner.Y + pad + art_size);
    ModernUI::panel(driver, art_rect, ModernUI::Radius, video::SColor(200, 70, 70, 90), video::SColor(160, 140, 140, 160), /*shadow=*/false);
    content_x += art_size + art_gap;

    s32 y = box.UpperLeftCorner.Y + pad;
    s32 text_right = box.LowerRightCorner.X - pad;

    core::rect<s32> rect1(content_x, y, text_right, y + line_h);
    font->draw(wline1, rect1, title_color, false, true);
    y += line_h;

    core::rect<s32> rect2(content_x, y, text_right, y + line_h);
    font->draw(wline2, rect2, author_color, false, true);
}

// Photo HUD preview: shows whichever built-in photo is currently
// selected via "photo_hud_image" (see src/client/photohud.h's
// PhotoHudBuiltinImages table -- same table PhotoHudSettingsMenu's own
// picker buttons are built from), scaled into the drag box -- same fit
// logic as PhotoHud::draw() in src/client/photohud.cpp. Doesn't attempt
// to preview a custom image (unlike the real HUD element/the settings
// panel's own preview) -- this is just a quick drag-positioning aid, and
// keeping it to a cheap table lookup (no filesystem access) keeps it
// that way.
static void drawPhotoHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font,
        Sprite_ &sprite, ITextureSource *tsrc, s32 screenW, s32 screenH)
{
    video::ITexture *tex = nullptr;
    if (tsrc) {
        std::string selected = g_settings->exists("photo_hud_image") ?
            g_settings->get("photo_hud_image") : "face";
        const char *texture_filename = PhotoHudBuiltinImages.front().texture_filename;
        for (const auto &img : PhotoHudBuiltinImages) {
            if (selected == img.settings_key) {
                texture_filename = img.texture_filename;
                break;
            }
        }
        tex = tsrc->getTexture(texture_filename);
    }

    if (tex) {
        core::dimension2du imgsize = tex->getOriginalSize();
        if (imgsize.Width > 0 && imgsize.Height > 0) {
            // Must match Hud::drawPhotoHud()'s sizing exactly: fit the
            // image within a max_dim x max_dim box (scaled by the global
            // "hud_size" multiplier, same as every other MineBoost HUD
            // element), keeping the image's own aspect ratio. The sprite
            // used to be a fixed max_dim x max_dim SQUARE regardless of
            // the image's actual shape, which for a non-square photo
            // (e.g. a tall phone screenshot) left a big chunk of the box
            // as bare fill color -- white by default (hud_color_photo),
            // which is the "totally white" preview area. Keep the drag
            // hit-box in sync with what's actually drawn, same pattern as
            // the Inventory/Craft HUD previews above.
            float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f);
            s32 max_dim = std::max<s32>(16, (s32)(g_settings->getS32("photo_hud_size") * hud_size));
            float scale = std::min(
                (float)max_dim / (float)imgsize.Width,
                (float)max_dim / (float)imgsize.Height);
            sprite.width = std::max<s32>(1, (s32)(imgsize.Width * scale));
            sprite.height = std::max<s32>(1, (s32)(imgsize.Height * scale));
        }

        core::rect<s32> box = sprite.get_rect();

        // Only the outline is user-colorable via "hud_color_photo" (see
        // the "Colors" panel) -- fixed fill, matching
        // Hud::drawPhotoHud()'s drawHudColorPanel() in src/client/hud.cpp,
        // and the photo itself is drawn at its original colors (no tint).
        // Shadow off, same reason as everywhere else in this file: a raw
        // ModernUI::dropShadow() used to be called here and smeared past
        // the corners of a box this small.
        v3f c = g_settings->getV3F("hud_color_photo").value_or(v3f(255, 255, 255));
        video::SColor border(255,
            rangelim((s32)myround(c.X), 0, 255),
            rangelim((s32)myround(c.Y), 0, 255),
            rangelim((s32)myround(c.Z), 0, 255));
        ModernUI::panel(driver, box, ModernUI::Radius,
            video::SColor(190, 22, 24, 30), border, /*shadow=*/false);
        if (imgsize.Width > 0 && imgsize.Height > 0) {
            core::rect<s32> src(0, 0, imgsize.Width, imgsize.Height);
            driver->draw2DImage(tex, box, src, nullptr, nullptr, true);
        }
        return;
    }

    drawHudPreviewBox(driver, font, sprite.get_rect(), L"PhotoHUD");
}


// ArmorHUD temporarily disabled -- preview drawer commented out below.
#if 0
// Armor HUD preview: a single row of empty slots with a durability
// readout beneath, so the user can see roughly how it will look without
// needing real armor equipped.
static void drawArmorHudPreview(video::IVideoDriver *driver, gui::IGUIFont *font,
        const core::rect<s32> &rect)
{
    ModernUI::panel(driver, rect, ModernUI::Radius, video::SColor(160, 20, 20, 20), ModernUI::PanelBorder, /*shadow=*/false);

    const s32 pad = 8;
    const s32 line_h = font ? font->getDimension(L"Ag").Height : 12;
    const s32 title_h = line_h + pad;
    const s32 durability_h = line_h + pad / 2;
    if (font) {
        core::rect<s32> title_rect(rect.UpperLeftCorner.X + pad, rect.UpperLeftCorner.Y + pad / 2,
            rect.LowerRightCorner.X - pad, rect.UpperLeftCorner.Y + title_h);
        font->draw(L"Armor", title_rect, video::SColor(255, 255, 255, 255), false, true);
    }

    const s32 cols = 6;
    const s32 avail_w = rect.getWidth() - pad * 2;
    const s32 avail_h = rect.getHeight() - title_h - durability_h - pad * 2;
    if (avail_w <= 0 || avail_h <= 0)
        return;
    const s32 cell = std::min<s32>(avail_w / cols, avail_h);
    if (cell <= 4)
        return;
    const s32 slot_pad = cell / 8;

    for (s32 col = 0; col < cols; col++) {
        core::rect<s32> slot(
            rect.UpperLeftCorner.X + pad + col * cell + slot_pad,
            rect.UpperLeftCorner.Y + title_h + pad + slot_pad,
            rect.UpperLeftCorner.X + pad + (col + 1) * cell - slot_pad,
            rect.UpperLeftCorner.Y + title_h + pad + cell - slot_pad);
        ModernUI::panel(driver, slot, ModernUI::RadiusSmall, video::SColor(120, 60, 60, 60), video::SColor(160, 120, 120, 120), /*shadow=*/false);

        if (font) {
            core::rect<s32> pct_rect(
                rect.UpperLeftCorner.X + pad + col * cell,
                rect.UpperLeftCorner.Y + title_h + pad + cell,
                rect.UpperLeftCorner.X + pad + (col + 1) * cell,
                rect.UpperLeftCorner.Y + title_h + pad + cell + durability_h);
            font->draw(L"100%", pct_rect, video::SColor(255, 60, 200, 60), true, true);
        }
    }
}
#endif // ArmorHUD disabled

void drawBackground(video::IVideoDriver* driver, s32 screenW, s32 screenH) {
    s32 x = (screenW - WIDTH_) / 2;
    s32 y = (screenH - HEIGHT_) / 2;
    core::rect<s32> winRect(x, y, x + WIDTH_, y + HEIGHT_);

    ModernUI::panel(driver, winRect, ModernUI::Radius,
            video::SColor(200, 22, 24, 30), getMineBoostGuiColor(),
            /*shadow=*/true, /*borderThickness=*/2);

    s32 lineOffsetX = 190;
    s32 lineX = x + lineOffsetX;
    s32 lineYStart = y + ModernUI::Radius;
    s32 lineYEnd = y + HEIGHT_ - ModernUI::Radius;

    // Soft translucent category divider (was a flat 1px grey line).
    driver->draw2DRectangle(video::SColor(90, 255, 255, 255),
            core::rect<s32>(lineX, lineYStart, lineX + 1, lineYEnd));
}

void Menu::updateScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH)
{
    if (!scrollbar) return;
    const int SCROLL_WIDTH = 300;
    const int SCROLL_HEIGHT = 20;
    const int TOP_OFFSET = 25; // 1st row: FOV
    int bgLeft = (screenW - WIDTH_) / 2;
    int bgTop = (screenH - HEIGHT_) / 2;
    int left = bgLeft + (WIDTH_ - SCROLL_WIDTH) / 2;
    int top = bgTop + TOP_OFFSET;
    int right = left + SCROLL_WIDTH;
    int bottom = top + SCROLL_HEIGHT;
    scrollbarTop = top;

    scrollbar->setRelativePosition(core::rect<s32>(left, top, right, bottom));
}

void Menu::updateFpsScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH)
{
    if (!scrollbar) return;
    const int SCROLL_WIDTH = 300;
    const int SCROLL_HEIGHT = 20;
    const int TOP_OFFSET = 65; // 2nd row: FPS
    int bgLeft = (screenW - WIDTH_) / 2;
    int bgTop = (screenH - HEIGHT_) / 2;
    int left = bgLeft + (WIDTH_ - SCROLL_WIDTH) / 2;
    int top = bgTop + TOP_OFFSET;
    int right = left + SCROLL_WIDTH;
    int bottom = top + SCROLL_HEIGHT;
    fpsScrollbarTop = top;

    scrollbar->setRelativePosition(core::rect<s32>(left, top, right, bottom));
}

void Menu::updateHitParticleScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH)
{
    if (!scrollbar) return;
    const int SCROLL_WIDTH = 300;
    const int SCROLL_HEIGHT = 20;
    const int TOP_OFFSET = 145; // 4th row: Hit Particles
    int bgLeft = (screenW - WIDTH_) / 2;
    int bgTop = (screenH - HEIGHT_) / 2;
    int left = bgLeft + (WIDTH_ - SCROLL_WIDTH) / 2;
    int top = bgTop + TOP_OFFSET;
    int right = left + SCROLL_WIDTH;
    int bottom = top + SCROLL_HEIGHT;
    hitparticleScrollbarTop = top;

    scrollbar->setRelativePosition(core::rect<s32>(left, top, right, bottom));
}

void Menu::updateTargetParticleScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH)
{
    if (!scrollbar) return;
    const int SCROLL_WIDTH = 300;
    const int SCROLL_HEIGHT = 20;
    const int TOP_OFFSET = 105; // 3rd row: Target Particles
    int bgLeft = (screenW - WIDTH_) / 2;
    int bgTop = (screenH - HEIGHT_) / 2;
    int left = bgLeft + (WIDTH_ - SCROLL_WIDTH) / 2;
    int top = bgTop + TOP_OFFSET;
    int right = left + SCROLL_WIDTH;
    int bottom = top + SCROLL_HEIGHT;
    targetParticleScrollbarTop = top;

    scrollbar->setRelativePosition(core::rect<s32>(left, top, right, bottom));
}

void Menu::updateHudSizeScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH)
{
    if (!scrollbar) return;
    const int SCROLL_WIDTH = 300;
    const int SCROLL_HEIGHT = 20;
    const int TOP_OFFSET = 185; // 5th row: HUD Size
    int bgLeft = (screenW - WIDTH_) / 2;
    int bgTop = (screenH - HEIGHT_) / 2;
    int left = bgLeft + (WIDTH_ - SCROLL_WIDTH) / 2;
    int top = bgTop + TOP_OFFSET;
    int right = left + SCROLL_WIDTH;
    int bottom = top + SCROLL_HEIGHT;
    hudSizeScrollbarTop = top;

    scrollbar->setRelativePosition(core::rect<s32>(left, top, right, bottom));
}

Menu::Menu(gui::IGUIEnvironment* env,
    gui::IGUIElement* parent,
    s32 id, IMenuManager* menumgr,
    Client* client)
    : IGUIElement(gui::EGUIET_ELEMENT, env, parent, id,
    core::rect<s32>(0, 0, 0, 0)),
    m_menumgr(menumgr),
    m_client(client),
    env(env), driver(env->getVideoDriver())
{
    s_instance = this;

    screenW = driver->getScreenSize().Width;
    screenH = driver->getScreenSize().Height;
    this->parent = parent;

    scrollbar = env->addScrollBar(true, core::rect<s32>((screenW - 300) / 2 + (-45), screenH - 90 + (-85),
    (screenW + 300) / 2 + (-45), screenH - 70 + (-85)), nullptr, 105);
    scrollbar->setMax(160);
    scrollbar->setMin(75);
    scrollbar->setPos(g_settings->getFloat("fov_custom.data"));
    scrollbar->setVisible(false);

    fps_scrollbar = env->addScrollBar(true, core::rect<s32>((screenW - 300) / 2 + (-45), screenH - 130 + (-85),
        (screenW + 300) / 2 + (-45), screenH - 110 + (-85)), nullptr, 106);
    fps_scrollbar->setMax(1000);
    fps_scrollbar->setMin(1);
    fps_scrollbar->setSmallStep(1);
    fps_scrollbar->setLargeStep(1);
    fps_scrollbar->setPos(g_settings->getFloat("fps_max"));
    fps_scrollbar->setVisible(false);

    // Amount of particles spawned when actually hitting a block/entity
    // (register_hits() in builtin/client/init.lua) -- distinct from the
    // continuous target-highlight (ESP) particles below.
    hitparticle_scrollbar = env->addScrollBar(true, core::rect<s32>((screenW - 300) / 2 + (-45), screenH - 170 + (-85),
        (screenW + 300) / 2 + (-45), screenH - 150 + (-85)), nullptr, 107);
    hitparticle_scrollbar->setMax(300);
    hitparticle_scrollbar->setMin(1);
    hitparticle_scrollbar->setSmallStep(1);
    hitparticle_scrollbar->setLargeStep(1);
    hitparticle_scrollbar->setPos(g_settings->getS32("hit_particle_amount"));
    hitparticle_scrollbar->setVisible(false);

    // Continuous target-highlight (ESP) particles shown on the locked
    // target -- see MineBoostPresence/target lock code, unrelated to the
    // hit-impact particles above.
    target_particle_scrollbar = env->addScrollBar(true, core::rect<s32>((screenW - 300) / 2 + (-45), screenH - 210 + (-85),
        (screenW + 300) / 2 + (-45), screenH - 190 + (-85)), nullptr, 108);
    target_particle_scrollbar->setMax(50);
    target_particle_scrollbar->setMin(1);
    target_particle_scrollbar->setSmallStep(1);
    target_particle_scrollbar->setLargeStep(1);
    target_particle_scrollbar->setPos(g_settings->getS32("target_highlight_particle_amount"));
    target_particle_scrollbar->setVisible(false);

    // Global size multiplier for MineBoost's custom HUD elements (coords,
    // FPS, ping, KeyStroker, Music HUD, Target HUD, Photo HUD). Stored as
    // a percentage (50-250%) on the slider, converted to the 0.5-2.5
    // "hud_size" float when read/written.
    hud_size_scrollbar = env->addScrollBar(true, core::rect<s32>((screenW - 300) / 2 + (-45), screenH - 250 + (-85),
        (screenW + 300) / 2 + (-45), screenH - 230 + (-85)), nullptr, 109);
    hud_size_scrollbar->setMax(250);
    hud_size_scrollbar->setMin(50);
    hud_size_scrollbar->setSmallStep(1);
    hud_size_scrollbar->setLargeStep(1);
    hud_size_scrollbar->setPos((s32)(g_settings->getFloat("hud_size") * 100));
    hud_size_scrollbar->setVisible(false);

    // "Photo HUD" advanced-settings panel -- right-clicking the PhotoHUD
    // tile opens a small image picker over the main settings list (see
    // PhotoHudSettingsMenu.h/.cpp, and the "photo_panel.isOpen()"/
    // "photo_panel.onEvent()"/"photo_panel.draw()" calls in this file).
    photo_panel.init(env, this, client);

    // "HandView" picker panel -- right-clicking the HandView tile opens a
    // swing-style picker plus 3 embedded offset/scale sliders (see
    // openHandViewSettings()/closeHandViewSettings() and the
    // handview_settings_open handling in OnEvent()/draw()).
    handview_settings_close_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Close");
    handview_settings_close_button.setColor(video::SColor(180, 20, 20, 20));
    handview_settings_close_button.setOnClick([this]() { closeHandViewSettings(); });

    handview_pick_vanilla_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Vanilla");
    handview_pick_vanilla_button.setOnClick([this]() { g_settings->set("hand_anim_style", "vanilla"); });

    handview_pick_static_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Static");
    handview_pick_static_button.setOnClick([this]() { g_settings->set("hand_anim_style", "static"); });

    handview_pick_fast_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Fast");
    handview_pick_fast_button.setOnClick([this]() { g_settings->set("hand_anim_style", "fast"); });

    handview_pick_sway_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Sway");
    handview_pick_sway_button.setOnClick([this]() { g_settings->set("hand_anim_style", "sway"); });

    handview_pick_chime_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Chime");
    handview_pick_chime_button.setOnClick([this]() { g_settings->set("hand_anim_style", "chime"); });

    handview_pick_old_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Old");
    handview_pick_old_button.setOnClick([this]() { g_settings->set("hand_anim_style", "old"); });

    handview_pick_punch_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Punch");
    handview_pick_punch_button.setOnClick([this]() { g_settings->set("hand_anim_style", "punch"); });

    handview_pick_tilt_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Tilt");
    handview_pick_tilt_button.setOnClick([this]() { g_settings->set("hand_anim_style", "tilt"); });

    // "изменение руки" -- this is the only place that actually lets the
    // player swap which side the hand/wielditem renders on from the
    // HandView panel. There used to be no UI control for it at all here
    // (only the F key / keymap_toggle_left_hand did anything), which is
    // why it looked broken from inside this menu.
    handview_left_hand_button.setOnClick([this]() {
        bool cur = g_settings->getBool("left_hand");
        g_settings->setBool("left_hand", !cur);
    });

    // Suppresses only the idle walking wield-bob (see the "no_view_bob"
    // check in the wielded-item block of Camera::update() in
    // src/client/camera.cpp) -- attack/dig swings and item-use
    // animations are untouched.
    handview_no_view_bob_button.setOnClick([this]() {
        bool cur = g_settings->getBool("no_view_bob");
        g_settings->setBool("no_view_bob", !cur);
    });

    handview_offset_x_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 110);
    handview_offset_x_scrollbar->setMax(100);
    handview_offset_x_scrollbar->setMin(-100);
    handview_offset_x_scrollbar->setSmallStep(1);
    handview_offset_x_scrollbar->setLargeStep(1);
    handview_offset_x_scrollbar->setPos((s32)g_settings->getFloat("handview_offset_x"));
    handview_offset_x_scrollbar->setVisible(false);

    handview_offset_y_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 111);
    handview_offset_y_scrollbar->setMax(100);
    handview_offset_y_scrollbar->setMin(-100);
    handview_offset_y_scrollbar->setSmallStep(1);
    handview_offset_y_scrollbar->setLargeStep(1);
    handview_offset_y_scrollbar->setPos((s32)g_settings->getFloat("handview_offset_y"));
    handview_offset_y_scrollbar->setVisible(false);

    handview_offset_z_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 113);
    handview_offset_z_scrollbar->setMax(100);
    handview_offset_z_scrollbar->setMin(-100);
    handview_offset_z_scrollbar->setSmallStep(1);
    handview_offset_z_scrollbar->setLargeStep(1);
    handview_offset_z_scrollbar->setPos((s32)g_settings->getFloat("handview_offset_z"));
    handview_offset_z_scrollbar->setVisible(false);

    handview_scale_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 112);
    handview_scale_scrollbar->setMax(300);
    handview_scale_scrollbar->setMin(30);
    handview_scale_scrollbar->setSmallStep(1);
    handview_scale_scrollbar->setLargeStep(1);
    handview_scale_scrollbar->setPos((s32)(g_settings->getFloat("handview_scale") * 100));
    handview_scale_scrollbar->setVisible(false);

    // "Colors" panel -- lets the player pick a color for each HUD element
    // and the chat background (see openColorsPanel()/closeColorsPanel()
    // and getColorTargets() below).
    colors_settings_close_button.addButton(core::rect<s32>(0, 0, 10, 10), L"Close");
    colors_settings_close_button.setColor(video::SColor(180, 20, 20, 20));
    colors_settings_close_button.setOnClick([this]() { closeColorsPanel(); });

    {
        std::vector<ColorTarget> targets = getColorTargets();
        colors_target_buttons.clear();
        for (size_t i = 0; i < targets.size(); i++) {
            Button b;
            b.addButton(core::rect<s32>(0, 0, 10, 10), targets[i].label);
            b.setOnClick([this, i]() { selectColorsTarget(i); });
            colors_target_buttons.push_back(b);
        }
    }

    colors_r_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 120);
    colors_g_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 121);
    colors_b_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 122);
    colors_a_scrollbar = env->addScrollBar(true, core::rect<s32>(0, 0, 10, 10), nullptr, 123);
    IGUIScrollBar *rgba_scrollbars[] = {colors_r_scrollbar, colors_g_scrollbar,
        colors_b_scrollbar, colors_a_scrollbar};
    for (IGUIScrollBar *sb : rgba_scrollbars) {
        sb->setMax(255);
        sb->setMin(0);
        sb->setSmallStep(1);
        sb->setLargeStep(1);
        sb->setVisible(false);
    }

	coords_sprite.width = (s32)(140 * g_settings->getFloat("coords_size"));
	coords_sprite.height = (s32)(30 * g_settings->getFloat("coords_size"));
    if (!g_settings->exists("coords_sprite")) {
        coords_sprite.x = 5;
        coords_sprite.y = (Environment->getVideoDriver()->getScreenSize().Height - coords_sprite.height / 2 + g_fontengine->getTextHeight() - coords_sprite.height);
    } else {
        v2f data = g_settings->getV2F("coords_sprite");
        coords_sprite.x = data[0];
        coords_sprite.y = data[1];
    }

    fov_sprite.width = (s32)(140 * g_settings->getFloat("fps_size"));
    fov_sprite.height = (s32)(30 * g_settings->getFloat("fps_size"));
    if (!g_settings->exists("fov_coords")) {
        fov_sprite.x = 5;
        fov_sprite.y = (Environment->getVideoDriver()->getScreenSize().Height - coords_sprite.height / 2 + g_fontengine->getTextHeight() - coords_sprite.height);
    } else {
        v2f fov_data = g_settings->getV2F("fov_coords");
        fov_sprite.x = fov_data[0];
        fov_sprite.y = fov_data[1];
    }

    // Ping HUD: sits one line below the FPS HUD by default (see
    // GameUI::update in src/client/gameui.cpp, "show_ping").
    ping_sprite.width = (s32)(140 * g_settings->getFloat("ping_size"));
    ping_sprite.height = (s32)(30 * g_settings->getFloat("ping_size"));
    if (!g_settings->exists("ping_coords")) {
        ping_sprite.x = 5;
        ping_sprite.y = fov_sprite.y - ping_sprite.height;
    } else {
        v2f ping_data = g_settings->getV2F("ping_coords");
        ping_sprite.x = ping_data[0];
        ping_sprite.y = ping_data[1];
    }

	chat.width = 1200;
	chat.height = 200;
	chat.x = g_settings->getS32("chat_x");
	chat.y = g_settings->getS32("chat_y");

    // KeyStroker/ShowCPS box size AND position are derived to exactly
    // match "keys_panel_bg.png"/"cps_panel_bg.png"'s real on-screen
    // bounds in builtin/client/keystroker.lua:
    //   - "keys_x"/"keys_y" ("cps_x"/"cps_y") are defined there as the
    //     background image's own top-left corner -- see the
    //     "KEYS_BG_OFFSET*size" comment in update_hud_positions()/
    //     update_cps_hud_position() there, which cancels out exactly for
    //     the bg image itself (only the individual key icons end up
    //     offset from it).
    //   - Its rendered size is native_texture_size * BG_BASE_SCALE(2) *
    //     get_hud_size()/get_cps_hud_size(). keys_panel_bg.png is 80x80,
    //     cps_panel_bg.png is 90x27 (textures/base/pack/*.png) -- so
    //     160x160 and 180x54 respectively at size 1.0.
    // A hand-tuned 132x150 / 132x36 that didn't match either the image's
    // aspect ratio or its absolute size is what made the preview box
    // visibly drift apart from the real HUD even after "hud_size" was
    // accounted for. Kept in sync every frame in draw() below too, since
    // "hud_size"/"keys_size"/"cps_size" can all change live via their
    // scrollbars while this menu is open.
    f32 keys_hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
        rangelim(g_settings->getFloat("keys_size"), 0.5f, 2.5f);
    keystr.width = (s32)(160 * keys_hud_size);
    keystr.height = (s32)(160 * keys_hud_size);
    keystr.x = g_settings->getS32("keys_x");
    keystr.y = g_settings->getS32("keys_y");

    // ShowCPS: same derivation as KeyStroker above, just with
    // cps_panel_bg.png's own native size (90x27) instead.
    f32 cps_hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
        rangelim(g_settings->getFloat("cps_size"), 0.5f, 2.5f);
    cps_sprite.width = (s32)(180 * cps_hud_size);
    cps_sprite.height = (s32)(54 * cps_hud_size);
    cps_sprite.x = g_settings->getS32("cps_x");
    cps_sprite.y = g_settings->getS32("cps_y");

    music_sprite.width = 220;
    music_sprite.height = 50;
    music_sprite.x = g_settings->getS32("music_hud_x");
    music_sprite.y = g_settings->getS32("music_hud_y");
    if (music_sprite.x < 0) {
        music_sprite.x = Environment->getVideoDriver()->getScreenSize().Width - music_sprite.width - 10;
    }

    rp_sprite.width = 220;
    rp_sprite.height = 50;
    rp_sprite.x = g_settings->getS32("rp_hud_x");
    rp_sprite.y = g_settings->getS32("rp_hud_y");
    if (rp_sprite.x < 0) {
        rp_sprite.x = Environment->getVideoDriver()->getScreenSize().Width - rp_sprite.width - 10;
    }

    // Width is a rough estimate here -- drawConsumptionHudPreview() below
    // recomputes it every frame from the actual fixed sample text and
    // font size, same "keep the drag hit-box in sync with what's really
    // drawn" pattern as the Coords/PhotoHUD previews use.
    consumption_sprite.width = 320;
    consumption_sprite.height = 40;
    consumption_sprite.x = g_settings->getS32("consumption_hud_x");
    consumption_sprite.y = g_settings->getS32("consumption_hud_y");
    if (consumption_sprite.x < 0) {
        consumption_sprite.x = 10;
    }

    target_hud_sprite.width = (s32)(160 * g_settings->getFloat("target_hud_size"));
    target_hud_sprite.height = (s32)(50 * g_settings->getFloat("target_hud_size"));
    target_hud_sprite.x = g_settings->getS32("target_hud_x");
    target_hud_sprite.y = g_settings->getS32("target_hud_y");
    if (target_hud_sprite.x < 0) {
        target_hud_sprite.x = Environment->getVideoDriver()->getScreenSize().Width / 2 - target_hud_sprite.width / 2;
    }
    if (target_hud_sprite.y < 0) {
        target_hud_sprite.y = (s32)(Environment->getVideoDriver()->getScreenSize().Height * 0.16f);
    }

    // Inventory HUD: shows the player's full inventory on-screen at all
    // times (see Hud::drawInventoryHud in src/client/hud.cpp). Default
    // box size is just a starting point for the drag outline; the actual
    // drawn size follows the inventory's item count/columns.
    inventory_hud_sprite.width = 440;
    inventory_hud_sprite.height = 220;
    inventory_hud_sprite.x = g_settings->getS32("inventory_hud_x");
    inventory_hud_sprite.y = g_settings->getS32("inventory_hud_y");
    if (inventory_hud_sprite.x < 0) {
        inventory_hud_sprite.x = Environment->getVideoDriver()->getScreenSize().Width / 2 - inventory_hud_sprite.width / 2;
    }
    if (inventory_hud_sprite.y < 0) {
        inventory_hud_sprite.y = Environment->getVideoDriver()->getScreenSize().Height / 2 - inventory_hud_sprite.height / 2;
    }

    // Craft HUD: shows only the crafting grid + result (see
    // Hud::drawCraftHud in src/client/hud.cpp). Default position sits to
    // the right of InventoryHud's default centered position so the two
    // don't overlap out of the box.
    craft_hud_sprite.width = 220;
    craft_hud_sprite.height = 220;
    craft_hud_sprite.x = g_settings->getS32("craft_hud_x");
    craft_hud_sprite.y = g_settings->getS32("craft_hud_y");
    if (craft_hud_sprite.x < 0) {
        craft_hud_sprite.x = Environment->getVideoDriver()->getScreenSize().Width / 2 + 40;
    }
    if (craft_hud_sprite.y < 0) {
        craft_hud_sprite.y = Environment->getVideoDriver()->getScreenSize().Height / 2 - craft_hud_sprite.height / 2;
    }

    // Photo HUD preview box (see Hud::drawPhotoHud in src/client/hud.cpp).
    // Size mirrors the "photo_hud_size" setting (max dimension of the
    // actual image once loaded); default position is screen center, same
    // fallback as the real HUD uses when never dragged.
    {
        s32 photo_dim = std::max<s32>(16, g_settings->getS32("photo_hud_size"));
        photo_sprite.width = photo_dim;
        photo_sprite.height = photo_dim;
        photo_sprite.x = g_settings->getS32("photo_hud_x");
        photo_sprite.y = g_settings->getS32("photo_hud_y");
        if (photo_sprite.x < 0) {
            photo_sprite.x = Environment->getVideoDriver()->getScreenSize().Width / 2 - photo_sprite.width / 2;
        }
        if (photo_sprite.y < 0) {
            photo_sprite.y = Environment->getVideoDriver()->getScreenSize().Height / 2 - photo_sprite.height / 2;
        }
    }

    // Armor HUD temporarily disabled -- init block commented out below.
    // armor_hud_sprite.width = 360;
    // armor_hud_sprite.height = 110;
    // armor_hud_sprite.x = g_settings->getS32("armor_hud_x");
    // armor_hud_sprite.y = g_settings->getS32("armor_hud_y");
    // if (armor_hud_sprite.x < 0) {
    //     armor_hud_sprite.x = Environment->getVideoDriver()->getScreenSize().Width / 2 - armor_hud_sprite.width / 2;
    // }
    // if (armor_hud_sprite.y < 0) {
    //     armor_hud_sprite.y = (s32)(Environment->getVideoDriver()->getScreenSize().Height * 0.6f);
    // }

    {
        s32 sw = Environment->getVideoDriver()->getScreenSize().Width;
        s32 sh = Environment->getVideoDriver()->getScreenSize().Height;
        const s32 bw = 130, bh = 32, margin = 10;
        hud_move_button.addButton(
            core::rect<s32>(sw - bw - margin, sh - bh - margin, sw - margin, sh - margin),
            stringToWString("Move HUD"));
        hud_move_button.setColor(video::SColor(180, 20, 20, 20));
        hud_move_button.setVisible(true);
        hud_move_button.setOnClick([this]() {
            editMode = !editMode;
            if (editMode && colors_panel_open)
                closeColorsPanel();
        });

        // Sits just above "Move HUD" in the same bottom-right corner.
        colors_open_button.addButton(
            core::rect<s32>(sw - bw - margin, sh - bh - margin - bh - margin, sw - margin, sh - margin - bh - margin),
            stringToWString("Colors"));
        colors_open_button.setColor(video::SColor(180, 20, 20, 20));
        colors_open_button.setVisible(true);
        colors_open_button.setOnClick([this]() {
            if (colors_panel_open)
                closeColorsPanel();
            else
                openColorsPanel();
        });
    }

    initCategoryButtons();
}

void Menu::ItemsInit(SettingCategory category)
{
    std::vector<Setting> settings = getSettings();
    s32 x = (screenW - WIDTH_) / 2;
    s32 y = (screenH - HEIGHT_) / 2;

    int startPosX = 190 + 25;
    int startPosY = y + 25;
    const int itemWidth = 120;
    const int itemHeight = 120;
    const int spacing = 15;
    items.clear();

    // Scrollbars are dedicated widgets, not regular toggle/edit items, so
    // their visibility is driven directly by the selected category instead
    // of piggybacking on whether any matching item happens to exist in the
    // loop below (which would never run at all for a category with no
    // plain items in it).
    bool show_scrollbars = (category == SettingCategory::Scrollbars);
    scrollbar->setVisible(show_scrollbars);
    fps_scrollbar->setVisible(show_scrollbars);
    hitparticle_scrollbar->setVisible(show_scrollbars);
    target_particle_scrollbar->setVisible(show_scrollbars);
    hud_size_scrollbar->setVisible(show_scrollbars);

    for (size_t i = 0; i < settings.size(); ++i) {
        if (settings[i].category == category) {
            int posX = x + startPosX + (items.size() % 4) * (itemWidth + spacing);
            int posY = startPosY + (items.size() / 4) * (itemHeight + spacing);

            Items it(core::rect<s32>(posX, posY, posX + itemWidth, posY + itemHeight));
            it.set_title(stringToWString(settings[i].name));
            it.set_setting(settings[i].value);
            it.setSetting(settings[i]);
            if (settings[i].value == "photo_hud")
                it.set_has_advanced_settings(true);
            if (settings[i].value == "handview_enabled")
                it.set_has_advanced_settings(true);
            items.push_back(it);
        }
    }
}

void Menu::onCategoryButtonClick(SettingCategory category)
{
    this->current_category = category;
    ItemsInit(category);
    updateCategoryButtonActiveStates();
}

void Menu::updateCategoryButtonActiveStates()
{
    // Must match the exact push order in initCategoryButtons() below.
    static const SettingCategory order[4] = {
        SettingCategory::GUI, SettingCategory::RENDER,
        SettingCategory::MISC, SettingCategory::Scrollbars
    };
    for (size_t i = 0; i < buttons.size() && i < 4; i++)
        buttons[i].setActive(order[i] == current_category);
}

void Menu::initCategoryButtons()
{
    s32 x = (screenW - WIDTH_) / 2;
    s32 y = (screenH - HEIGHT_) / 2;

    Button button_gui;
    button_gui.addButton(core::rect<s32>(x + 15, y + 15, x + 15 + 160, y + 15 + 30), L"GUI");
    button_gui.setColor(video::SColor(105, 0, 0, 0));
    button_gui.setOnClick([this]() { onCategoryButtonClick(SettingCategory::GUI); });
    buttons.push_back(button_gui);

    Button button_render;
    button_render.addButton(core::rect<s32>(x + 15, y + 15 + 45, x + 15 + 160, y + 15 + 30 + 45),
    L"Render");
    button_render.setColor(video::SColor(115, 0, 0, 0));
    button_render.setOnClick([this]() { onCategoryButtonClick(SettingCategory::RENDER); });
    buttons.push_back(button_render);

    Button button_mouse;
    button_mouse.addButton(core::rect<s32>(x + 15, y + 15 + 90, x + 15 + 160, y + 15 + 30 + 90),
    L"Movement");
    button_mouse.setColor(video::SColor(115, 0, 0, 0));
    button_mouse.setOnClick([this]() { onCategoryButtonClick(SettingCategory::MISC); });
    buttons.push_back(button_mouse);

    Button button_scrollbars;
    button_scrollbars.addButton(core::rect<s32>(x + 15, y + 15 + 135, x + 15 + 160, y + 15 + 30 + 135),
    L"Scrollbars");
    button_scrollbars.setColor(video::SColor(115, 0, 0, 0));
    button_scrollbars.setOnClick([this]() { onCategoryButtonClick(SettingCategory::Scrollbars); });
    buttons.push_back(button_scrollbars);

    for (size_t i = 0; i < buttons.size(); i++) {
        buttons[i].setVisible(false);
    }
    ItemsInit(SettingCategory::GUI);
    updateCategoryButtonActiveStates();
}

void Menu::create()
{
    core::rect<s32> screenRect(0, 0,
        env->getVideoDriver()->getScreenSize().Width,
        env->getVideoDriver()->getScreenSize().Height);
    setRelativePosition(screenRect);
    IGUIElement::setVisible(true);
    Environment->setFocus(this);
    m_menumgr->createdMenu(this);
    isOpen = true;
    // Reopening the menu (via the keybind) should always show the normal
    // settings panel, never leave the user stuck in HUD-drag mode from a
    // previous session.
    editMode = false;
    for (size_t i = 0; i < buttons.size(); i++) {
        buttons[i].setVisible(true);
    }
    if (this-> current_category == SettingCategory::Scrollbars) {
        scrollbar->setVisible(true);
        fps_scrollbar->setVisible(true);
        hitparticle_scrollbar->setVisible(true);
        target_particle_scrollbar->setVisible(true);
        hud_size_scrollbar->setVisible(true);
    }
    photo_panel.close();
    closeHandViewSettings();
    closeColorsPanel();
}

void Menu::close()
{
    Environment->removeFocus(this);
    m_menumgr->deletingMenu(this);
    // NOTE: deliberately not calling IGUIElement::setVisible(false) here,
    // so that draw()/OnEvent() keep being called for this element even
    // while the panel is closed. That's what lets isOpen/editMode (below)
    // fully control what's actually shown/interactive.
    //
    // We DO, however, shrink this element's own rect back to zero size.
    // Menu is a direct child of the true GUI root (a sibling of `guiroot`,
    // the element formspecs/inventory/HUD actually live under), added to
    // the root's child list after `guiroot`. Irrlicht's
    // IGUIElement::getElementFromPoint hit-tests children back-to-front
    // and returns on the first match, so as long as Menu's rect stays
    // full-screen (which it does from the moment create() first runs) it
    // keeps winning that hit-test and swallowing every click meant for
    // anything under `guiroot` - including the inventory formspec -
    // regardless of isOpen/editMode. Shrinking the rect to empty here
    // removes Menu from hit-testing while closed, so clicks correctly
    // fall through to the formspec/HUD/chat again. create() restores the
    // full-screen rect the next time the menu is actually opened.
    setRelativePosition(core::rect<s32>(0, 0, 0, 0));
    isOpen = false;
    editMode = false;
    for (size_t i = 0; i < buttons.size(); i++) {
        buttons[i].setVisible(false);
    }
    scrollbar->setVisible(false);
    fps_scrollbar->setVisible(false);
    hitparticle_scrollbar->setVisible(false);
    target_particle_scrollbar->setVisible(false);
    hud_size_scrollbar->setVisible(false);
    photo_panel.close();
    closeHandViewSettings();
    closeColorsPanel();
}

// getPhotoSettingsPanelRect() removed -- see PhotoHudSettingsMenu::getPanelRect()
// (src/gui/custom_menu/PhotoHudSettingsMenu.cpp), which Menu::
// getHandViewSettingsPanelRect() below now calls directly so the two
// panels keep sharing the exact same footprint.

// Middle-click on a settings tile calls this. Cycles: first click fills
// slot 1, second fills slot 2, a third click (both already full) clears
// both and starts fresh on slot 1 again -- so repeatedly middle-clicking
// is always a safe way to get back to a clean state without any extra
// UI for "clear bind".
void Menu::startBindCapture(const std::string &setting_name)
{
    std::string s1, s2;
    getBindTokens(setting_name, s1, s2);

    if (!s1.empty() && !s2.empty()) {
        setBindTokens(setting_name, "", "");
        s1.clear();
        s2.clear();
    }

    bind_capture_setting = setting_name;
    bind_capture_slot = s1.empty() ? 1 : 2;
}

// openPhotoSettings()/closePhotoSettings()/applyPhotoCustomPath() removed
// -- replaced by PhotoHudSettingsMenu::open()/close()/applyCustomPath() (see
// src/gui/custom_menu/PhotoHudSettingsMenu.cpp), called via the `photo_panel`
// member below.

core::rect<s32> Menu::getHandViewSettingsPanelRect()
{
    // Same fixed 600x480 centered panel every advanced-settings tile
    // uses (see Items::drawSetting()) -- keeps the generic background
    // it draws lined up with whatever buttons/sliders we place on it.
    // Deliberately shares PhotoHudSettingsMenu's own rect (rather than each
    // panel computing its own identical copy) so the two stay in sync
    // automatically if that size is ever tuned again.
    return photo_panel.getPanelRect();
}

void Menu::openHandViewSettings()
{
    // Mutually exclusive with the PhotoHUD panel -- see the matching
    // photo_panel.close() call at the PhotoHUD tile's right-click handler
    // below, and the note on this in Button::isPressed() (src/gui/
    // custom_menu/Button.cpp): both panels share the exact same screen
    // rect (getHandViewSettingsPanelRect() returns photo_panel.
    // getPanelRect()), so having both "open" at once would mean two full
    // sets of buttons/sliders sitting on top of each other, both
    // receiving clicks in that shared area.
    photo_panel.close();

    handview_settings_open = true;

    core::rect<s32> panel = getHandViewSettingsPanelRect();
    const s32 btn_w = 130, btn_h = 30, gap = 10;
    s32 row1_y = panel.UpperLeftCorner.Y + 50;
    s32 row2_y = row1_y + btn_h + gap;
    s32 row3_y = row2_y + btn_h + gap;

    Button *row1[] = {&handview_pick_vanilla_button, &handview_pick_static_button,
        &handview_pick_fast_button, &handview_pick_sway_button};
    Button *row2[] = {&handview_pick_chime_button, &handview_pick_old_button,
        &handview_pick_punch_button, &handview_pick_tilt_button};
    // Own row, separate from the animation-style pickers above: these two
    // are on/off modifiers (which side the hand renders on / whether the
    // idle walking wobble plays), not styles to pick between.
    Button *row3[] = {&handview_left_hand_button, &handview_no_view_bob_button};
    const wchar_t *row1_labels[] = {L"Vanilla", L"Static", L"Fast", L"Sway"};
    const wchar_t *row2_labels[] = {L"Chime", L"Old", L"Punch", L"Tilt"};
    const wchar_t *row3_labels[] = {L"Left Hand", L"NoViewBob"};

    for (int i = 0; i < 4; i++) {
        s32 x = panel.UpperLeftCorner.X + 20 + i * (btn_w + gap);
        row1[i]->addButton(core::rect<s32>(x, row1_y, x + btn_w, row1_y + btn_h), row1_labels[i]);
        row1[i]->setVisible(true);
    }
    for (int i = 0; i < 4; i++) {
        s32 x = panel.UpperLeftCorner.X + 20 + i * (btn_w + gap);
        row2[i]->addButton(core::rect<s32>(x, row2_y, x + btn_w, row2_y + btn_h), row2_labels[i]);
        row2[i]->setVisible(true);
    }
    for (int i = 0; i < 2; i++) {
        s32 x = panel.UpperLeftCorner.X + 20 + i * (btn_w + gap);
        row3[i]->addButton(core::rect<s32>(x, row3_y, x + btn_w, row3_y + btn_h), row3_labels[i]);
        row3[i]->setVisible(true);
    }

    s32 slider_x = panel.UpperLeftCorner.X + 20;
    s32 slider_w = 560;
    s32 slider_y = row3_y + btn_h + 25;
    handview_offset_x_scrollbar->setRelativePosition(
        core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    handview_offset_x_scrollbar->setVisible(true);

    slider_y += 40;
    handview_offset_y_scrollbar->setRelativePosition(
        core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    handview_offset_y_scrollbar->setVisible(true);

    slider_y += 40;
    handview_offset_z_scrollbar->setRelativePosition(
        core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    handview_offset_z_scrollbar->setVisible(true);

    slider_y += 40;
    handview_scale_scrollbar->setRelativePosition(
        core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    handview_scale_scrollbar->setVisible(true);

    core::rect<s32> close_rect(panel.LowerRightCorner.X - 90, panel.UpperLeftCorner.Y + 10,
        panel.LowerRightCorner.X - 10, panel.UpperLeftCorner.Y + 40);
    handview_settings_close_button.addButton(close_rect, L"Close");
    handview_settings_close_button.setVisible(true);
}

void Menu::closeHandViewSettings()
{
    handview_settings_close_button.setVisible(false);
    handview_pick_vanilla_button.setVisible(false);
    handview_pick_static_button.setVisible(false);
    handview_pick_fast_button.setVisible(false);
    handview_pick_sway_button.setVisible(false);
    handview_pick_chime_button.setVisible(false);
    handview_pick_old_button.setVisible(false);
    handview_pick_punch_button.setVisible(false);
    handview_pick_tilt_button.setVisible(false);
    handview_left_hand_button.setVisible(false);
    handview_no_view_bob_button.setVisible(false);
    handview_offset_x_scrollbar->setVisible(false);
    handview_offset_y_scrollbar->setVisible(false);
    handview_offset_z_scrollbar->setVisible(false);
    handview_scale_scrollbar->setVisible(false);
    handview_settings_open = false;
    Environment->setFocus(this);
}

std::vector<Menu::ColorTarget> Menu::getColorTargets()
{
    return {
        {L"MineBoost GUI", "mineboost_gui_color",  false, ""},
        {L"Coords",       "hud_color_coords",     false, ""},
        {L"FPS",          "hud_color_fps",         false, ""},
        {L"Ping",         "hud_color_ping",        false, ""},
        {L"NowPlaying",   "hud_color_music",       false, ""},
        {L"ShowRP",       "hud_color_rp",          false, ""},
        {L"ConsumptionHUD", "hud_color_consumption", false, ""},
        {L"InventoryHUD", "hud_color_inventory",   false, ""},
        {L"CraftHUD",     "hud_color_craft",       false, ""},
        {L"TargetHUD",    "hud_color_target",      false, ""},
        {L"PhotoHUD",     "hud_color_photo",       false, ""},
        {L"KeyStroker Outline", "hud_color_keystroker_border", false, ""},
        {L"CPS Outline",  "hud_color_cps_border",  false, ""},
        {L"Preview Outline", "hud_preview_border_color", false, ""},
    };
}

core::rect<s32> Menu::getColorsPanelRect()
{
    // Wider/taller than the shared 600x400 photo/handview panel size --
    // this one has its own background (drawn directly in draw() below)
    // rather than piggybacking on Items::drawSetting(), so it isn't tied
    // to that size.
    s32 sw = Environment->getVideoDriver()->getScreenSize().Width;
    s32 sh = Environment->getVideoDriver()->getScreenSize().Height;
    // Height bumped from 480 to fit the "MineBoost GUI" entry added to
    // getColorTargets() (14 rows now instead of 13, each 28px + 4px gap
    // starting 50px down from the top) without the target-button list
    // spilling past the panel's bottom edge.
    s32 rectWidth = 700, rectHeight = 520;
    s32 posX = (sw - rectWidth) / 2;
    s32 posY = (sh - rectHeight) / 2;
    return core::rect<s32>(posX, posY, posX + rectWidth, posY + rectHeight);
}

void Menu::selectColorsTarget(size_t index)
{
    std::vector<ColorTarget> targets = getColorTargets();
    if (index >= targets.size())
        return;
    colors_selected_index = index;

    v3f c = g_settings->getV3F(targets[index].setting).value_or(v3f(255, 255, 255));
    colors_r_scrollbar->setPos(rangelim((s32)myround(c.X), 0, 255));
    colors_g_scrollbar->setPos(rangelim((s32)myround(c.Y), 0, 255));
    colors_b_scrollbar->setPos(rangelim((s32)myround(c.Z), 0, 255));

    if (targets[index].has_alpha) {
        colors_a_scrollbar->setPos(rangelim(g_settings->getS32(targets[index].alpha_setting), 0, 255));
        colors_a_scrollbar->setVisible(true);
    } else {
        colors_a_scrollbar->setVisible(false);
    }
}

void Menu::openColorsPanel()
{
    colors_panel_open = true;
    Environment->setFocus(this);

    // The Colors panel is a full modal over whatever category tab was
    // active underneath -- most tabs are just plain Items tiles that
    // naturally don't draw once covered, but "Scrollbars" is drawn via
    // real Irrlicht widgets (fps_scrollbar/hitparticle_scrollbar/etc.)
    // plus its own label text in draw(), both gated only on
    // "current_category == Scrollbars". Without explicitly hiding them
    // here too, opening Colors while on the Scrollbars tab left its
    // sliders and "FOV: .../FPS: .../HUD Size: ..." labels rendering
    // right on top of the Colors panel. closeColorsPanel() restores them
    // if the Scrollbars tab is still the active one.
    scrollbar->setVisible(false);
    fps_scrollbar->setVisible(false);
    hitparticle_scrollbar->setVisible(false);
    target_particle_scrollbar->setVisible(false);
    hud_size_scrollbar->setVisible(false);

    core::rect<s32> panel = getColorsPanelRect();
    std::vector<ColorTarget> targets = getColorTargets();

    const s32 list_x = panel.UpperLeftCorner.X + 15;
    const s32 list_w = 160, btn_h = 28, gap = 4;
    s32 list_y = panel.UpperLeftCorner.Y + 50;
    for (size_t i = 0; i < colors_target_buttons.size(); i++) {
        core::rect<s32> r(list_x, list_y, list_x + list_w, list_y + btn_h);
        colors_target_buttons[i].addButton(r, targets[i].label);
        colors_target_buttons[i].setVisible(true);
        list_y += btn_h + gap;
    }

    const s32 slider_x = list_x + list_w + 40;
    const s32 slider_w = panel.LowerRightCorner.X - slider_x - 20;
    s32 slider_y = panel.UpperLeftCorner.Y + 90;
    colors_r_scrollbar->setRelativePosition(core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    colors_r_scrollbar->setVisible(true);

    slider_y += 50;
    colors_g_scrollbar->setRelativePosition(core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    colors_g_scrollbar->setVisible(true);

    slider_y += 50;
    colors_b_scrollbar->setRelativePosition(core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    colors_b_scrollbar->setVisible(true);

    slider_y += 50;
    colors_a_scrollbar->setRelativePosition(core::rect<s32>(slider_x, slider_y, slider_x + slider_w, slider_y + 20));
    // Visibility (alpha only applies to the chat background target) is
    // finalized by selectColorsTarget() right below.

    core::rect<s32> close_rect(panel.LowerRightCorner.X - 90, panel.UpperLeftCorner.Y + 10,
        panel.LowerRightCorner.X - 10, panel.UpperLeftCorner.Y + 40);
    colors_settings_close_button.addButton(close_rect, L"Close");
    colors_settings_close_button.setVisible(true);

    selectColorsTarget(colors_selected_index);
}

void Menu::closeColorsPanel()
{
    colors_panel_open = false;
    colors_settings_close_button.setVisible(false);
    for (Button &b : colors_target_buttons)
        b.setVisible(false);
    colors_r_scrollbar->setVisible(false);
    colors_g_scrollbar->setVisible(false);
    colors_b_scrollbar->setVisible(false);
    colors_a_scrollbar->setVisible(false);

    // Undo the hide in openColorsPanel() -- only if Scrollbars is still
    // the active tab underneath, so we don't make them pop up over some
    // other category the player switched to while Colors was open.
    if (current_category == SettingCategory::Scrollbars) {
        scrollbar->setVisible(true);
        fps_scrollbar->setVisible(true);
        hitparticle_scrollbar->setVisible(true);
        target_particle_scrollbar->setVisible(true);
        hud_size_scrollbar->setVisible(true);
    }

    Environment->setFocus(this);
}

Menu *Menu::s_instance = nullptr;

void Menu::checkGlobalBinds(const irr::SEvent &event)
{
    if (!s_instance)
        return;

    // While actively waiting for a bind assignment (middle-clicked a
    // tile, see startBindCapture()), the very next key/mouse press is
    // being captured as the new bind token in OnEvent() instead -- skip
    // the toggle here so that assigning "F" as a new bind doesn't also
    // immediately toggle whatever "F" already happened to be bound to.
    if (!s_instance->bind_capture_setting.empty())
        return;

    // Skipped while a text field (chat, formspec input, console) has
    // focus, so typing doesn't accidentally trigger binds.
    gui::IGUIElement *focused = s_instance->Environment->getFocus();
    bool typing = focused && focused->getType() == gui::EGUIET_EDIT_BOX;
    if (typing)
        return;

    std::string token;
    bool cancel = false;
    if (!eventToBindToken(event, token, cancel) || cancel || token.empty())
        return;

    for (const Setting &s : s_instance->getSettings()) {
        std::string s1, s2;
        getBindTokens(s.value, s1, s2);
        if ((s1 == token || s2 == token) && g_settings->exists(s.value)) {
            bool cur = g_settings->getBool(s.value);
            g_settings->setBool(s.value, !cur);
        }
    }
}

void Menu::checkMainMenuOpenKeybind(const irr::SEvent &event)
{
    if (!s_instance || s_instance->hasClient())
        return;

    if (event.EventType != irr::EET_KEY_INPUT_EVENT || !event.KeyInput.PressedDown)
        return;

    // Same setting Game::processKeyInput() reads via
    // KeyCache::populate() (src/client/inputhandler.cpp) for the in-game
    // "open settings menu" key -- reused here rather than a separate
    // setting so there's exactly one keybind to configure/remember for
    // "open the settings menu", whether that's from the title screen or
    // in-game.
    if (!(KeyPress(event.KeyInput) == getKeySetting("keymap_menu")))
        return;

    s_instance->toggleOpenFromKeybind();
}

bool Menu::OnEvent(const irr::SEvent& event)
{
    s32 screenWidth = Environment->getVideoDriver()->getScreenSize().Width, screenHeight = Environment->getVideoDriver()->getScreenSize().Height;

    if (event.EventType == irr::EET_MOUSE_INPUT_EVENT)
        last_mouse_pos = core::position2d<s32>(event.MouseInput.X, event.MouseInput.Y);

    // Actively waiting for a bind assignment (middle-clicked a tile) --
    // this takes priority over absolutely everything else: the very next
    // key press or bindable mouse action is captured and consumed here,
    // never reaching normal menu handling below. Escape/LMB/RMB cancel
    // the capture instead of being assigned.
    if (!bind_capture_setting.empty()) {
        if (event.EventType == irr::EET_KEY_INPUT_EVENT || event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
            std::string token;
            bool cancel = false;
            if (eventToBindToken(event, token, cancel)) {
                if (!cancel) {
                    std::string s1, s2;
                    getBindTokens(bind_capture_setting, s1, s2);
                    if (bind_capture_slot == 1)
                        s1 = token;
                    else
                        s2 = token;
                    if (!s1.empty() && s1 == s2) {
                        // Same token in both slots is pointless -- drop
                        // whichever one we didn't just set.
                        if (bind_capture_slot == 1) s2.clear(); else s1.clear();
                    }
                    setBindTokens(bind_capture_setting, s1, s2);
                }
                bind_capture_setting.clear();
            }
            // Swallow every key/mouse event while capturing, matched or
            // not (e.g. a mouse-move or key-up in between), so nothing
            // else in the menu reacts to it mid-capture.
            return true;
        }
    }

    // Global keybind matching used to live inline here, but now runs from
    // checkGlobalBinds() above, called directly from
    // MyEventReceiver::OnEvent() in src/client/inputhandler.cpp for every
    // raw input event -- see the comment on checkGlobalBinds() in Menu.h
    // for why. Calling it a second time from here too, on top of that,
    // would double-toggle it back to whatever it started as: exactly the
    // "only ever seems to turn off" bug this was written to fix, since
    // that's exactly the state this element used to still reliably get
    // events in (open + focused), while the closed-menu case silently
    // stopped receiving them and never got the offsetting second call.

    if (event.EventType == irr::EET_KEY_INPUT_EVENT) {
        if (event.KeyInput.Key == KEY_MENU || event.KeyInput.Key == KEY_LMENU || event.KeyInput.Key == KEY_RMENU) {
            altPressed = event.KeyInput.PressedDown;
        }
    }

    if (event.EventType == irr::EET_KEY_INPUT_EVENT && isOpen) {
        if (event.KeyInput.Key == KEY_ESCAPE && event.KeyInput.PressedDown) {
            close();
            return true;
        }
    }
    if (photo_panel.isOpen()) {
        // The panel handles its own buttons/edit box/click-outside-to-
        // close entirely internally now -- see PhotoHudSettingsMenu::onEvent()
        // (src/gui/custom_menu/PhotoHudSettingsMenu.cpp). Same convention as
        // before: every event is considered consumed while the panel is
        // open, so nothing leaks through to whatever's behind it.
        photo_panel.onEvent(event);
        return true;
    }

    if (handview_settings_open) {
        if (handview_settings_close_button.isPressed(event))
            return true;
        if (handview_pick_vanilla_button.isPressed(event))
            return true;
        if (handview_pick_static_button.isPressed(event))
            return true;
        if (handview_pick_fast_button.isPressed(event))
            return true;
        if (handview_pick_sway_button.isPressed(event))
            return true;
        if (handview_pick_chime_button.isPressed(event))
            return true;
        if (handview_pick_old_button.isPressed(event))
            return true;
        if (handview_pick_punch_button.isPressed(event))
            return true;
        if (handview_pick_tilt_button.isPressed(event))
            return true;
        if (handview_left_hand_button.isPressed(event))
            return true;
        if (handview_no_view_bob_button.isPressed(event))
            return true;

        // Menu keeps GUI focus (see Environment->setFocus(this) in
        // openHandViewSettings()/closeHandViewSettings()), so it gets
        // first look at every event -- and this block used to end with
        // an unconditional "return true" below, which swallowed clicks
        // and drags before they ever reached these scrollbars. They're
        // separate native GUI elements (children of the root, not of
        // Menu), so they only actually respond if we hand them the
        // event ourselves, the same way Buttons/Items get isPressed()
        // called manually elsewhere in this file.
        handview_offset_x_scrollbar->OnEvent(event);
        handview_offset_y_scrollbar->OnEvent(event);
        handview_offset_z_scrollbar->OnEvent(event);
        handview_scale_scrollbar->OnEvent(event);

        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
                event.MouseInput.Event == irr::EMIE_LMOUSE_PRESSED_DOWN &&
                !getHandViewSettingsPanelRect().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !handview_offset_x_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !handview_offset_y_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !handview_offset_z_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !handview_scale_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
            closeHandViewSettings();
        }

        return true;
    }

    if (colors_panel_open) {
        if (colors_settings_close_button.isPressed(event))
            return true;
        for (Button &b : colors_target_buttons) {
            if (b.isPressed(event))
                return true;
        }

        colors_r_scrollbar->OnEvent(event);
        colors_g_scrollbar->OnEvent(event);
        colors_b_scrollbar->OnEvent(event);
        if (colors_a_scrollbar->isVisible())
            colors_a_scrollbar->OnEvent(event);

        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
                event.MouseInput.Event == irr::EMIE_LMOUSE_PRESSED_DOWN &&
                !getColorsPanelRect().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !colors_r_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !colors_g_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !colors_b_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)) &&
                !colors_a_scrollbar->getAbsolutePosition().isPointInside(
                    core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
            closeColorsPanel();
        }

        return true;
    }

    // The "Move HUD" button lives inside the settings panel: it should
    // only be visible and clickable while the menu is open (isOpen==true),
    // which is also the only time this element's rect is guaranteed to be
    // sized to the full screen (see create()), so hit-testing works
    // correctly. It stays available in editMode too, so the user can
    // press it again to leave HUD-drag mode.
    if (isOpen && event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
        s32 sw = Environment->getVideoDriver()->getScreenSize().Width;
        s32 sh = Environment->getVideoDriver()->getScreenSize().Height;
        const s32 bw = 130, bh = 32, margin = 10;
        hud_move_button.addButton(
            core::rect<s32>(sw - bw - margin, sh - bh - margin, sw - margin, sh - margin),
            stringToWString("Move HUD"));
        if (hud_move_button.isPressed(event))
            return true;

        colors_open_button.addButton(
            core::rect<s32>(sw - bw - margin, sh - bh - margin - bh - margin, sw - margin, sh - margin - bh - margin),
            stringToWString("Colors"));
        // Disabled while dragging/resizing HUD elements (Move HUD mode) --
        // the Colors panel is a full modal that would cover the drag
        // targets and grid overlay, plus it hides the scrollbars that
        // Move HUD itself may be using (see openColorsPanel()'s comment
        // about the Scrollbars tab).
        if (!editMode && colors_open_button.isPressed(event))
            return true;
    }

    if (editMode) {
        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
            switch (event.MouseInput.Event) {
                case irr::EMIE_LMOUSE_PRESSED_DOWN:
                    if (coords_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        coords_sprite.isDragging = true;
                        offset = core::vector2d<s32>(event.MouseInput.X - coords_sprite.x, event.MouseInput.Y - coords_sprite.y);
                        return true;
                    }

                    if (fov_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        fov_sprite.isDragging = true;
                        offset_f = core::vector2d<s32>(event.MouseInput.X - fov_sprite.x, event.MouseInput.Y - fov_sprite.y);
                        return true;
                    }

                    if (ping_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        ping_sprite.isDragging = true;
                        offset_ping = core::vector2d<s32>(event.MouseInput.X - ping_sprite.x, event.MouseInput.Y - ping_sprite.y);
                        return true;
                    }

                    if (chat.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        chat.isDragging = true;
                        offset_chp = core::vector2d<s32>(event.MouseInput.X - chat.x, event.MouseInput.Y - chat.y);
                        return true;
                    }

                    {
                        f32 keys_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
                            rangelim(g_settings->getFloat("keys_size"), 0.5f, 2.5f);
                        if (getKeysVisualRect(keystr, keys_size).isPointInside(
                                core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                            keystr.isDragging = true;
                            offset_keys = core::vector2d<s32>(event.MouseInput.X - keystr.x, event.MouseInput.Y - keystr.y);
                            return true;
                        }
                    }

                    {
                        f32 cps_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
                            rangelim(g_settings->getFloat("cps_size"), 0.5f, 2.5f);
                        if (getCpsVisualRect(cps_sprite, cps_size).isPointInside(
                                core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                            cps_sprite.isDragging = true;
                            offset_cps = core::vector2d<s32>(event.MouseInput.X - cps_sprite.x, event.MouseInput.Y - cps_sprite.y);
                            return true;
                        }
                    }

                    if (music_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        music_sprite.isDragging = true;
                        offset_music = core::vector2d<s32>(event.MouseInput.X - music_sprite.x, event.MouseInput.Y - music_sprite.y);
                        return true;
                    }

                    if (rp_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        rp_sprite.isDragging = true;
                        offset_rp = core::vector2d<s32>(event.MouseInput.X - rp_sprite.x, event.MouseInput.Y - rp_sprite.y);
                        return true;
                    }

                    if (consumption_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        consumption_sprite.isDragging = true;
                        offset_consumption = core::vector2d<s32>(event.MouseInput.X - consumption_sprite.x, event.MouseInput.Y - consumption_sprite.y);
                        return true;
                    }

                    if (target_hud_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        target_hud_sprite.isDragging = true;
                        offset_target_hud = core::vector2d<s32>(event.MouseInput.X - target_hud_sprite.x, event.MouseInput.Y - target_hud_sprite.y);
                        return true;
                    }

                    if (inventory_hud_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        inventory_hud_sprite.isDragging = true;
                        offset_inventory_hud = core::vector2d<s32>(event.MouseInput.X - inventory_hud_sprite.x, event.MouseInput.Y - inventory_hud_sprite.y);
                        return true;
                    }

                    if (craft_hud_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        craft_hud_sprite.isDragging = true;
                        offset_craft_hud = core::vector2d<s32>(event.MouseInput.X - craft_hud_sprite.x, event.MouseInput.Y - craft_hud_sprite.y);
                        return true;
                    }

                    if (photo_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                        photo_sprite.isDragging = true;
                        offset_photo = core::vector2d<s32>(event.MouseInput.X - photo_sprite.x, event.MouseInput.Y - photo_sprite.y);
                        return true;
                    }

                    // ArmorHUD temporarily disabled -- drag-start block commented out.
                    // if (armor_hud_sprite.get_rect().isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                    //     armor_hud_sprite.isDragging = true;
                    //     offset_armor_hud = core::vector2d<s32>(event.MouseInput.X - armor_hud_sprite.x, event.MouseInput.Y - armor_hud_sprite.y);
                    //     return true;
                    // }
                    break;

                case irr::EMIE_LMOUSE_LEFT_UP:
                    coords_sprite.isDragging = false;
                    fov_sprite.isDragging = false;
                    ping_sprite.isDragging = false;
                    chat.isDragging = false;
                    keystr.isDragging = false;
                    cps_sprite.isDragging = false;
                    music_sprite.isDragging = false;
                    rp_sprite.isDragging = false;
                    consumption_sprite.isDragging = false;
                    target_hud_sprite.isDragging = false;
                    inventory_hud_sprite.isDragging = false;
                    craft_hud_sprite.isDragging = false;
                    photo_sprite.isDragging = false;
                    // armor_hud_sprite.isDragging = false; // ArmorHUD temporarily disabled
                    break;

                case irr::EMIE_MOUSE_MOVED: {
                    // Snap-to-grid while dragging (see the grid overlay
                    // drawn in draw() when editMode is active) -- makes it
                    // easy to line up multiple HUD elements, same idea as
                    // the grid in CloakV4's HUD editor. Hold Alt to
                    // temporarily disable snapping for fine positioning.
                    s32 grid = g_settings->getS32("hud_grid_size");
                    bool snap_on = g_settings->getBool("hud_grid_enabled") && !altPressed && grid > 1;
                    auto snap = [&](s32 v) -> s32 {
                        if (!snap_on) return v;
                        return (s32)std::round((float)v / grid) * grid;
                    };

                    if (coords_sprite.isDragging) {
                        coords_sprite.x = snap(event.MouseInput.X - offset.X);
                        coords_sprite.y = snap(event.MouseInput.Y - offset.Y);
                        coords_sprite.save(screenWidth, screenHeight, "coords_sprite");
                    }

                    if (fov_sprite.isDragging) {
                        fov_sprite.x = snap(event.MouseInput.X - offset_f.X);
                        fov_sprite.y = snap(event.MouseInput.Y - offset_f.Y);
                        fov_sprite.save(screenWidth, screenHeight, "fov_coords");
                    }

                    if (ping_sprite.isDragging) {
                        ping_sprite.x = snap(event.MouseInput.X - offset_ping.X);
                        ping_sprite.y = snap(event.MouseInput.Y - offset_ping.Y);
                        ping_sprite.save(screenWidth, screenHeight, "ping_coords");
                    }

                    if (chat.isDragging) {
                        chat.x = snap(event.MouseInput.X - offset_chp.X);
                        chat.y = snap(event.MouseInput.Y - offset_chp.Y);
                        chat.save(screenWidth, screenHeight, "chat_x", "chat_y");
                    }

                    if (keystr.isDragging) {
                        keystr.x = snap(event.MouseInput.X - offset_keys.X);
                        keystr.y = snap(event.MouseInput.Y - offset_keys.Y);
                        keystr.save(screenWidth, screenHeight, "keys_x", "keys_y");
                    }

                    if (cps_sprite.isDragging) {
                        cps_sprite.x = snap(event.MouseInput.X - offset_cps.X);
                        cps_sprite.y = snap(event.MouseInput.Y - offset_cps.Y);
                        cps_sprite.save(screenWidth, screenHeight, "cps_x", "cps_y");
                    }

                    if (music_sprite.isDragging) {
                        music_sprite.x = snap(event.MouseInput.X - offset_music.X);
                        music_sprite.y = snap(event.MouseInput.Y - offset_music.Y);
                        music_sprite.save(screenWidth, screenHeight, "music_hud_x", "music_hud_y");
                    }

                    if (rp_sprite.isDragging) {
                        rp_sprite.x = snap(event.MouseInput.X - offset_rp.X);
                        rp_sprite.y = snap(event.MouseInput.Y - offset_rp.Y);
                        rp_sprite.save(screenWidth, screenHeight, "rp_hud_x", "rp_hud_y");
                    }

                    if (consumption_sprite.isDragging) {
                        consumption_sprite.x = snap(event.MouseInput.X - offset_consumption.X);
                        consumption_sprite.y = snap(event.MouseInput.Y - offset_consumption.Y);
                        consumption_sprite.save(screenWidth, screenHeight, "consumption_hud_x", "consumption_hud_y");
                    }

                    if (target_hud_sprite.isDragging) {
                        target_hud_sprite.x = snap(event.MouseInput.X - offset_target_hud.X);
                        target_hud_sprite.y = snap(event.MouseInput.Y - offset_target_hud.Y);
                        target_hud_sprite.save(screenWidth, screenHeight, "target_hud_x", "target_hud_y");
                    }

                    if (inventory_hud_sprite.isDragging) {
                        inventory_hud_sprite.x = snap(event.MouseInput.X - offset_inventory_hud.X);
                        inventory_hud_sprite.y = snap(event.MouseInput.Y - offset_inventory_hud.Y);
                        inventory_hud_sprite.save(screenWidth, screenHeight, "inventory_hud_x", "inventory_hud_y");
                    }

                    if (craft_hud_sprite.isDragging) {
                        craft_hud_sprite.x = snap(event.MouseInput.X - offset_craft_hud.X);
                        craft_hud_sprite.y = snap(event.MouseInput.Y - offset_craft_hud.Y);
                        craft_hud_sprite.save(screenWidth, screenHeight, "craft_hud_x", "craft_hud_y");
                    }

                    if (photo_sprite.isDragging) {
                        photo_sprite.x = snap(event.MouseInput.X - offset_photo.X);
                        photo_sprite.y = snap(event.MouseInput.Y - offset_photo.Y);
                        photo_sprite.save(screenWidth, screenHeight, "photo_hud_x", "photo_hud_y");
                    }

                    // ArmorHUD temporarily disabled -- drag-move block commented out.
                    // if (armor_hud_sprite.isDragging) {
                    //     armor_hud_sprite.x = event.MouseInput.X - offset_armor_hud.X;
                    //     armor_hud_sprite.y = event.MouseInput.Y - offset_armor_hud.Y;
                    //     armor_hud_sprite.save(screenWidth, screenHeight, "armor_hud_x", "armor_hud_y");
                    // }
                    break;
                }

                case irr::EMIE_MOUSE_WHEEL: {
                    // Resize whichever draggable HUD element the mouse is
                    // currently hovering, right there in "Move HUD" mode --
                    // no separate menu needed. Scroll up = bigger, down =
                    // smaller. Sizes persist via the same per-element
                    // "*_size" settings the real HUD drawing code reads
                    // (src/client/hud.cpp, src/client/gameui.cpp,
                    // builtin/client/keystroker.lua).
                    core::vector2d<s32> mp(event.MouseInput.X, event.MouseInput.Y);
                    float dir = event.MouseInput.Wheel > 0 ? 1.0f : -1.0f;

                    // Plain text/icon HUDs: a simple multiplier on a fixed
                    // base box size (the base box itself is just a hitbox;
                    // the real on-screen size follows the multiplied font/
                    // icon scale -- see the "*_size" reads in hud.cpp/
                    // gameui.cpp/keystroker.lua).
                    auto resize_multiplier = [&](Sprite_ &spr, const char *setting,
                            s32 base_w, s32 base_h) -> bool {
                        if (!spr.get_rect().isPointInside(mp))
                            return false;
                        float mult = g_settings->getFloat(setting) + dir * 0.1f;
                        mult = std::max(0.5f, std::min(2.5f, mult));
                        g_settings->setFloat(setting, mult);
                        spr.width = (s32)(base_w * mult);
                        spr.height = (s32)(base_h * mult);
                        return true;
                    };

                    if (resize_multiplier(coords_sprite, "coords_size", 140, 30))
                        return true;
                    if (resize_multiplier(fov_sprite, "fps_size", 140, 30))
                        return true;
                    if (resize_multiplier(ping_sprite, "ping_size", 140, 30))
                        return true;
                    if (resize_multiplier(keystr, "keys_size", 132, 150))
                        return true;
                    if (resize_multiplier(cps_sprite, "cps_size", 132, 36))
                        return true;
                    if (resize_multiplier(target_hud_sprite, "target_hud_size", 160, 50))
                        return true;

                    // Content-sized HUDs: the drag box is recomputed every
                    // frame from live content (see drawMusicHudPreview()/
                    // drawInventoryHudPreview()/drawCraftHudPreview() above
                    // and their real-HUD equivalents in hud.cpp), so only
                    // the multiplier setting needs updating here -- the box
                    // itself catches up on the next frame.
                    auto bump_setting = [&](Sprite_ &spr, const char *setting) -> bool {
                        if (!spr.get_rect().isPointInside(mp))
                            return false;
                        float mult = g_settings->getFloat(setting) + dir * 0.1f;
                        mult = std::max(0.5f, std::min(2.5f, mult));
                        g_settings->setFloat(setting, mult);
                        return true;
                    };

                    if (bump_setting(music_sprite, "music_hud_size"))
                        return true;
                    if (bump_setting(rp_sprite, "rp_hud_size"))
                        return true;
                    if (bump_setting(consumption_sprite, "consumption_hud_size"))
                        return true;
                    if (bump_setting(inventory_hud_sprite, "inventory_hud_size"))
                        return true;
                    if (bump_setting(craft_hud_sprite, "craft_hud_size"))
                        return true;

                    // Photo HUD: stores its size directly in pixels
                    // ("photo_hud_size" -- the max width/height of the
                    // drawn image), so scroll steps by a pixel amount
                    // instead of a multiplier.
                    if (photo_sprite.get_rect().isPointInside(mp)) {
                        s32 sz = g_settings->getS32("photo_hud_size") + (s32)(dir * 20);
                        sz = std::max<s32>(40, std::min<s32>(600, sz));
                        g_settings->setS32("photo_hud_size", sz);
                        return true;
                    }

                    // Scrolling over empty space (not over any HUD
                    // element above) instead resizes the snap grid itself.
                    {
                        s32 grid = g_settings->getS32("hud_grid_size") + (s32)(dir * 5);
                        grid = std::max<s32>(5, std::min<s32>(100, grid));
                        g_settings->setS32("hud_grid_size", grid);
                    }
                    break;
                }

                default:
                    break;
            }
        }
        return Parent ? Parent->OnEvent(event) : false;
    }

    if (isOpen && !editMode) {
        for (size_t i = 0; i < buttons.size(); i++) {
            buttons[i].isPressed(event);
        }

        // Shift+RMB clears whatever bind is on the tile currently under the
        // cursor. Previously this was the "`" key (KEY_OEM_3, right above
        // Tab) -- moved to Shift+RMB so it lives on the same button players
        // already reach for to interact with a tile, instead of a separate
        // key across the keyboard (and so it works the same way on
        // touch/Android, where there's no "`" key at all). Checked *before*
        // the plain-RMB handler below (which opens PhotoHUD/HandView's
        // advanced settings), and that handler explicitly requires Shift to
        // be *not* held, so Shift+RMB can never fall through and also
        // toggle those settings panels open/closed. Uses last_mouse_pos
        // (tracked from real mouse events at the top of OnEvent(), the same
        // coordinate space item rects live in) rather than querying the raw
        // device cursor control directly -- that can disagree with
        // GUI-space coordinates under DPI/hud scaling, which silently made
        // every hit-test here miss regardless of where the cursor actually
        // was.
        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
                event.MouseInput.Event == irr::EMIE_RMOUSE_PRESSED_DOWN &&
                event.MouseInput.Shift) {
            for (size_t i = 0; i < items.size(); i++) {
                const std::string &setting_item = items[i].get_setting_item();
                if (!items[i].get_rect().isPointInside(last_mouse_pos))
                    continue;

                std::string s1, s2;
                getBindTokens(setting_item, s1, s2);
                if (!s1.empty() || !s2.empty())
                    setBindTokens(setting_item, "", "");
                return true;
            }
        }

        // Right-click (without Shift) PhotoHUD/HandView to open their
        // advanced settings. Every other tile ignores plain RMB now
        // (bind-clearing moved to Shift+RMB above) -- the explicit
        // "!event.MouseInput.Shift" check is what keeps Shift+RMB on these
        // two tiles from also toggling their settings panels open/closed.
        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
                event.MouseInput.Event == irr::EMIE_RMOUSE_PRESSED_DOWN &&
                !event.MouseInput.Shift) {
            for (size_t i = 0; i < items.size(); i++) {
                if (items[i].get_setting_item() == "photo_hud" &&
                        items[i].get_rect().isPointInside(
                            core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                    if (photo_panel.isOpen())
                        photo_panel.close();
                    else {
                        // Mutually exclusive with HandView -- see the
                        // comment on this in Menu::openHandViewSettings().
                        closeHandViewSettings();
                        photo_panel.open();
                    }
                    return true;
                }
                if (items[i].get_setting_item() == "handview_enabled" &&
                        items[i].get_rect().isPointInside(
                            core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                    if (handview_settings_open)
                        closeHandViewSettings();
                    else
                        openHandViewSettings();
                    return true;
                }
            }
        }

        // Middle-click any tile to bind it to a key/mouse action (see
        // startBindCapture()/eventToBindToken() above and the
        // bind_capture_setting handling at the top of OnEvent()).
        if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
                event.MouseInput.Event == irr::EMIE_MMOUSE_PRESSED_DOWN) {
            for (size_t i = 0; i < items.size(); i++) {
                if (items[i].get_rect().isPointInside(
                        core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                    startBindCapture(items[i].get_setting_item());
                    return true;
                }
            }
        }

        for (size_t i = 0; i < items.size(); i++) {
            items[i].isPressed(event);
        }
    }

    return Parent ? Parent->OnEvent(event) : false;
}

void Menu::draw()
{
    updateScrollBarPosition(scrollbar, screenW, screenH);
    updateFpsScrollBarPosition(fps_scrollbar, screenW, screenH);
    updateHitParticleScrollBarPosition(hitparticle_scrollbar, screenW, screenH);
    updateTargetParticleScrollBarPosition(target_particle_scrollbar, screenW, screenH);
    updateHudSizeScrollBarPosition(hud_size_scrollbar, screenW, screenH);

    if (isOpen) {
        hud_move_button.draw(driver);
        if (!editMode)
            colors_open_button.draw(driver);
    }

    if (editMode) {
        bool grid_on = g_settings->getBool("hud_grid_enabled");
        s32 grid_size = g_settings->getS32("hud_grid_size");
        if (grid_on)
            drawSnapGrid(driver, screenW, screenH, grid_size);

        if (font) {
            std::wstring wgrid = stringToWString("Grid: " + std::string(grid_on ? "on" : "off") +
                " (" + std::to_string(grid_size) + "px) -- scroll empty space to resize, hold Alt to bypass");
            font->draw(wgrid.c_str(), core::rect<s32>(10, 10, screenW - 10, 30),
                video::SColor(200, 255, 255, 0));
        }

        // Coords preview: show the player's real current position, so
        // this looks exactly like the real HUD text once placed.
        v3f ppos;
        if (m_client && m_client->getEnv().getLocalPlayer())
            ppos = m_client->getEnv().getLocalPlayer()->getPosition() / BS;
        wchar_t coords_buf[96];
        swprintf(coords_buf, sizeof(coords_buf) / sizeof(wchar_t),
            L"(X: %.1f, Y: %.1f, Z: %.1f)", ppos.X, ppos.Y, ppos.Z);
        // Keep the drag hit-box in sync with what's actually drawn -- same
        // pattern as PhotoHUD/Inventory/Craft above. Mirrors the real HUD's
        // dynamic width in Hud::drawDebugTextBackgrounds() (src/client/hud.cpp)
        // exactly: same 140px baseline, same font-size formula, same
        // padding, so the preview box never disagrees with the real one
        // once coordinates get long (e.g. far-out negative positions).
        {
            float combined_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
                rangelim(g_settings->getFloat("coords_size"), 0.5f, 2.5f);
            coords_sprite.width = (s32)(140 * combined_size);
            if (font) {
                s32 text_w = (s32)font->getDimension(coords_buf).Width;
                coords_sprite.width = std::max(coords_sprite.width,
                    text_w + (s32)(24 * combined_size));
            }
        }
        drawHudPreviewBox(driver, font, core::rect<s32>(coords_sprite.get_rect()), coords_buf);

        drawHudPreviewBox(driver, font, core::rect<s32>(fov_sprite.get_rect()), L"[FPS: 60]");
        drawHudPreviewBox(driver, font, core::rect<s32>(ping_sprite.get_rect()), L"[Ping: 42 ms]");

        drawHudPreviewBox(driver, font, core::rect<s32>(chat.get_rect()),
            L"<Player1> hey", L"<Player2> preview text");

        // Keep these two in sync with keys_panel_bg.png/cps_panel_bg.png's
        // real rendered size every frame (not just at create()) -- see
        // the derivation comment where keystr.width/height are first set,
        // above in create(). "hud_size"/"keys_size"/"cps_size" can all
        // change live via their scrollbars while this menu is open, and
        // the real HUD resizes live too, so the preview box needs to
        // track it or it visibly drifts out of alignment with the real
        // one.
        f32 keys_hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
            rangelim(g_settings->getFloat("keys_size"), 0.5f, 2.5f);
        keystr.width = (s32)(160 * keys_hud_size);
        keystr.height = (s32)(160 * keys_hud_size);

        f32 cps_hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f) *
            rangelim(g_settings->getFloat("cps_size"), 0.5f, 2.5f);
        cps_sprite.width = (s32)(180 * cps_hud_size);
        cps_sprite.height = (s32)(54 * cps_hud_size);

        drawHudPreviewBox(driver, font, getKeysVisualRect(keystr, keys_hud_size),
            L"KeyStroker", L"W A S D");

        drawHudPreviewBox(driver, font, getCpsVisualRect(cps_sprite, cps_hud_size),
            L"LMB CPS: 12", L"RMB CPS: 8");

        drawMusicHudPreview(driver, font, music_sprite);

        drawShowRpPreview(driver, font, rp_sprite);
        drawConsumptionHudPreview(driver, font, consumption_sprite);

        drawHudPreviewBox(driver, font, core::rect<s32>(target_hud_sprite.get_rect()),
            L"PlayerName", L"HP: 20/20");

        drawInventoryHudPreview(driver, font, inventory_hud_sprite);
        drawCraftHudPreview(driver, font, craft_hud_sprite);
        drawPhotoHudPreview(driver, font, photo_sprite,
            m_client ? m_client->getTextureSource() : nullptr, screenW, screenH);

        // drawArmorHudPreview(driver, font, core::rect<s32>(armor_hud_sprite.get_rect())); // ArmorHUD temporarily disabled
    }

    if (isOpen && !editMode) {
        drawBackground(driver, screenW, screenH);

        for (size_t i = 0; i < buttons.size(); i++) {
            buttons[i].draw(driver);
        }

        // How binds work, explained once here instead of repeated on
        // every tile (see getBindDisplayString() above for why: the old
        // per-tile "(MMB to set)"/"(RMB to clear)" suffixes were routinely
        // wider than a tile and spilled into the neighboring one).
        if (font) {
            s32 px = (screenW - WIDTH_) / 2;
            s32 py = (screenH - HEIGHT_) / 2;
            std::wstring legend = L"MMB: set bind    Shift+RMB: clear bind";
            core::dimension2du lsz = font->getDimension(legend.c_str());
            s32 lx = px + WIDTH_ - (s32)lsz.Width - 16;
            s32 ly = py + 10;
            font->draw(legend.c_str(), core::rect<s32>(lx, ly, lx + (s32)lsz.Width, ly + (s32)lsz.Height),
                video::SColor(200, 170, 170, 170));
        }

        for (size_t i = 0; i < items.size(); i++) {
            items[i].draw(driver, screenW, screenH);

            // Bind label, drawn as an overlay from here rather than
            // inside Items::draw() so the bind-token helpers (which know
            // about the "bind_<setting>" settings) stay local to
            // Menu.cpp instead of being duplicated into Items.cpp too.
            if (font) {
                core::rect<s32> r = items[i].get_rect();
                std::wstring wbind = getBindDisplayString(items[i].get_setting_item());
                core::dimension2du sz = font->getDimension(wbind.c_str());

                // Center on the tile, but the label text (especially
                // once "(RMB to clear)" is appended) is often wider
                // than the tile itself, which used to push it past the
                // edge of the whole menu panel for tiles near the left
                // or right border. Clamp it to stay inside the panel.
                s32 panelLeft = (screenW - WIDTH_) / 2;
                s32 panelRight = panelLeft + WIDTH_;
                const s32 pad = 6;

                s32 bx = r.UpperLeftCorner.X + (r.getWidth() - (s32)sz.Width) / 2;
                bx = std::max(bx, panelLeft + pad);
                bx = std::min(bx, panelRight - pad - (s32)sz.Width);

                s32 by = r.LowerRightCorner.Y - (s32)sz.Height * 2 - 6;
                font->draw(wbind.c_str(), core::rect<s32>(bx, by, bx + (s32)sz.Width, by + (s32)sz.Height),
                    video::SColor(220, 120, 220, 255));
            }
        }

        if (!bind_capture_setting.empty() && font) {
            std::wstring wcap = L"Binding \"" + stringToWString(bind_capture_setting) +
                L"\" -- press a key, mouse wheel, side button, or MMB " +
                L"(Esc to cancel)";
            core::dimension2du sz = font->getDimension(wcap.c_str());
            s32 cx = (screenW - (s32)sz.Width) / 2;
            s32 cy = screenH - (s32)sz.Height - 40;
            core::rect<s32> bg(cx - 10, cy - 6, cx + (s32)sz.Width + 10, cy + (s32)sz.Height + 6);
            ModernUI::panel(driver, bg, ModernUI::Radius, video::SColor(220, 20, 20, 20), getMineBoostGuiColor());
            font->draw(wcap.c_str(), core::rect<s32>(cx, cy, cx + (s32)sz.Width, cy + (s32)sz.Height),
                video::SColor(255, 255, 255, 0));
        }
        if (photo_panel.isOpen()) {
            // Fully self-contained now -- see PhotoHudSettingsMenu::draw()
            // (src/gui/custom_menu/PhotoHudSettingsMenu.cpp) for the panel's own
            // background/labels/preview/buttons.
            photo_panel.draw(driver, font);
        }

        if (handview_settings_open) {
            core::rect<s32> panel = getHandViewSettingsPanelRect();
            ModernUI::panel(driver, panel, ModernUI::Radius, video::SColor(230, 20, 20, 20), getMineBoostGuiColor());

            std::string style = g_settings->get("hand_anim_style");
            Button *style_buttons[] = {&handview_pick_vanilla_button, &handview_pick_static_button,
                &handview_pick_fast_button, &handview_pick_sway_button, &handview_pick_chime_button,
                &handview_pick_old_button, &handview_pick_punch_button,
                &handview_pick_tilt_button};
            const char *style_names[] = {"vanilla", "static", "fast", "sway", "chime", "old", "punch", "tilt"};
            for (int i = 0; i < 8; i++) {
                style_buttons[i]->setColor(style == style_names[i] ?
                    video::SColor(255, 0, 130, 0) : video::SColor(180, 20, 20, 20));
                style_buttons[i]->draw(driver);
            }

            bool left_hand_on = g_settings->getBool("left_hand");
            handview_left_hand_button.addButton(handview_left_hand_button.get_rect(),
                left_hand_on ? L"Left Hand: On" : L"Left Hand: Off");
            handview_left_hand_button.setColor(left_hand_on ?
                video::SColor(255, 0, 130, 0) : video::SColor(180, 20, 20, 20));
            handview_left_hand_button.draw(driver);

            bool no_view_bob_on = g_settings->getBool("no_view_bob");
            handview_no_view_bob_button.addButton(handview_no_view_bob_button.get_rect(),
                no_view_bob_on ? L"NoViewBob: On" : L"NoViewBob: Off");
            handview_no_view_bob_button.setColor(no_view_bob_on ?
                video::SColor(255, 0, 130, 0) : video::SColor(180, 20, 20, 20));
            handview_no_view_bob_button.draw(driver);

            handview_settings_close_button.draw(driver);

            if (font) {
                font->draw(L"HandView settings", core::rect<s32>(panel.UpperLeftCorner.X + 20,
                    panel.UpperLeftCorner.Y + 15, panel.LowerRightCorner.X - 100, panel.UpperLeftCorner.Y + 40),
                    video::SColor(255, 255, 255, 255));

                s32 slider_x = panel.UpperLeftCorner.X + 20;
                s32 label_y = handview_offset_x_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring wox = stringToWString("Offset X: " +
                    std::to_string(handview_offset_x_scrollbar->getPos()));
                font->draw(wox.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 220, 220, 220));

                label_y = handview_offset_y_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring woy = stringToWString("Offset Y: " +
                    std::to_string(handview_offset_y_scrollbar->getPos()));
                font->draw(woy.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 220, 220, 220));

                label_y = handview_offset_z_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring woz = stringToWString("Offset Z: " +
                    std::to_string(handview_offset_z_scrollbar->getPos()));
                font->draw(woz.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 220, 220, 220));

                label_y = handview_scale_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring wsc = stringToWString("Scale: " +
                    std::to_string(handview_scale_scrollbar->getPos()) + "%");
                font->draw(wsc.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 220, 220, 220));

                const wchar_t *hint = L"Enable HandView (left-click the tile) to actually apply these.";
                font->draw(hint, core::rect<s32>(panel.UpperLeftCorner.X + 20,
                    handview_scale_scrollbar->getRelativePosition().LowerRightCorner.Y + 15,
                    panel.LowerRightCorner.X - 20,
                    handview_scale_scrollbar->getRelativePosition().LowerRightCorner.Y + 35),
                    video::SColor(200, 190, 190, 190));
            }

            g_settings->setFloat("handview_offset_x", (float)handview_offset_x_scrollbar->getPos());
            g_settings->setFloat("handview_offset_y", (float)handview_offset_y_scrollbar->getPos());
            g_settings->setFloat("handview_offset_z", (float)handview_offset_z_scrollbar->getPos());
            g_settings->setFloat("handview_scale", handview_scale_scrollbar->getPos() / 100.0f);
        }

        if (colors_panel_open) {
            core::rect<s32> panel = getColorsPanelRect();
            ModernUI::panel(driver, panel, ModernUI::Radius, video::SColor(230, 20, 20, 20), getMineBoostGuiColor());

            std::vector<ColorTarget> targets = getColorTargets();

            for (size_t i = 0; i < colors_target_buttons.size(); i++) {
                bool selected = (i == colors_selected_index);
                colors_target_buttons[i].setColor(selected ?
                    video::SColor(255, 0, 130, 0) : video::SColor(180, 20, 20, 20));
                colors_target_buttons[i].draw(driver);
            }

            colors_settings_close_button.draw(driver);

            if (font && colors_selected_index < targets.size()) {
                const ColorTarget &target = targets[colors_selected_index];

                font->draw(L"Colors", core::rect<s32>(panel.UpperLeftCorner.X + 20,
                    panel.UpperLeftCorner.Y + 15, panel.LowerRightCorner.X - 100, panel.UpperLeftCorner.Y + 40),
                    video::SColor(255, 255, 255, 255));

                s32 slider_x = colors_r_scrollbar->getRelativePosition().UpperLeftCorner.X;

                s32 label_y = colors_r_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring wr = stringToWString("Red: " + std::to_string(colors_r_scrollbar->getPos()));
                font->draw(wr.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 255, 120, 120));

                label_y = colors_g_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring wg = stringToWString("Green: " + std::to_string(colors_g_scrollbar->getPos()));
                font->draw(wg.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 120, 255, 120));

                label_y = colors_b_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                std::wstring wb = stringToWString("Blue: " + std::to_string(colors_b_scrollbar->getPos()));
                font->draw(wb.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                    video::SColor(255, 120, 170, 255));

                if (target.has_alpha) {
                    label_y = colors_a_scrollbar->getRelativePosition().UpperLeftCorner.Y - 18;
                    std::wstring wa = stringToWString("Alpha: " + std::to_string(colors_a_scrollbar->getPos()));
                    font->draw(wa.c_str(), core::rect<s32>(slider_x, label_y, slider_x + 300, label_y + 18),
                        video::SColor(255, 220, 220, 220));
                }

                // Live preview swatch, updated from the sliders every frame
                // (same values written to g_settings just below).
                video::SColor preview(255,
                    (u32)colors_r_scrollbar->getPos(),
                    (u32)colors_g_scrollbar->getPos(),
                    (u32)colors_b_scrollbar->getPos());
                s32 swatch_size = 40;
                s32 swatch_x = panel.LowerRightCorner.X - swatch_size - 20;
                s32 swatch_y = panel.UpperLeftCorner.Y + 55;
                core::rect<s32> swatch_rect(swatch_x, swatch_y, swatch_x + swatch_size, swatch_y + swatch_size);
                driver->draw2DRectangle(preview, swatch_rect);
                driver->draw2DRectangleOutline(swatch_rect, video::SColor(255, 255, 255, 255));

                const wchar_t *hint = L"Pick a target on the left, then drag the sliders to recolor it.";
                s32 hint_y = colors_b_scrollbar->getRelativePosition().LowerRightCorner.Y + 30;
                font->draw(hint, core::rect<s32>(panel.UpperLeftCorner.X + 20, hint_y,
                    panel.LowerRightCorner.X - 20, hint_y + 20),
                    video::SColor(200, 190, 190, 190));

                // Live-apply: written every frame while the panel is open,
                // same pattern as HandView's offset/scale sliders above.
                v3f color((f32)colors_r_scrollbar->getPos(),
                    (f32)colors_g_scrollbar->getPos(),
                    (f32)colors_b_scrollbar->getPos());
                g_settings->setV3F(target.setting, color);
                if (target.has_alpha)
                    g_settings->setS32(target.alpha_setting, colors_a_scrollbar->getPos());
            }
        }

        if (!colors_panel_open) {
            if (g_settings->getBool("fov_custom")) {
                g_settings->setFloat("fov_custom.data", scrollbar->getPos());
            }

            g_settings->setU16("fps_max", fps_scrollbar->getPos());
            g_settings->setS32("hit_particle_amount", hitparticle_scrollbar->getPos());
            g_settings->setS32("target_highlight_particle_amount", target_particle_scrollbar->getPos());
            g_settings->setFloat("hud_size", hud_size_scrollbar->getPos() / 100.0f);

            if (current_category == SettingCategory::Scrollbars) {
                std::wstring wfov = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes("FOV: " + std::to_string(int(g_settings->getFloat("fov_custom.data"))));

                int offsetX = 190;
                font->draw(wfov.c_str(), core::rect<s32>(((screenW - 300) / 2 + (-45)) * 1.72 - offsetX, scrollbarTop,
                (screenW + 300) / 2 + (-45) - offsetX, scrollbarTop + 20), video::SColor(255, 255, 255, 255));

                std::wstring wfps = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes("FPS: " + std::to_string(int(fps_scrollbar->getPos())));
                font->draw(wfps.c_str(), core::rect<s32>(((screenW - 300) / 2 + (-45)) * 1.72 - offsetX, fpsScrollbarTop,
                (screenW + 300) / 2 + (-45) - offsetX, fpsScrollbarTop + 20), video::SColor(255, 255, 255, 255));

                std::wstring whit = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes("Hit Particles: " + std::to_string(int(hitparticle_scrollbar->getPos())));
                font->draw(whit.c_str(), core::rect<s32>(((screenW - 300) / 2 + (-45)) * 1.72 - offsetX, hitparticleScrollbarTop,
                (screenW + 300) / 2 + (-45) - offsetX, hitparticleScrollbarTop + 20), video::SColor(255, 255, 255, 255));

                std::wstring wtarget = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes("Target Particles: " + std::to_string(int(target_particle_scrollbar->getPos())));
                font->draw(wtarget.c_str(), core::rect<s32>(((screenW - 300) / 2 + (-45)) * 1.72 - offsetX, targetParticleScrollbarTop,
                (screenW + 300) / 2 + (-45) - offsetX, targetParticleScrollbarTop + 20), video::SColor(255, 255, 255, 255));

                std::wstring whudsize = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes("HUD Size: " + std::to_string(int(hud_size_scrollbar->getPos())) + "%");
                font->draw(whudsize.c_str(), core::rect<s32>(((screenW - 300) / 2 + (-45)) * 1.72 - offsetX, hudSizeScrollbarTop,
                (screenW + 300) / 2 + (-45) - offsetX, hudSizeScrollbarTop + 20), video::SColor(255, 255, 255, 255));
            }
        }
    } else {
        scrollbar->setVisible(false);
        fps_scrollbar->setVisible(false);
        hitparticle_scrollbar->setVisible(false);
        target_particle_scrollbar->setVisible(false);
        hud_size_scrollbar->setVisible(false);
        if (photo_panel.isOpen())
            photo_panel.close();
        if (handview_settings_open)
            closeHandViewSettings();
    }
}

Menu::~Menu()
{
    if (s_instance == this)
        s_instance = nullptr;

    if (scrollbar) scrollbar->remove();
    if (fps_scrollbar) fps_scrollbar->remove();
    if (hitparticle_scrollbar) hitparticle_scrollbar->remove();
    if (target_particle_scrollbar) target_particle_scrollbar->remove();
    if (hud_size_scrollbar) hud_size_scrollbar->remove();
    if (handview_offset_x_scrollbar) handview_offset_x_scrollbar->remove();
    if (handview_offset_y_scrollbar) handview_offset_y_scrollbar->remove();
    if (handview_offset_z_scrollbar) handview_offset_z_scrollbar->remove();
    if (handview_scale_scrollbar) handview_scale_scrollbar->remove();
}