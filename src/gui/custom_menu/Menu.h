#ifndef MENU_H
#define MENU_H

#define WIDTH_ 800
#define HEIGHT_ 450

#include <irrlicht.h>
#include "IGUIEnvironment.h"
#include "gui/modalMenu.h"
#include <vector>
#include "client/client.h"
#include "irrlichttypes_extrabloated.h"
#include "IVideoDriver.h"
#include "client/game.h"
#include "client/fontengine.h"
#include "IGUIFont.h"
#include "IGUIScrollBar.h"
#include "Button.h"
#include "Items.h"
#include "settings.h"

using namespace irr;
using namespace gui;
#include <map>
#include <string>
#include <vector>
#include <locale>
#include <codecvt>

struct Sprite_
{
    int x = 0, y = 0;
    int width = 0;
    int height = 0;
    core::rect<s32> get_rect() const {return core::rect<s32>(x, y, x + width, y + height);}
    bool isDragging = false;
    bool changedPos = false;

    void save(s32 screenWidth, s32 screenHeight, std::string name) {
        if (x < 0) {
            x = 0;
        } else if (y + height > screenHeight) {
            y = screenHeight - height;
        }
        g_settings->setV2F(name, v2f(x, y));
    }

	void save(s32 screenWidth, s32 screenHeight, std::string name, std::string name1) {
		g_settings->setFloat(name, x);
		g_settings->setFloat(name1, y);
	}

};

class Menu: public IGUIElement
{
public:
    Menu(gui::IGUIEnvironment* env, gui::IGUIElement* parent, s32 id,
    IMenuManager* menumgr, Client *client);
    ~Menu();

    // Called directly from MyEventReceiver::OnEvent() in
    // src/client/inputhandler.cpp for every raw input event, unconditionally
    // -- not just while this menu happens to be open. That's the whole
    // point of a keybind: it needs to work in normal gameplay too.
    //
    // This used to live inline in OnEvent() instead, on the assumption
    // that Irrlicht would keep delivering events to this element even
    // while "closed". It doesn't: close() explicitly drops this element's
    // focus (Environment->removeFocus(this)) and shrinks it to a
    // zero-size rect so it stops swallowing clicks meant for the game
    // world/formspecs underneath (see the comment in close()) -- but
    // Irrlicht's GUI environment normally routes EET_KEY_INPUT_EVENT to
    // whichever element has focus, and EET_MOUSE_INPUT_EVENT to whichever
    // element the cursor hit-tests against, so once neither is true for
    // this element anymore, it silently stopped receiving the events a
    // bind depends on. A keybind pressed while the menu was open would
    // still reach OnEvent() as before and toggle fine; the same key
    // pressed with the menu closed just never arrived. Whichever of
    // those two states a given setting happened to be in from the last
    // menu session determined which single direction still worked from
    // there on.
    //
    // Calling it from the raw input receiver instead sidesteps all of
    // that: it no longer depends on focus or hit-testing at all, so it
    // toggles the same way whether the menu is open, closed, or was
    // never opened this session. No null check is required by the
    // caller -- this is a no-op before the menu has been created yet.
    static void checkGlobalBinds(const irr::SEvent &event);

    // Whether this instance was constructed with a real Client -- false
    // for the standalone instance ClientLauncher::main_menu() creates so
    // the settings menu (Colors, HandView, keybinds, ...) is reachable
    // from the title screen too, before ever joining a world. Almost
    // everything in this class already tolerated m_client being null
    // (see the "m_client ?"/"m_client &&" checks sprinkled through
    // Menu.cpp) -- PhotoHUD's texture and the coords preview's live
    // position are the only two things that actually need a Client, and
    // both already degrade gracefully without one (blank texture / the
    // placeholder sample coordinates respectively).
    bool hasClient() const { return m_client != nullptr; }

    // Called from checkMainMenuOpenKeybind() below when "keymap_menu" is
    // pressed and this instance has no Client -- i.e. only the title-
    // screen instance responds to it this way. The in-game instance
    // keeps using Game::processKeyInput()'s wasKeyDown(KeyType::MENU)
    // exactly as before; this isn't wired into that path at all, so
    // there's no risk of the two double-toggling each other.
    void toggleOpenFromKeybind()
    {
        if (isOpen)
            close();
        else
            create();
    }

    // Companion to checkGlobalBinds() above, called from the same place
    // in MyEventReceiver::OnEvent() (src/client/inputhandler.cpp) --
    // toggles s_instance open/closed on "keymap_menu", but ONLY when
    // s_instance has no Client (see hasClient() above), i.e. only for
    // the title-screen instance ClientLauncher::main_menu() creates.
    // Keeping this entirely separate from checkGlobalBinds() (rather
    // than folding it in there) means a bound function whose key happens
    // to collide with "keymap_menu" still toggles normally in-game
    // instead of being shadowed by this.
    static void checkMainMenuOpenKeybind(const irr::SEvent &event);

    SettingCategory current_category = SettingCategory::GUI;
    void create();

    void close();

