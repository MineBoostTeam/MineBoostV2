// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- implementation. See imgui_texture_bridge.h for the
// design notes/why this is built the way it is.

#include "imgui_texture_bridge.h"

#include "imgui_impl_opengl3_loader.h" // plain glGenTextures()/glTexImage2D()/etc.
#include "ITexture.h"
#include "log.h"

#include <cstdint>
#include <vector>

ImGuiTextureCache::~ImGuiTextureCache()
{
	if (m_gl_texture != 0)
		glDeleteTextures(1, &m_gl_texture);
}

void ImGuiTextureCache::invalidate()
{
	m_source = nullptr;
}

void *ImGuiTextureCache::get(irr::video::ITexture *tex, int *out_width, int *out_height)
{
	if (!tex)
		return nullptr;

	if (tex == m_source && m_gl_texture != 0) {
		*out_width = m_width;
		*out_height = m_height;
		return (void *)(intptr_t)m_gl_texture;
	}

	// Only this one format is handled -- it's what every texture this
	// class is actually used with (album art fetched over HTTP and
	// decoded by the engine's own image loader, texture-pack
	// screenshots, player skins, item icons -- all regular RGBA/RGB
	// image files) ends up as once loaded, so this isn't a meaningfully
	// narrower restriction in practice; it just means this doesn't try
	// to guess at a conversion for some other format nothing here ever
	// actually produces.
	if (tex->getColorFormat() != irr::video::ECF_A8R8G8B8) {
		warningstream << "[MineBoost] ImGuiTextureCache: texture \""
			<< tex->getName().getInternalName().c_str() << "\" isn't "
			"ECF_A8R8G8B8 (got format " << (int)tex->getColorFormat()
			<< ") -- skipping, won't be shown in any ImGui window."
			<< std::endl;
		return nullptr;
	}

	irr::core::dimension2du size = tex->getSize();
	if (size.Width == 0 || size.Height == 0)
		return nullptr;

	void *pixels = tex->lock(irr::video::ETLM_READ_ONLY);
	if (!pixels) {
		m_source = nullptr;
		return nullptr;
	}

	// ECF_A8R8G8B8 is BGRA in memory (see the comment on this in
	// irr/include/SColor.h) -- GL_BGRA exists on desktop GL but isn't
	// reliably available on GLES (some Android GPUs/drivers lack the
	// extension it needs), and this needs to work on Android too (see
	// lib/imgui/CMakeLists.txt's IMGUI_IMPL_OPENGL_ES3 handling), so
	// this does the B<->R swizzle on the CPU once at upload time rather
	// than relying on a GL format token that isn't universally
	// supported on every platform this runs on.
	std::vector<uint8_t> rgba((size_t)size.Width * size.Height * 4);
	const uint8_t *src = (const uint8_t *)pixels;
	irr::u32 pitch = tex->getPitch();
	for (irr::u32 y = 0; y < size.Height; ++y) {
		const uint8_t *row = src + (size_t)y * pitch;
		uint8_t *dst_row = &rgba[(size_t)y * size.Width * 4];
		for (irr::u32 x = 0; x < size.Width; ++x) {
			dst_row[x * 4 + 0] = row[x * 4 + 2]; // R <- B
			dst_row[x * 4 + 1] = row[x * 4 + 1]; // G
			dst_row[x * 4 + 2] = row[x * 4 + 0]; // B <- R
			dst_row[x * 4 + 3] = row[x * 4 + 3]; // A
		}
	}
	tex->unlock();

	if (m_gl_texture == 0)
		glGenTextures(1, &m_gl_texture);

	// Save/restore the previously-bound 2D texture rather than assuming
	// nothing else cares what's bound afterwards -- this runs at an
	// arbitrary point during ImGui window-building (i.e. well outside
	// imgui_impl_opengl3's own render pass, which does its own state
	// save/restore around actually drawing), interleaved with
	// IrrlichtMt's own rendering in a way this code has no visibility
	// into.
	GLint previous_texture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);

	glBindTexture(GL_TEXTURE_2D, m_gl_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)size.Width, (GLsizei)size.Height,
		0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

	glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);

	m_source = tex;
	m_width = (int)size.Width;
	m_height = (int)size.Height;
	*out_width = m_width;
	*out_height = m_height;
	return (void *)(intptr_t)m_gl_texture;
}
