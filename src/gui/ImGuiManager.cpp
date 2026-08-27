// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- Dear ImGui integration implementation. See ImGuiManager.h
// for the design/lifecycle notes.

#include "ImGuiManager.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"

#include "IVideoDriver.h"
#include "custom_menu/ImGuiMineBoostMenu.h"
#include "imgui_hud.h"
#include "log.h"
#include "porting.h"
#include "settings.h"

#include <algorithm>

ImGuiManager &ImGuiManager::get()
{
	static ImGuiManager instance;
	return instance;
}

void ImGuiManager::init(video::IVideoDriver *driver)
{
	if (m_initialized) {
		warningstream << "[MineBoost] ImGuiManager::init() called while "
			"already initialized -- ignoring." << std::endl;
		return;
	}
	if (!driver) {
		errorstream << "[MineBoost] ImGuiManager::init() called with a "
			"null video driver -- ImGui will stay disabled this session."
			<< std::endl;
		return;
	}

	// Not using exceptions to signal failure below (Dear ImGui itself
	// doesn't throw -- IM_ASSERT() on a fatal misuse instead), but this
	// class's entire contract is "never a reason the client can't start",
	// so a stray exception from anywhere in here (an unexpected driver
	// state, std::bad_alloc from the font atlas build, ...) still
	// shouldn't propagate out and take the whole client down with it.
	try {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO &io = ImGui::GetIO();

		// Step 1 of this integration deliberately doesn't wire up
		// ImGuiConfigFlags_NavEnableKeyboard/Gamepad or docking/viewports
		// -- none of that is needed yet to prove the basic
		// init/render/input pipeline works, and each one is its own can
		// of worms (keyboard nav in particular would fight over
		// Tab/arrow keys with the existing GUI's own navigation). Can be
		// turned on deliberately, one at a time, once real menus start
		// migrating.

		// No imgui.ini -- avoids writing a file nothing currently reads
		// into whatever the process's working directory happens to be.
		// Worth reconsidering once real, migrated menus want their
		// window layout/sizes to persist.
		io.IniFilename = nullptr;

		// Dear ImGui's built-in font (ProggyClean) renders at a tiny,
		// cramped 13px by default -- fine for a debug overlay, not for
		// HUD text players are meant to actually read at a glance. Bump
		// it to something comfortably legible without needing to ship
		// and license a separate TTF; every MineBoostV2-drawn HUD panel
		// already scales its own font size off ImGui::GetFontSize() (see
		// src/gui/imgui_hud.cpp), so this one change raises the base
		// size everywhere at once.
		ImFontConfig font_cfg;
		font_cfg.SizePixels = 20.0f;
		io.Fonts->AddFontDefault(&font_cfg);

		ImGui::StyleColorsDark();

		// ImGui_ImplOpenGL3_Init() with glsl_version == nullptr
		// auto-detects the right #version line from the live GL context
		// (glGetString(GL_VERSION)) -- deliberate, since MineBoostV2/
		// IrrlichtMt can hand back either a desktop GL compatibility
		// context or a GL3.2+ core profile one depending on platform and
		// the user's "video_driver" setting, and this avoids having to
		// hardcode (and keep in sync) which one that is here.
		if (!ImGui_ImplOpenGL3_Init(nullptr)) {
			errorstream << "[MineBoost] ImGui_ImplOpenGL3_Init() failed -- "
				"ImGui will stay disabled this session." << std::endl;
			ImGui::DestroyContext();
			return;
		}

		m_initialized = true;
		m_last_frame_time_ms = 0;
		infostream << "[MineBoost] ImGui initialized (" << IMGUI_VERSION
			<< ")." << std::endl;
	} catch (const std::exception &e) {
		errorstream << "[MineBoost] ImGuiManager::init() failed: "
			<< e.what() << " -- ImGui will stay disabled this session."
			<< std::endl;
		if (ImGui::GetCurrentContext())
			ImGui::DestroyContext();
		m_initialized = false;
	}
}

