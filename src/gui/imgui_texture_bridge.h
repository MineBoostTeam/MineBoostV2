// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- bridges an Irrlicht video::ITexture to something
// ImGui::Image() can display.
//
// Deliberately built ONLY on ITexture's public API (lock()/unlock()/
// getColorFormat()/getSize()/getPitch(), all in irr/include/ITexture.h)
// plus the plain OpenGL functions ImGui's own bundled loader already
// resolved (see lib/imgui/backends/imgui_impl_opengl3_loader.h) -- NOT
// by reaching into IrrlichtMt's private per-driver texture
// implementation classes (irr/src/COpenGLCoreTexture.h and friends).
// Those *do* expose a raw GL texture handle (getOpenGLTextureName()),
// which would avoid the pixel copy this does -- but only on the
// concrete, driver-specific subclass, not on ITexture itself, so using
// it means dynamic_cast-ing against IrrlichtMt's internal class layout,
// which is liable to silently break on a future IrrlichtMt update, and
// getting the include-path/header-collision setup for reaching into
// irr/src's private headers from application code right is its own can
// of worms (its GL headers and ImGui's bundled loader both want to
// define the same GL types/macros in one compile unit). This costs one
// CPU-side pixel copy + a fresh glTexImage2D upload the first time (or
// whenever the source texture pointer changes) a given texture is
// requested, which is the right trade for the handful of small,
// infrequently-changing images this is actually used for (album art
// thumbnails, a texture pack screenshot, player skin avatar crops, item
// icons) -- not something to use for anything large or updated every
// frame.

#pragma once

namespace irr { namespace video { class ITexture; } }

// One of these per on-screen image a caller wants to keep showing
// across frames -- e.g. one ImGuiTextureCache member for "the current
// album art thumbnail" in whatever class draws NowPlaying. Not a global
// cache keyed by texture pointer: callers each own exactly the images
// they need, so there's no unbounded-growth or complicated eviction
// question, and no risk of two unrelated call sites fighting over the
// same slot.
class ImGuiTextureCache
{
public:
	ImGuiTextureCache() = default;
	~ImGuiTextureCache();
	ImGuiTextureCache(const ImGuiTextureCache &) = delete;
	ImGuiTextureCache &operator=(const ImGuiTextureCache &) = delete;

	// Returns an ImTextureID (as a plain void* -- callers pass it
	// straight to ImGui::Image() without needing to know it's really a
	// GLuint under the hood) for `tex`'s current pixels, and the size
	// they were uploaded at (needed for ImGui::Image()'s size
	// parameter). Re-uploads only when `tex` is a different pointer
	// than last time this was called -- the normal case (the same
	// texture requested again next frame) is just a pointer compare
	// after the first call. Returns nullptr (and leaves *out_width/
	// *out_height untouched) for a null `tex`, an empty texture, or one
	// in a pixel format this doesn't know how to convert (see the
	// implementation -- currently just video::ECF_A8R8G8B8, which is
	// what every texture actually used with this ends up as in
	// practice; ImGui::Image() simply isn't called by any caller that
	// gets nullptr back, same as any other "nothing to draw" case
	// elsewhere in this codebase).
	void *get(irr::video::ITexture *tex, int *out_width, int *out_height);

	// Drops the cached upload unconditionally, forcing the next get()
	// call (even for the same texture pointer) to re-upload -- for the
	// rare case a caller knows a texture's *content* changed in place
	// under a stable pointer (nothing currently does this, but it's
	// cheap insurance rather than an unstated assumption).
	void invalidate();

private:
	irr::video::ITexture *m_source = nullptr;
	unsigned int m_gl_texture = 0; // GLuint, 0 = none uploaded yet
	int m_width = 0;
	int m_height = 0;
};