    void initCategoryButtons();
    // Repositions the already-created category tab buttons (GUI/Render/
    // Movement/Scrollbars) to match the current screenW/screenH, without
    // touching the `buttons` vector itself -- unlike initCategoryButtons(),
    // safe to call every time the menu opens (see create()).
    void repositionCategoryButtons();

    void ItemsInit(SettingCategory category);

    void onCategoryButtonClick(SettingCategory category);
    // Highlights whichever of the 4 category tab buttons matches
    // current_category (see Button::setActive() in Button.h/.cpp) --
    // relies on `buttons` being populated in the exact GUI/Render/
    // Movement/Scrollbars order pushed by initCategoryButtons() below.
    void updateCategoryButtonActiveStates();

    virtual bool OnEvent(const irr::SEvent& event);

    virtual void draw();

    void updateScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);
    void updateFpsScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);
    void updateHitParticleScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);
    void updateTargetParticleScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);
    void updateHudSizeScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);
    void updatePlaceCooldownScrollBarPosition(gui::IGUIScrollBar* scrollbar, int screenW, int screenH);

    std::vector<Setting> getSettings() {
        std::vector<Setting> settings;

        settings.push_back({"KeyStroker", "show_keys", SettingCategory::GUI});
        settings.push_back({"ShowCPS", "show_cps", SettingCategory::GUI});
        settings.push_back({"ShowCoords", "show_coords", SettingCategory::GUI});
        settings.push_back({"ShowFPS", "show_fps", SettingCategory::GUI});
        settings.push_back({"ShowPing", "show_ping", SettingCategory::GUI});
        settings.push_back({"NowPlaying", "music_hud", SettingCategory::GUI});
        settings.push_back({"InventoryHUD", "inventory_hud", SettingCategory::GUI});
        settings.push_back({"CraftHUD", "craft_hud", SettingCategory::GUI});
        // settings.push_back({"ArmorHUD", "armor_hud", SettingCategory::GUI}); // ArmorHUD temporarily disabled

        settings.push_back({"Water Effect", "small_post_effect_color", SettingCategory::RENDER});
        settings.push_back({"Node \n illumination", "node_illumination", SettingCategory::RENDER});
        settings.push_back({"Display sunrise", "display_sunrise", SettingCategory::RENDER});
        settings.push_back({"Disable stars", "disable_stars", SettingCategory::RENDER});
        settings.push_back({"CustomFog", "use_custom_fog_color", SettingCategory::RENDER});
        settings.push_back({"Sky color","use_custom_sky_color", SettingCategory::RENDER});
        settings.push_back({"Particles", "particles", SettingCategory::RENDER});

        settings.push_back({"TargetHUD", "target_hud", SettingCategory::GUI});
        settings.push_back({"TargedESP", "target_highlight_particles", SettingCategory::RENDER});
        settings.push_back({"PhotoHUD", "photo_hud", SettingCategory::GUI});
        settings.push_back({"HandView", "handview_enabled", SettingCategory::RENDER});
        return settings;
    }

    std::wstring stringToWString(const std::string& str) {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.from_bytes(str);
    }