void ImGuiManager::shutdown()
{
	if (!m_initialized)
		return;

	try {
		ImGui_ImplOpenGL3_Shutdown();
		ImGui::DestroyContext();
	} catch (const std::exception &e) {
		errorstream << "[MineBoost] ImGuiManager::shutdown() failed: "
			<< e.what() << std::endl;
	}
	m_initialized = false;
}

namespace {

// Irrlicht's own EKEY_CODE (irr/include/Keycodes.h) is a superset covering
// every platform it supports; this only needs to cover the keys ImGui
// itself actually reacts to (text-field navigation/editing, widget
// activation, and the modifier keys it tracks for shortcuts) -- not every
// possible EKEY_CODE. Anything not listed here (game-specific keys,
// media keys, IME keys, ...) is simply never forwarded to ImGui, which is
// the correct behavior: those should keep going wherever they already go.
ImGuiKey irrKeyToImGuiKey(irr::EKEY_CODE key)
{
	using namespace irr;
	switch (key) {
	case KEY_TAB: return ImGuiKey_Tab;
	case KEY_LEFT: return ImGuiKey_LeftArrow;
	case KEY_RIGHT: return ImGuiKey_RightArrow;
	case KEY_UP: return ImGuiKey_UpArrow;
	case KEY_DOWN: return ImGuiKey_DownArrow;
	case KEY_PRIOR: return ImGuiKey_PageUp;
	case KEY_NEXT: return ImGuiKey_PageDown;
	case KEY_HOME: return ImGuiKey_Home;
	case KEY_END: return ImGuiKey_End;
	case KEY_INSERT: return ImGuiKey_Insert;
	case KEY_DELETE: return ImGuiKey_Delete;
	case KEY_BACK: return ImGuiKey_Backspace;
	case KEY_SPACE: return ImGuiKey_Space;
	case KEY_RETURN: return ImGuiKey_Enter;
	case KEY_ESCAPE: return ImGuiKey_Escape;
	case KEY_LCONTROL: return ImGuiKey_LeftCtrl;
	case KEY_LSHIFT: return ImGuiKey_LeftShift;
	case KEY_LMENU: return ImGuiKey_LeftAlt;
	case KEY_LWIN: return ImGuiKey_LeftSuper;
	case KEY_RCONTROL: return ImGuiKey_RightCtrl;
	case KEY_RSHIFT: return ImGuiKey_RightShift;
	case KEY_RMENU: return ImGuiKey_RightAlt;
	case KEY_RWIN: return ImGuiKey_RightSuper;
	case KEY_APPS: return ImGuiKey_Menu;
	case KEY_KEY_0: return ImGuiKey_0;
	case KEY_KEY_1: return ImGuiKey_1;
	case KEY_KEY_2: return ImGuiKey_2;
	case KEY_KEY_3: return ImGuiKey_3;
	case KEY_KEY_4: return ImGuiKey_4;
	case KEY_KEY_5: return ImGuiKey_5;
	case KEY_KEY_6: return ImGuiKey_6;
	case KEY_KEY_7: return ImGuiKey_7;
	case KEY_KEY_8: return ImGuiKey_8;
	case KEY_KEY_9: return ImGuiKey_9;
	case KEY_KEY_A: return ImGuiKey_A;
	case KEY_KEY_B: return ImGuiKey_B;
	case KEY_KEY_C: return ImGuiKey_C;
	case KEY_KEY_D: return ImGuiKey_D;
	case KEY_KEY_E: return ImGuiKey_E;
	case KEY_KEY_F: return ImGuiKey_F;
	case KEY_KEY_G: return ImGuiKey_G;
	case KEY_KEY_H: return ImGuiKey_H;
	case KEY_KEY_I: return ImGuiKey_I;
	case KEY_KEY_J: return ImGuiKey_J;
	case KEY_KEY_K: return ImGuiKey_K;
	case KEY_KEY_L: return ImGuiKey_L;
	case KEY_KEY_M: return ImGuiKey_M;
	case KEY_KEY_N: return ImGuiKey_N;
	case KEY_KEY_O: return ImGuiKey_O;
	case KEY_KEY_P: return ImGuiKey_P;
	case KEY_KEY_Q: return ImGuiKey_Q;
	case KEY_KEY_R: return ImGuiKey_R;
	case KEY_KEY_S: return ImGuiKey_S;
	case KEY_KEY_T: return ImGuiKey_T;
	case KEY_KEY_U: return ImGuiKey_U;
	case KEY_KEY_V: return ImGuiKey_V;
	case KEY_KEY_W: return ImGuiKey_W;
	case KEY_KEY_X: return ImGuiKey_X;
	case KEY_KEY_Y: return ImGuiKey_Y;
	case KEY_KEY_Z: return ImGuiKey_Z;
	case KEY_F1: return ImGuiKey_F1;
	case KEY_F2: return ImGuiKey_F2;
	case KEY_F3: return ImGuiKey_F3;
	case KEY_F4: return ImGuiKey_F4;
	case KEY_F5: return ImGuiKey_F5;
	case KEY_F6: return ImGuiKey_F6;
	case KEY_F7: return ImGuiKey_F7;
	case KEY_F8: return ImGuiKey_F8;
	case KEY_F9: return ImGuiKey_F9;
	case KEY_F10: return ImGuiKey_F10;
	case KEY_F11: return ImGuiKey_F11;
	case KEY_F12: return ImGuiKey_F12;
	case KEY_NUMPAD0: return ImGuiKey_Keypad0;
	case KEY_NUMPAD1: return ImGuiKey_Keypad1;
	case KEY_NUMPAD2: return ImGuiKey_Keypad2;
	case KEY_NUMPAD3: return ImGuiKey_Keypad3;
	case KEY_NUMPAD4: return ImGuiKey_Keypad4;
	case KEY_NUMPAD5: return ImGuiKey_Keypad5;
	case KEY_NUMPAD6: return ImGuiKey_Keypad6;
	case KEY_NUMPAD7: return ImGuiKey_Keypad7;
	case KEY_NUMPAD8: return ImGuiKey_Keypad8;
	case KEY_NUMPAD9: return ImGuiKey_Keypad9;
	case KEY_MULTIPLY: return ImGuiKey_KeypadMultiply;
	case KEY_ADD: return ImGuiKey_KeypadAdd;
	case KEY_SUBTRACT: return ImGuiKey_KeypadSubtract;
	case KEY_DECIMAL: return ImGuiKey_KeypadDecimal;
	case KEY_DIVIDE: return ImGuiKey_KeypadDivide;
	case KEY_OEM_1: return ImGuiKey_Semicolon;
	case KEY_PLUS: return ImGuiKey_Equal;
	case KEY_COMMA: return ImGuiKey_Comma;
	case KEY_MINUS: return ImGuiKey_Minus;
	case KEY_PERIOD: return ImGuiKey_Period;
	case KEY_OEM_2: return ImGuiKey_Slash;
	case KEY_OEM_3: return ImGuiKey_GraveAccent;
	case KEY_OEM_4: return ImGuiKey_LeftBracket;
	case KEY_OEM_5: return ImGuiKey_Backslash;
	case KEY_OEM_6: return ImGuiKey_RightBracket;
	case KEY_OEM_7: return ImGuiKey_Apostrophe;
	case KEY_CAPITAL: return ImGuiKey_CapsLock;
	case KEY_SCROLL: return ImGuiKey_ScrollLock;
	case KEY_NUMLOCK: return ImGuiKey_NumLock;
	case KEY_PAUSE: return ImGuiKey_Pause;
	case KEY_SNAPSHOT: return ImGuiKey_PrintScreen;
	default: return ImGuiKey_None;
	}
}

} // namespace

