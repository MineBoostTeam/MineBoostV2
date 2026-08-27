// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 PhotoHUD -- implementation. See photohud.h for the design
// notes this was built around.

#include "photohud.h"

#include "client/texturesource.h"
#include "filesys.h"
#include "gui/custom_menu/ModernUI.h"
#include "IVideoDriver.h"
#include "log.h"
#include "settings.h"
#include "util/numeric.h"

#include <algorithm>

const std::array<PhotoHudBuiltinImage, PhotoHudBuiltinImageCount> PhotoHudBuiltinImages = {{
	{"face", "face.png", L"Face"},
	{"cat_kuki", "cat_kuki.png", L"Cat Kuki"},
	{"mellstroy", "mellstroy.png", L"Mellstroy"},
	{"pawn_black", "PawnWithBlackPeople.png", L"Pawn+1"},
	{"pawn_two_black", "PawnWithTwoBlackPeoples.png", L"Pawn+2"},
}};

namespace {

// g_settings->get()/getBool()/getS32() throw SettingNotFoundException for
// any setting missing BOTH a saved value and a registered default.
// defaultsettings.cpp registers one for every "photo_hud_*" setting this
// reads, but this class doesn't *rely* on that staying true -- every read
// below goes through these exists()-guarded helpers with a sane fallback
// instead, so a missing/corrupted setting can't crash the client the
// moment it's read (whether that's at startup or from the middle of a
// settings-changed callback).
std::string safeGetString(const std::string &name, const std::string &fallback)
{
	return g_settings->exists(name) ? g_settings->get(name) : fallback;
}

bool safeGetBool(const std::string &name, bool fallback)
{
	return g_settings->exists(name) ? g_settings->getBool(name) : fallback;
}

s32 safeGetS32(const std::string &name, s32 fallback)
{
	return g_settings->exists(name) ? g_settings->getS32(name) : fallback;
}

float safeGetFloat(const std::string &name, float fallback)
{
	return g_settings->exists(name) ? g_settings->getFloat(name) : fallback;
}

// Resolves "photo_hud_image" to a textures/base/pack/ filename via the
// PhotoHudBuiltinImages table (photohud.h) -- the single shared place
// this mapping lives, rather than duplicated here and in the settings
// menu. Falls back to the first entry (currently "face.png") for a
// missing/unrecognized value, same as every other fallback in this file.
const char *builtinTextureFilename(const std::string &image_key)
{
	for (const auto &img : PhotoHudBuiltinImages)
		if (image_key == img.settings_key)
			return img.texture_filename;
	return PhotoHudBuiltinImages.front().texture_filename;
}

// Same "hud_color_*" -> SColor resolution Hud::getHudColorSetting() (see
// src/client/hud.cpp) does, kept as its own small local copy rather than
// exported from hud.cpp -- this class is deliberately independent of
// Hud's internals (see the class comment in photohud.h), and this is a
// handful of lines, not worth coupling the two files over.
video::SColor resolveFrameColor()
{
	v3f c = g_settings->getV3F("hud_color_photo").value_or(v3f(255, 255, 255));
	u32 r = rangelim(myround(c.X), 0, 255);
	u32 g = rangelim(myround(c.Y), 0, 255);
	u32 b = rangelim(myround(c.Z), 0, 255);
	return video::SColor(255, r, g, b);
}

} // namespace

void PhotoHud::onSettingChanged(const std::string &name, void *data)
{
	static_cast<PhotoHud *>(data)->refreshFromSettings();
}

void PhotoHud::init(video::IVideoDriver *driver, ITextureSource *tsrc)
{
	if (m_initialized) {
		warningstream << "[MineBoost] PhotoHud::init() called while "
			"already initialized -- ignoring." << std::endl;
		return;
	}

	m_driver = driver;
	m_tsrc = tsrc;
	m_initialized = true;

	for (const char *setting : {"photo_hud", "photo_hud_show_in_game",
			"photo_hud_image", "photo_hud_custom_path", "photo_hud_size",
			"photo_hud_x", "photo_hud_y"})
		g_settings->registerChangedCallback(setting, &PhotoHud::onSettingChanged, this);

	refreshFromSettings();
}

PhotoHud::~PhotoHud()
{
	// Safe even if init() was never called: deregisterAllChangedCallbacks()
	// is a no-op if nothing was ever registered for `this`, and
	// releaseCustomTexture() checks m_texture itself.
	g_settings->deregisterAllChangedCallbacks(this);
	releaseCustomTexture();
}

void PhotoHud::releaseCustomTexture()
{
	// Only custom (disk-loaded) textures are this class's responsibility
	// to release -- they were fetched directly from IVideoDriver, which
	// otherwise has no way to know MineBoostV2 is done with this one.
	// The 5 built-ins come from ITextureSource::getTexture() instead,
	// which owns and shares them across whatever else in the engine also
	// references that same named texture -- removing one of those here
	// would be a use-after-free waiting to happen for that other code,
	// so those are never touched.
	if (m_texture && m_texture_is_custom && m_driver)
		m_driver->removeTexture(m_texture);
	m_texture = nullptr;
	m_texture_is_custom = false;
}

