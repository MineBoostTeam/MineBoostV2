// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 settings menu -- ground-up rewrite on Dear ImGui, replacing
// the old Irrlicht-custom-widget Menu class (src/gui/custom_menu/Menu.h/
// .cpp, kept in the tree but no longer built -- see src/gui/CMakeLists.txt
// -- purely as reference while anything not yet ported here, currently
// just "Move HUD" drag-repositioning, gets finished in a follow-up pass;
// see the note on that below).
//
// This is a singleton (same convention as ImGuiManager, since like ImGui
// itself this only ever needs one instance for the whole process,
// spanning the title screen and every game session) rather than something
// Game/ClientLauncher construct and own directly the way the old Menu
// was -- setClient() is called instead whenever a Client becomes
// available/unavailable (see game.cpp/clientlauncher.cpp), so the menu
// can be opened identically from the title screen or in-game, with
// whatever needs a live Client (currently just the PhotoHUD preview)
// simply doing nothing useful until one exists.
//
// Every actual settings widget here is a real ImGui widget (Checkbox,
// SliderFloat/Int, ColorEdit3/4, InputText, Button, ...) rather than a
// hand-rolled Button/manual-rect-layout/raw IGUIEditBox the way the old
// Menu did it -- that's the entire point of this rewrite: ImGui already
// handles layout, focus, hover/active state, and text editing correctly,
// so none of that needs reimplementing or re-debugging here.
//
// Bind-capture (an arbitrary key/mouse-wheel/mouse-button press, not just
// a normal ImGui widget click) is the one piece of raw-input handling
// this class still needs -- see processEvent().
#pragma once

#include <string>

class Client;
namespace irr { struct SEvent; }

class ImGuiMineBoostMenu
{
public:
	static ImGuiMineBoostMenu &get();

	// Called whenever a Client becomes available/unavailable -- see
	// Game::initGui() and Game::~Game() in src/client/game.cpp, and the
	// title-screen call site in src/client/clientlauncher.cpp. May be
	// called with nullptr. Currently unused by anything in this class
	// (the PhotoHUD sub-window's preview only shows the selected
	// filename as text, not a live thumbnail -- see the comment on that
	// in drawPhotoHudWindow()) but kept as real, wired-up infrastructure
	// for when that's added properly.
	void setClient(Client *client) { m_client = client; }

	bool isOpen() const { return m_open; }
	void open() { m_open = true; }
	void close();
	void toggle() { m_open ? close() : open(); }

	// Raw input, for bind-capture only -- every other interaction goes
	// through ImGui's own widgets via ImGuiManager::processEvent()
	// (src/gui/ImGuiManager.cpp). Unlike that call, THIS one must run
	// BEFORE ImGuiManager::processEvent() in MyEventReceiver::OnEvent()
	// (src/client/inputhandler.cpp), not after: while actively capturing
	// a bind, the very next key/mouse press has to be intercepted here
	// even though the settings menu window (the thing the player just
	// clicked "Bind: ..." in) is almost certainly still focused/hovered
	// and would otherwise have ImGuiManager's own WantCaptureKeyboard/
	// Mouse check swallow it first. Returns true if an event was
	// actually consumed (a capture was in progress and this was a
	// key/mouse-button event) -- the caller should stop processing that
	// event any further in that case, same "takes priority over
	// absolutely everything else" contract the old Menu's own bind
	// capture had.
	bool processEvent(const irr::SEvent &event);

	// Applies any already-configured bind (set via the "Bind: ..."
	// buttons in the main window -- see drawSettingToggle()) by toggling
	// its setting -- checked completely unconditionally from
	// MyEventReceiver::OnEvent(), so a bound key works during normal
	// gameplay whether or not this menu is even open, exactly like the
	// old Menu::checkGlobalBinds() did (see its own comment, formerly in
	// src/gui/custom_menu/Menu.cpp, for the full story this preserves).
	void checkGlobalBinds(const irr::SEvent &event);

	// Toggles the menu open/closed on "keymap_menu" -- checked
	// completely unconditionally, so it works identically from the
	// title screen or in-game (see the old, more complicated split
	// between Menu::checkMainMenuOpenKeybind()'s title-screen-only path
	// and Game::processKeyInput()'s in-game one this replaces with a
	// single always-on path, now that there's one persistent menu
	// instance instead of two separate ones with different lifetimes).
	void checkOpenKeybind(const irr::SEvent &event);

	// Builds every currently-open MineBoost ImGui window this frame --
	// the main settings window plus whichever of the Colors/HandView/
	// PhotoHUD sub-windows are open. A cheap no-op if the menu itself is
	// closed. Called from ImGuiManager::renderFrame(), between
	// ImGui::NewFrame() and ImGui::Render().
	void draw();

private:
	ImGuiMineBoostMenu() = default;
	ImGuiMineBoostMenu(const ImGuiMineBoostMenu &) = delete;
	ImGuiMineBoostMenu &operator=(const ImGuiMineBoostMenu &) = delete;

	void drawMainWindow();
	void drawScrollbarsSection();
	void drawColorsWindow();
	void drawHandViewWindow();
	void drawPhotoHudWindow();
	// Per-face custom skybox texture paths -- the backend for this
	// (Sky::loadSkyboxFaceTexture()/"skybox_texture_<face>", see
	// src/client/sky.cpp/.h) has existed all along; this is just the
	// settings-menu UI to actually reach it, same relationship as
	// drawPhotoHudWindow() has to PhotoHud.
	void drawSkyTextureWindow();
	// "Move HUD" -- drag-repositioning overlay for every draggable
	// built-in HUD element (Coords/FPS/Ping/Chat/KeyStroker/CPS/
	// NowPlaying/ShowRP/ConsumptionHUD/TargetHUD/InventoryHUD/CraftHUD/
	// PhotoHUD). A fullscreen, background-less ImGui window holding one
	// ImGui::InvisibleButton per element -- see the data-driven
	// kDraggableElements table in the .cpp. Ground-up rewrite of the old
	// Menu::editMode system (which drove this with ~300 lines of raw
	// per-element Irrlicht mouse-event handling in Menu.cpp); toggled
	// the same way, via a "Move HUD" button in the main window.
	void drawHudEditOverlay();
	// One checkbox + bind-capture button for a single Menu::getSettings()-
	// style boolean setting. `label` is the on-screen tile name (e.g.
	// "ShowFPS"), `setting` the actual g_settings key it toggles (e.g.
	// "show_fps").
	void drawSettingToggle(const char *label, const char *setting);

	Client *m_client = nullptr;
	bool m_open = false;
	bool m_show_colors = false;
	bool m_show_handview = false;
	bool m_show_photohud = false;
	bool m_show_sky = false;
	bool m_hud_edit_mode = false;

	// Bind capture: waiting for the next arbitrary key/mouse-wheel/
	// mouse-button press to assign as a keybind for `m_bind_capture_setting`
	// (a plain g_settings key, e.g. "show_fps" -- stored keybinds
	// themselves live under "bind_<setting>", see processEvent()). Empty
	// = not currently capturing. m_bind_capture_slot picks which of the
	// (up to 2) bind slots for that setting gets overwritten.
	std::string m_bind_capture_setting;
	int m_bind_capture_slot = 1;
};