bool ImGuiManager::processEvent(const irr::SEvent &event)
{
	if (!m_initialized)
		return false;

	ImGuiIO &io = ImGui::GetIO();

	switch (event.EventType) {
	case irr::EET_MOUSE_INPUT_EVENT: {
		io.AddMousePosEvent((float)event.MouseInput.X, (float)event.MouseInput.Y);
		switch (event.MouseInput.Event) {
		case irr::EMIE_LMOUSE_PRESSED_DOWN:
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			break;
		case irr::EMIE_LMOUSE_LEFT_UP:
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			break;
		case irr::EMIE_RMOUSE_PRESSED_DOWN:
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			break;
		case irr::EMIE_RMOUSE_LEFT_UP:
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			break;
		case irr::EMIE_MMOUSE_PRESSED_DOWN:
			io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true);
			break;
		case irr::EMIE_MMOUSE_LEFT_UP:
			io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false);
			break;
		case irr::EMIE_MOUSE_WHEEL:
			// Irrlicht's Wheel is already "notches", same unit ImGui
			// wants for AddMouseWheelEvent's Y axis -- no horizontal
			// wheel support on the Irrlicht side to feed the X axis
			// with, so that's always 0.
			io.AddMouseWheelEvent(0.0f, event.MouseInput.Wheel);
			break;
		default:
			// EMIE_MOUSE_MOVED and the various double/triple-click
			// events don't need any extra handling here -- the position
			// update above already covers movement, and ImGui derives
			// double/triple-click itself from AddMouseButtonEvent timing
			// rather than needing to be told directly.
			break;
		}
		return io.WantCaptureMouse;
	}

	case irr::EET_KEY_INPUT_EVENT: {
		io.AddKeyEvent(ImGuiMod_Ctrl, event.KeyInput.Control);
		io.AddKeyEvent(ImGuiMod_Shift, event.KeyInput.Shift);

		ImGuiKey mapped = irrKeyToImGuiKey(event.KeyInput.Key);
		if (mapped != ImGuiKey_None)
			io.AddKeyEvent(mapped, event.KeyInput.PressedDown);

		// Text input: only on key-down, and only an actual printable
		// character (Char == 0 for pure modifier/navigation presses,
		// see the SKeyInput comment in irr/include/IEventReceiver.h).
		// wchar_t on this engine's supported platforms is either UTF-16
		// (Windows) or UTF-32 (Linux/macOS/Android) code units -- ImGui's
		// AddInputCharacter() takes a UTF-32 codepoint directly, and a
		// single BMP character (everything this can realistically
		// produce from one keypress) is numerically identical in either
		// encoding, so no conversion is needed either way. Covers
		// non-ASCII input (e.g. Cyrillic) the same as any other text
		// field in this engine already does.
		if (event.KeyInput.PressedDown && event.KeyInput.Char != 0)
			io.AddInputCharacter((unsigned int)event.KeyInput.Char);

		return io.WantCaptureKeyboard;
	}

	default:
		return false;
	}
}