private:
    // Set in the constructor, cleared in the destructor -- see
    // checkGlobalBinds() above. There's only ever one Menu instance
    // (game.cpp owns it directly, not via a container), so a plain
    // static pointer is enough; no lifetime/ownership implied here.
    static Menu *s_instance;

    // Updated from every EET_MOUSE_INPUT_EVENT's own X/Y (the same
    // coordinate space Irrlicht uses for all GUI hit-testing, e.g.
    // items[i].get_rect().isPointInside()) -- used instead of
    // RenderingEngine::get_raw_device()->getCursorControl()->getPosition()
    // for the "`" clear-bind key below, since that queries the raw
    // device cursor and isn't guaranteed to agree with GUI-space
    // coordinates under DPI/hud scaling (see the "`" handling in
    // OnEvent()).
    core::position2d<s32> last_mouse_pos = core::position2d<s32>(0, 0);

    bool editMode = false;
    bool altPressed = false;
    IMenuManager* m_menumgr;
    Client* m_client;
    gui::IGUIEnvironment* env;

    video::IVideoDriver* driver = nullptr;

    bool isOpen = false;
    s32 screenW, screenH;
    std::vector<Button> buttons;
    Button hud_move_button;
    std::vector<Items> items;

    gui::IGUIElement* parent;

    static Sprite_ coords_sprite;
    core::vector2d<s32> offset;
    core::rect<s32> pos;
    IGUIScrollBar* scrollbar;
    IGUIScrollBar* fps_scrollbar;
    IGUIScrollBar* hitparticle_scrollbar;
    IGUIScrollBar* target_particle_scrollbar;
    IGUIScrollBar* hud_size_scrollbar;
    // Place-repeat cooldown ("repeat_place_time", 0.001-2.0s, see
    // fast_place removal) -- stored on the slider as whole milliseconds
    // (1-2000) since IGUIScrollBar only deals in integer positions, then
    // divided by 1000 when written back to the float setting in draw().
    IGUIScrollBar* place_cooldown_scrollbar;
    gui::IGUIFont* font = g_fontengine->getFont(FONT_SIZE_UNSPECIFIED, FM_Standard);

    int scrollbarTop;
    int fpsScrollbarTop;
    int hitparticleScrollbarTop;
    int targetParticleScrollbarTop;
    int hudSizeScrollbarTop;
    int placeCooldownScrollbarTop;
    // Last value actually written to "repeat_place_time" -- see draw(),
    // where this guards the write. "repeat_place_time" has a
    // registered Game::settingChangedCallback (so the live cooldown
    // actually updates while dragging), and g_settings->set() fires
    // that callback unconditionally on every call regardless of
    // whether the value changed -- so without this guard, having the
    // MineBoost menu open at all (any tab but Colors) meant a full
    // Game::readSettings() re-parse every single frame, forever, not
    // just while actually dragging the slider.
    s32 placeCooldownLastWrittenMs = -1;

    // Per-function keybinds (up to 2 per setting -- see "bind_<setting>"
    // in g_settings, e.g. "bind_fullbright" = "KEY_F6,WHEEL_UP"). Middle-
    // click a tile in the settings menu to (re)bind it; the next
    // key/mouse-wheel/side-button press is captured. See
    // startBindCapture()/eventToBindToken() in Menu.cpp and the global
    // bind-matching check at the top of OnEvent(), which runs regardless
    // of whether the menu is even open.
    std::string bind_capture_setting = "";
    int bind_capture_slot = 1;
    void startBindCapture(const std::string &setting_name);

    // "Photo HUD" picker panel: right-clicking the "Photo HUD" tile in the
    // GUI category opens this -- lets you pick which of the 3 built-in
    // photos (face/cat_kuki/mellstroy, see textures/base/pack/) is shown.
    // See openPhotoSettings()/closePhotoSettings().
    bool photo_settings_open = false;
    Button photo_settings_close_button;
    Button photo_pick_face_button;
    Button photo_pick_cat_kuki_button;
    Button photo_pick_mellstroy_button;
    Button photo_pick_pawn_black_button;
    Button photo_pick_pawn_two_black_button;
    void openPhotoSettings();
    void closePhotoSettings();
    core::rect<s32> getPhotoSettingsPanelRect();

    // "Colors" panel: lets the player pick a color for each HUD element
    // via 3 RGB sliders shared across a list of targets on the left --
    // see openColorsPanel()/closeColorsPanel() and the "hud_color_*"
    // settings in src/defaultsettings.cpp. Opened via a persistent corner
    // button (colors_open_button), not tied to any settings-grid tile, so
    // it's reachable from every category. The chat background isn't a
    // target here -- it has a fixed look, see
    // GUIChatConsole::updateBackgroundColor() in src/gui/guiChatConsole.cpp.
    struct ColorTarget
    {
        const wchar_t *label;
        std::string setting;       // v3f "(r,g,b)" setting name
        bool has_alpha;            // true only for the chat background
        std::string alpha_setting; // s32 0-255 setting name, if has_alpha
    };
    std::vector<ColorTarget> getColorTargets();

    Button colors_open_button;
    bool colors_panel_open = false;
    Button colors_settings_close_button;
    std::vector<Button> colors_target_buttons;
    size_t colors_selected_index = 0;
    IGUIScrollBar *colors_r_scrollbar = nullptr;
    IGUIScrollBar *colors_g_scrollbar = nullptr;
    IGUIScrollBar *colors_b_scrollbar = nullptr;
    IGUIScrollBar *colors_a_scrollbar = nullptr;
    void openColorsPanel();
    void closeColorsPanel();
    void selectColorsTarget(size_t index);
    core::rect<s32> getColorsPanelRect();

    // "HandView" picker panel: right-clicking the "HandView" tile opens
    // this -- lets you pick the swing animation style and adjust the
    // hand's offset/scale with embedded sliders. See
    // openHandViewSettings()/closeHandViewSettings().
    bool handview_settings_open = false;
    Button handview_settings_close_button;
    Button handview_pick_vanilla_button;
    Button handview_pick_static_button;
    Button handview_pick_fast_button;
    Button handview_pick_sway_button;
    Button handview_pick_chime_button;
    Button handview_pick_old_button;
    Button handview_pick_punch_button;
    Button handview_pick_tilt_button;
    Button handview_left_hand_button;
    Button handview_no_view_bob_button;
    IGUIScrollBar *handview_offset_x_scrollbar = nullptr;
    IGUIScrollBar *handview_offset_y_scrollbar = nullptr;
    IGUIScrollBar *handview_offset_z_scrollbar = nullptr;
    IGUIScrollBar *handview_scale_scrollbar = nullptr;
    void openHandViewSettings();
    void closeHandViewSettings();
    core::rect<s32> getHandViewSettingsPanelRect();
};

// Returns the built-in texture filename (e.g. "face.png") for the
// currently selected "photo_hud_image" setting value. Shared between
// Menu.cpp (edit-mode preview / picker panel) and Hud::drawPhotoHud()
// (src/client/hud.cpp) so both always agree on what "selected" means.
std::string getSelectedPhotoHudTexture();

#endif