void PhotoHud::refreshFromSettings()
{
	m_enabled = safeGetBool("photo_hud", false);
	m_show_in_game = safeGetBool("photo_hud_show_in_game", false);
	m_size = rangelim(safeGetS32("photo_hud_size", 200), 16, 4000);
	m_pos_x = safeGetS32("photo_hud_x", -1);
	m_pos_y = safeGetS32("photo_hud_y", -1);

	std::string image_key = safeGetString("photo_hud_image", "face");

	releaseCustomTexture();

	video::ITexture *tex = nullptr;
	bool is_custom = false;

	// try/catch as a second, independent line of defense on top of the
	// exists()-guarded reads above: fs::PathExists()/IsDir() and
	// IVideoDriver::getTexture() are the one part of this class that
	// touches the filesystem/decodes an arbitrary file, so they're the
	// one part most plausibly able to throw something this class didn't
	// anticipate. Any failure here just means "no custom texture this
	// time" -- falls through to the built-in fallback below exactly like
	// an empty/missing/wrong-type path would.
	if (image_key == "custom" && m_driver) {
		try {
			std::string path = safeGetString("photo_hud_custom_path", "");
			if (!path.empty() && fs::PathExists(path) && !fs::IsDir(path)) {
				tex = m_driver->getTexture(path.c_str());
				is_custom = (tex != nullptr);
			}
		} catch (const std::exception &e) {
			errorstream << "[MineBoost] PhotoHud: failed to load custom "
				"image: " << e.what() << std::endl;
			tex = nullptr;
		}
	}

	if (!tex && m_tsrc)
		tex = m_tsrc->getTexture(builtinTextureFilename(image_key));

	m_texture = tex;
	m_texture_is_custom = is_custom;
}

void PhotoHud::draw(const core::dimension2du &screensize, bool gui_is_open) const
{
	if (!m_initialized || !m_driver || !m_enabled || !m_texture)
		return;
	if (!m_show_in_game && !gui_is_open)
		return;

	core::dimension2du imgsize = m_texture->getOriginalSize();
	if (imgsize.Width == 0 || imgsize.Height == 0)
		return;

	// Shared global HUD-scale slider ("hud_size", see defaultsettings.cpp)
	// on top of this element's own "photo_hud_size" -- same clamp range
	// every other MineBoost HUD element applies to it (see e.g.
	// Hud::drawMusicHud() in src/client/hud.cpp). Cheap settings lookup,
	// not filesystem/texture-source work, so read live here rather than
	// cached/cache-invalidated like m_texture above.
	float hud_size_multiplier = rangelim(safeGetFloat("hud_size", 1.0f), 0.5f, 2.5f);
	s32 max_dim = std::max<s32>(16, (s32)(m_size * hud_size_multiplier));
	float scale = std::min(
		(float)max_dim / (float)imgsize.Width,
		(float)max_dim / (float)imgsize.Height);
	s32 draw_w = std::max<s32>(1, (s32)(imgsize.Width * scale));
	s32 draw_h = std::max<s32>(1, (s32)(imgsize.Height * scale));

	// A saved X of -1 means "never moved yet" -> default to screen
	// center. Position is user-draggable via Shift+E edit mode in the
	// MineBoost GUI (see src/gui/custom_menu/Menu.cpp).
	s32 pos_x = m_pos_x < 0 ? ((s32)screensize.Width - draw_w) / 2 : m_pos_x;
	s32 pos_y = m_pos_y < 0 ? ((s32)screensize.Height - draw_h) / 2 : m_pos_y;

	core::rect<s32> dest(pos_x, pos_y, pos_x + draw_w, pos_y + draw_h);

	// Frame behind the photo, padded a few px outward so it's actually
	// visible around an opaque photo -- only its outline is user-
	// colorable via "hud_color_photo" (see the "Colors" panel in
	// src/gui/custom_menu/Menu.cpp). No drop shadow: it smears at the
	// corners on a box this small (same reasoning as every other
	// MineBoost HUD panel, see drawHudColorPanel() in src/client/hud.cpp).
	core::rect<s32> frame(dest.UpperLeftCorner.X - 6, dest.UpperLeftCorner.Y - 6,
		dest.LowerRightCorner.X + 6, dest.LowerRightCorner.Y + 6);
	ModernUI::panel(m_driver, frame, ModernUI::Radius,
		video::SColor(190, 22, 24, 30), resolveFrameColor(), /*shadow=*/false);

	core::rect<s32> src(0, 0, (s32)imgsize.Width, (s32)imgsize.Height);
	m_driver->draw2DImage(m_texture, dest, src, nullptr, nullptr, true);
}