void ImGuiManager::checkDemoToggleKey(const irr::SEvent &event)
{
	if (event.EventType != irr::EET_KEY_INPUT_EVENT || !event.KeyInput.PressedDown)
		return;

	if (!m_demo_toggle_key_loaded) {
		// Same exists()-guarded pattern as everywhere else new/optional
		// settings are read in this codebase -- defaultsettings.cpp
		// registers a default for "keymap_imgui_demo", but this doesn't
		// *rely* on that (a hand-edited minetest.conf missing it
		// outright shouldn't be able to throw here, on every single key
		// press, for the rest of the session).
		std::string key_name = g_settings->exists("keymap_imgui_demo") ?
			g_settings->get("keymap_imgui_demo") : "KEY_F9";
		try {
			m_demo_toggle_key = KeyPress(key_name.c_str());
		} catch (const UnknownKeycode &e) {
			errorstream << "[MineBoost] Invalid \"keymap_imgui_demo\" (\""
				<< key_name << "\"): " << e.what() << " -- the ImGui "
				"demo window toggle will be disabled this session."
				<< std::endl;
			m_demo_toggle_key = KeyPress();
		}
		m_demo_toggle_key_loaded = true;
	}

	if (KeyPress(event.KeyInput) == m_demo_toggle_key)
		toggleDemoWindow();
}

