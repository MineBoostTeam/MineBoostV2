// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- Dear ImGui integration, step 1 (see the task this was
// written for): get ImGui itself initialized, rendering, and receiving
// input correctly, fully alongside the existing Irrlicht-based GUI
// (src/gui/custom_menu/Menu.cpp etc.), without touching or depending on
// any of it. No existing menu is migrated to ImGui yet -- that's
// deliberately a separate, later step once this foundation is verified
// solid.
//
// Lifecycle (see the call sites for the authoritative story):
//   - init() is called once, after the rendering device/driver/GL context
//     exists and before the main loop starts (see
//     ClientLauncher::run() in src/client/clientlauncher.cpp) -- NOT
//     per-Game-session like Menu/the old GUIClientChat are, since the GL
//     context and window live for the whole process, across every
//     connect/reconnect and the title screen.
//   - shutdown() is called once, right before that same device/driver is
//     torn down (see ClientLauncher::~ClientLauncher()).
//   - processEvent() is fed every raw input event MineBoostV2 already
//     sees (see MyEventReceiver::OnEvent() in src/client/inputhandler.cpp)
//     -- returns whether ImGui wants to consume it, so it never steals
//     input from the existing GUI/game unless an ImGui window is actually
//     up and hovered/focused (this is exactly what ImGuiIO::
//     WantCaptureMouse/WantCaptureKeyboard already mean; this class does
//     no extra gating of its own on top of that).
//   - renderFrame() runs one whole ImGui frame (NewFrame -> build UI ->
//     Render -> the actual GL draw calls) -- called once per frame from
//     Game::drawScene() (src/client/game.cpp), after all of Irrlicht's
//     own scene/HUD/GUI drawing but still before driver->endScene().
//
// Every public method here is a safe no-op if called before init() or
// after shutdown() (or if init() itself failed) -- this class is
// deliberately allowed to just not work, rather than ever being a reason
// the client can't start or run.

#pragma once

#include <irrlicht.h>
#include "irrlichttypes_extrabloated.h"
#include "client/keycode.h"

namespace irr { namespace video { class IVideoDriver; } }

using namespace irr;

class ImGuiManager
{
public:
	static ImGuiManager &get();

	// See the class comment above for exactly when to call this.
	// Safe to call more than once -- every call after the first
	// successful one is a no-op (logs and returns cleanly rather than
	// re-initializing over an already-live context).
	void init(video::IVideoDriver *driver);

	// See the class comment above. Safe to call even if init() was never
	// called, already failed, or shutdown() was already called once --
	// a no-op in all of those cases.
	void shutdown();

	bool isInitialized() const { return m_initialized; }

	// Feed a raw Irrlicht input event to ImGui. Returns true if ImGui
	// wants to consume it -- the caller (see MyEventReceiver::OnEvent(),
	// src/client/inputhandler.cpp) should stop processing that event any
	// further in that case, same convention as the old GUIClientChat's
	// own forwardEvent(). Always returns false before init()/after
	// shutdown(), and in practice also returns false for basically every
	// event while no ImGui window is open/hovered/focused, since that's
	// exactly what ImGuiIO::WantCaptureMouse/WantCaptureKeyboard track.
	bool processEvent(const irr::SEvent &event);

	// Runs one whole ImGui frame and issues its GL draw calls -- see the
	// class comment above for exactly where/when to call this. A no-op
	// before init()/after shutdown().
	void renderFrame(video::IVideoDriver *driver);

	// Toggles the built-in ImGui demo window (Dear ImGui's own
	// ImGui::ShowDemoWindow(), plus a tiny MineBoostV2-specific test
	// window alongside it) -- this exists purely to verify the
	// integration end-to-end (renders, takes input, no crashes/leaks)
	// while nothing has been migrated onto ImGui yet. See the
	// "keymap_imgui_demo" keybind in src/client/inputhandler.cpp/
	// src/client/game.cpp. Individual menus migrated later should get
	// their own toggles instead of piggybacking on this one.
	void toggleDemoWindow() { m_show_demo = !m_show_demo; }
	bool isDemoWindowVisible() const { return m_show_demo; }

	// Checks a raw key event against "keymap_imgui_demo" and toggles the
	// demo window if it matches -- called unconditionally (not gated by
	// processEvent()'s WantCapture check) from MyEventReceiver::OnEvent()
	// (src/client/inputhandler.cpp), same as Menu::checkGlobalBinds() is,
	// so it works from the title screen too and never blocks whatever
	// else that key would otherwise do. Kept here (rather than needing
	// its own KeyCache-based lookup at the call site, which isn't
	// reachable from that raw IEventReceiver anyway -- see
	// InputHandler::keycache in src/client/inputhandler.h) so every
	// piece of ImGui-specific state/logic stays in this one class.
	void checkDemoToggleKey(const irr::SEvent &event);

private:
	ImGuiManager() = default;
	~ImGuiManager() = default;
	ImGuiManager(const ImGuiManager &) = delete;
	ImGuiManager &operator=(const ImGuiManager &) = delete;

	void buildTestWindow();

	bool m_initialized = false;
	bool m_show_demo = false;
	// Lazily loaded (and cached) from "keymap_imgui_demo" the first time
	// checkDemoToggleKey() runs -- not read in the constructor since
	// g_settings might not even exist yet at static-init time (this is a
	// function-local static via get()), same reasoning as
	// InputHandler::keycache (src/client/inputhandler.h) being populated
	// lazily rather than at construction.
	bool m_demo_toggle_key_loaded = false;
	KeyPress m_demo_toggle_key;
	// Absolute porting::getTimeMs() timestamp of the previous
	// renderFrame() call, used to compute ImGuiIO::DeltaTime ourselves --
	// this class doesn't get a real per-frame dtime handed to it, unlike
	// most of the rest of the client (see Game::step() etc.), since
	// drawScene() (src/client/game.cpp), where renderFrame() is called
	// from, doesn't have one readily available either. 0 means "no
	// previous frame yet" (the very first renderFrame() call this
	// session, right after init()).
	u64 m_last_frame_time_ms = 0;
};
