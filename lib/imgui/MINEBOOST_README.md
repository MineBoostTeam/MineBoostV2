# Dear ImGui (vendored)

Unmodified upstream source from https://github.com/ocornut/imgui, tag
**v1.91.9b**. `LICENSE.txt` is the original upstream license (MIT).

What's here and why:

- Core library: `imgui.h/.cpp`, `imgui_internal.h`, `imgui_draw.cpp`,
  `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`,
  `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`,
  `imconfig.h`.
  `imgui_demo.cpp` (ImGui::ShowDemoWindow()) is included deliberately,
  even though MineBoostV2 doesn't call it in "release" builds -- it's the
  quickest way to sanity-check that a fresh build's ImGui integration
  actually renders/inputs correctly (see the "keymap_imgui_demo" keybind
  in src/client/inputhandler.cpp/game.cpp, and src/gui/ImGuiManager.h/
  .cpp). Costs a bit of extra compile time and binary size; worth it for
  now while this integration is new. Can be dropped from
  `lib/imgui/CMakeLists.txt` later once ImGui usage has settled down and
  active development on it has slowed.
- Renderer backend: `backends/imgui_impl_opengl3.h/.cpp` and its bundled
  `backends/imgui_impl_opengl3_loader.h`. Deliberately **not** vendoring
  `imgui_impl_win32.cpp`/`imgui_impl_sdl2.cpp`/any other platform
  backend -- MineBoostV2's window/input handling is entirely routed
  through Irrlicht's own single `SEvent`-based `MyEventReceiver::OnEvent()`
  choke point already (see src/client/inputhandler.cpp), which already
  differs per platform (CIrrDeviceWin32 vs CIrrDeviceSDL vs ...) *inside*
  Irrlicht. Writing a second, MineBoost-specific "platform backend" against
  that single already-portable event stream (see
  `ImGuiManager::processEvent()` in src/gui/ImGuiManager.cpp) is simpler
  and more maintainable than wiring up a different official ImGui platform
  backend per device Irrlicht supports, and it means ImGui input
  automatically gets the same per-platform event handling Irrlicht/the
  rest of the engine already has (IME, dead keys, touch-to-mouse
  translation, ...) for free, rather than duplicating any of it.
- `imgui_impl_opengl3_loader.h` is ImGui's own bundled, independent GL
  function-pointer loader -- deliberately used instead of MineBoostV2's
  own `OpenGLProcedures` wrapper (irr/include/mt_opengl.h): that wrapper
  exposes GL calls as `GL.SomeFunction(...)` methods on a class instance
  rather than the plain free functions (`glSomeFunction(...)`) ImGui's
  backend calls directly, so bridging the two would need a translation
  shim with no real benefit -- both loaders just resolve function
  pointers against the *same* live GL context/driver, so there's no
  conflict having two independent loaders active at once.

## Updating

Re-run roughly the same clone-and-copy this was vendored with, against
a newer tag:

```sh
git clone --depth 1 --branch <new-tag> https://github.com/ocornut/imgui.git /tmp/imgui_src
cp /tmp/imgui_src/{LICENSE.txt,imgui.h,imgui.cpp,imgui_internal.h,imgui_demo.cpp,imgui_draw.cpp,imgui_tables.cpp,imgui_widgets.cpp,imstb_rectpack.h,imstb_textedit.h,imstb_truetype.h,imconfig.h} lib/imgui/
cp /tmp/imgui_src/backends/{imgui_impl_opengl3.h,imgui_impl_opengl3.cpp,imgui_impl_opengl3_loader.h} lib/imgui/backends/
```

Then update the tag noted at the top of this file and diff-review before
committing, same as any other vendored dependency update.