void ImGuiManager::buildTestWindow()
{
	// Tiny MineBoostV2-specific window alongside ImGui's own demo --
	// proves custom widgets/state work end-to-end (not just the
	// pre-built demo content), without depending on or touching anything
	// in the existing GUI. Toggled by the same "keymap_imgui_demo"
	// keybind as the main demo window (see ImGuiManager.h) -- this is a
	// verification aid for this integration step, not a real menu.
	ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("MineBoostV2 ImGui test", nullptr, ImGuiWindowFlags_NoCollapse)) {
		ImGui::TextWrapped(
			"If you can see this and it responds to your mouse/keyboard, "
			"the ImGui integration is working. This window and the main "
			"ImGui demo window are both purely for verifying that -- "
			"nothing here is a real menu yet.");
		ImGui::Separator();
		ImGui::Text("Frame rate: %.1f FPS (%.3f ms/frame)",
			ImGui::GetIO().Framerate, 1000.0f / std::max(ImGui::GetIO().Framerate, 0.001f));

		static bool demo_checkbox = false;
		ImGui::Checkbox("A checkbox", &demo_checkbox);

		static float demo_slider = 0.5f;
		ImGui::SliderFloat("A slider", &demo_slider, 0.0f, 1.0f);

		static char demo_text[64] = "type here";
		ImGui::InputText("Text input (try Cyrillic too)", demo_text, sizeof(demo_text));
	}
	ImGui::End();
}

void ImGuiManager::renderFrame(video::IVideoDriver *driver)
{
	if (!m_initialized || !driver)
		return;

	try {
		ImGuiIO &io = ImGui::GetIO();

		core::dimension2du screensize = driver->getScreenSize();
		io.DisplaySize = ImVec2((float)screensize.Width, (float)screensize.Height);

		u64 now_ms = porting::getTimeMs();
		float dtime = 1.0f / 60.0f; // sane default for the very first frame
		if (m_last_frame_time_ms != 0 && now_ms > m_last_frame_time_ms)
			dtime = std::min((now_ms - m_last_frame_time_ms) / 1000.0f, 1.0f);
		io.DeltaTime = dtime;
		m_last_frame_time_ms = now_ms;

		ImGui_ImplOpenGL3_NewFrame();
		ImGui::NewFrame();

		if (m_show_demo) {
			ImGui::ShowDemoWindow(&m_show_demo);
			buildTestWindow();
		}

		// Every real MineBoostV2 menu (settings, Colors, HandView,
		// PhotoHUD, ...) -- see src/gui/custom_menu/ImGuiMineBoostMenu.h.
		// A cheap no-op if closed. This is the one place ImGuiManager
		// (a generic ImGui plumbing class, otherwise unaware of any
		// specific MineBoostV2 UI) reaches into menu-specific code --
		// simplest way to guarantee it draws inside the same NewFrame()/
		// Render() pair every frame without every future ImGui window
		// needing its own registration mechanism.
		ImGuiMineBoostMenu::get().draw();

		// Same reasoning, for the GUI-section HUD elements that have
		// been rewritten onto ImGui (see src/gui/imgui_hud.h/.cpp) --
		// unlike the menu above, this runs regardless of whether the
		// settings menu is open, since these draw during normal
		// gameplay. Each element's own Hud::drawX() wrapper (src/client/
		// hud.cpp) already gathered whatever it needs to show earlier
		// this same frame; this just does the actual drawing.
		ImGuiHud::get().render();

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
	} catch (const std::exception &e) {
		errorstream << "[MineBoost] ImGuiManager::renderFrame() failed: "
			<< e.what() << std::endl;
	}
}
