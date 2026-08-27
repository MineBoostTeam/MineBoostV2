// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 PhotoHUD settings menu -- implementation. See
// PhotoHudSettingsMenu.h for the design notes.

#include "PhotoHudSettingsMenu.h"

#include "client/client.h"
#include "client/texturesource.h"
#include "gui/custom_menu/ModernUI.h"
#include "filesys.h"
#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/string.h"

#include <algorithm>

namespace {

// g_settings->get()/getBool() throw SettingNotFoundException for any
// setting missing BOTH a saved value and a registered default.
// defaultsettings.cpp registers one for every "photo_hud_*" setting this
// panel touches, but this panel doesn't *rely* on that staying true --
// every read below goes through these exists()-guarded helpers with a
// sane fallback instead. Deliberately local to this file (not shared
// with src/client/photohud.cpp's own copies) -- the two are independent
// classes on purpose, see the file comment in PhotoHudSettingsMenu.h.
std::string safeGetString(const std::string &name, const std::string &fallback)
{
	return g_settings->exists(name) ? g_settings->get(name) : fallback;
}

bool safeGetBool(const std::string &name, bool fallback)
{
	return g_settings->exists(name) ? g_settings->getBool(name) : fallback;
}

video::SColor safeGetColor(const std::string &name, v3f fallback)
{
	v3f c = g_settings->exists(name) ? g_settings->getV3F(name).value_or(fallback) : fallback;
	return video::SColor(255,
		rangelim((s32)myround(c.X), 0, 255),
		rangelim((s32)myround(c.Y), 0, 255),
		rangelim((s32)myround(c.Z), 0, 255));
}

// Accent/border color for MineBoost's UI chrome -- same setting Menu.cpp's
// own getMineBoostGuiColor() reads, duplicated locally rather than shared
// (this class has no dependency on Menu.cpp -- see the file comment in
// PhotoHudSettingsMenu.h).
video::SColor guiAccentColor()
{
	return safeGetColor("mineboost_gui_color", v3f(90, 150, 250));
}

constexpr u64 STATUS_DISPLAY_MS = 4000;

} // namespace

void PhotoHudSettingsMenu::init(gui::IGUIEnvironment *environment, gui::IGUIElement *parent, Client *client)
{
	m_env = environment;
	m_parent = parent;
	m_client = client;

	m_close_button.setOnClick([this]() { close(); });

	for (size_t i = 0; i < PhotoHudBuiltinImages.size(); ++i) {
		const char *key = PhotoHudBuiltinImages[i].settings_key;
		m_pick_buttons[i].setOnClick([key]() { g_settings->set("photo_hud_image", key); });
	}

	m_use_custom_button.setOnClick([this]() { applyCustomPath(); });
	m_show_in_game_button.setOnClick([this]() {
		bool now = !safeGetBool("photo_hud_show_in_game", false);
		g_settings->setBool("photo_hud_show_in_game", now);
		m_show_in_game_button.setText(now ? L"Show in game: ON" : L"Show in game: OFF");
	});
}

core::rect<s32> PhotoHudSettingsMenu::getPanelRect() const
{
	// Guards against being called before init()/with a torn-down
	// environment rather than dereferencing a null video driver --
	// shouldn't normally happen, but a 0x0 rect is a harmless fallback
	// instead of a crash if it ever does.
	if (!m_env || !m_env->getVideoDriver())
		return core::rect<s32>(0, 0, 0, 0);

	s32 sw = (s32)m_env->getVideoDriver()->getScreenSize().Width;
	s32 sh = (s32)m_env->getVideoDriver()->getScreenSize().Height;
	const s32 rectWidth = 600, rectHeight = 480;
	s32 posX = (sw - rectWidth) / 2;
	s32 posY = (sh - rectHeight) / 2;
	return core::rect<s32>(posX, posY, posX + rectWidth, posY + rectHeight);
}

void PhotoHudSettingsMenu::layout()
{
	core::rect<s32> panel = getPanelRect();

	const s32 btn_w = 160, btn_h = 32, gap = 15, per_row = 3;
	s32 col0_x = panel.UpperLeftCorner.X + 20;
	s32 row0_y = panel.UpperLeftCorner.Y + 80;

	for (size_t i = 0; i < m_pick_buttons.size(); ++i) {
		s32 col = (s32)(i % per_row);
		s32 row = (s32)(i / per_row);
		s32 x = col0_x + col * (btn_w + gap);
		s32 y = row0_y + row * (btn_h + gap);
		m_pick_buttons[i].addButton(
			core::rect<s32>(x, y, x + btn_w, y + btn_h),
			PhotoHudBuiltinImages[i].button_label);
		m_pick_buttons[i].setVisible(true);
	}
	s32 rows = (s32)((m_pick_buttons.size() + per_row - 1) / per_row);

	core::rect<s32> close_rect(panel.LowerRightCorner.X - 90, panel.UpperLeftCorner.Y + 10,
		panel.LowerRightCorner.X - 10, panel.UpperLeftCorner.Y + 40);
	m_close_button.addButton(close_rect, L"Close");
	m_close_button.setVisible(true);

	// Custom image path field -- created once (needs a live
	// IGUIEnvironment/parent from init()), just repositioned/re-shown on
	// later opens, and refreshed with whatever's currently saved so
	// reopening the panel doesn't lose track of a path already set.
	s32 row_after_grid_y = row0_y + rows * (btn_h + gap);
	core::rect<s32> path_input_rect(col0_x, row_after_grid_y,
		panel.LowerRightCorner.X - 100, row_after_grid_y + btn_h);
	if (m_env && m_parent) {
		if (!m_custom_path_input)
			m_custom_path_input = m_env->addEditBox(L"", path_input_rect, true, m_parent, -1);
		else
			m_custom_path_input->setRelativePosition(path_input_rect);
		if (m_custom_path_input) {
			m_custom_path_input->setText(
				utf8_to_wide(safeGetString("photo_hud_custom_path", "")).c_str());
			m_custom_path_input->setVisible(true);
		}
	}

	core::rect<s32> use_custom_rect(panel.LowerRightCorner.X - 90, row_after_grid_y,
		panel.LowerRightCorner.X - 20, row_after_grid_y + btn_h);
	m_use_custom_button.addButton(use_custom_rect, L"Use");
	m_use_custom_button.setVisible(true);

	s32 toggle_row_y = row_after_grid_y + btn_h + gap;
	core::rect<s32> show_in_game_rect(col0_x, toggle_row_y,
		col0_x + btn_w + gap + btn_w, toggle_row_y + btn_h);
	m_show_in_game_button.addButton(show_in_game_rect,
		safeGetBool("photo_hud_show_in_game", false) ?
			L"Show in game: ON" : L"Show in game: OFF");
	m_show_in_game_button.setVisible(true);
}

void PhotoHudSettingsMenu::open()
{
	m_open = true;
	layout();
	m_status.clear();
	m_status_expire_ms = 0;
	if (m_env)
		m_env->setFocus(m_parent);
}

void PhotoHudSettingsMenu::close()
{
	m_close_button.setVisible(false);
	for (auto &button : m_pick_buttons)
		button.setVisible(false);
	m_use_custom_button.setVisible(false);
	m_show_in_game_button.setVisible(false);
	if (m_custom_path_input) {
		m_custom_path_input->setVisible(false);
		if (m_env)
			m_env->removeFocus(m_custom_path_input);
	}
	m_open = false;
	if (m_env && m_parent)
		m_env->setFocus(m_parent);
}

void PhotoHudSettingsMenu::applyCustomPath()
{
	if (!m_custom_path_input)
		return;

	// try/catch as a second, more specific line of defense on top of the
	// one Button::isPressed() (src/gui/custom_menu/Button.cpp) already
	// wraps every callback in -- this is the one place that can turn a
	// failure into m_status text the player actually sees.
	try {
		std::string path = wide_to_utf8(m_custom_path_input->getText());

		size_t begin = path.find_first_not_of(" \t\r\n");
		size_t end = path.find_last_not_of(" \t\r\n");
		path = (begin == std::string::npos) ? "" : path.substr(begin, end - begin + 1);

		m_status_expire_ms = porting::getTimeMs() + STATUS_DISPLAY_MS;

		if (path.empty()) {
			m_status = "Enter a file path first";
			return;
		}
		if (!fs::PathExists(path)) {
			m_status = "File not found";
			return;
		}
		if (fs::IsDir(path)) {
			m_status = "That's a folder, not a file";
			return;
		}

		std::string ext = fs::GetFilenameFromPath(path.c_str());
		size_t dot = ext.find_last_of('.');
		ext = (dot == std::string::npos) ? "" : lowercase(ext.substr(dot + 1));
		if (ext != "png" && ext != "jpg" && ext != "jpeg") {
			m_status = "Must be a .png or .jpg file";
			return;
		}

		g_settings->set("photo_hud_custom_path", path);
		g_settings->set("photo_hud_image", "custom");
		m_status = "Applied";
	} catch (const std::exception &e) {
		errorstream << "[MineBoost] PhotoHudSettingsMenu::applyCustomPath() "
			"failed: " << e.what() << std::endl;
		m_status = "Couldn't apply that path (see the log)";
		m_status_expire_ms = porting::getTimeMs() + STATUS_DISPLAY_MS;
	} catch (...) {
		errorstream << "[MineBoost] PhotoHudSettingsMenu::applyCustomPath() "
			"failed with a non-std::exception" << std::endl;
		m_status = "Couldn't apply that path (see the log)";
		m_status_expire_ms = porting::getTimeMs() + STATUS_DISPLAY_MS;
	}
}

bool PhotoHudSettingsMenu::onEvent(const irr::SEvent &event)
{
	if (!m_open)
		return false;

	if (m_close_button.isPressed(event))
		return true;
	for (auto &button : m_pick_buttons)
		if (button.isPressed(event))
			return true;
	if (m_use_custom_button.isPressed(event))
		return true;
	if (m_show_in_game_button.isPressed(event))
		return true;

	// Text entry: the *panel's parent* holds Irrlicht focus (see open()
	// above), not the edit box itself, so keystrokes need to be handed
	// to it explicitly rather than relying on Irrlicht's normal focus-
	// based dispatch to reach it on its own -- same reasoning as
	// GUIClientChat's own input box used to. Enter submits, same as
	// pressing "Use".
	if (event.EventType == irr::EET_KEY_INPUT_EVENT && event.KeyInput.PressedDown) {
		if (event.KeyInput.Key == irr::KEY_RETURN) {
			applyCustomPath();
			return true;
		}
		if (m_custom_path_input)
			m_custom_path_input->OnEvent(event);
		return true;
	}
	if (event.EventType == irr::EET_MOUSE_INPUT_EVENT && m_custom_path_input &&
			m_custom_path_input->getAbsolutePosition().isPointInside(
				core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
		m_custom_path_input->OnEvent(event);
		return true;
	}

	if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
			event.MouseInput.Event == irr::EMIE_LMOUSE_PRESSED_DOWN &&
			!getPanelRect().isPointInside(
				core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
		close();
	}

	return true;
}

video::ITexture *PhotoHudSettingsMenu::resolvePreviewTexture(video::IVideoDriver *driver) const
{
	if (!m_client || !driver)
		return nullptr;

	std::string selected = safeGetString("photo_hud_image", "face");

	if (selected == "custom") {
		try {
			std::string custom_path = safeGetString("photo_hud_custom_path", "");
			if (!custom_path.empty() && fs::PathExists(custom_path) && !fs::IsDir(custom_path)) {
				if (video::ITexture *tex = driver->getTexture(custom_path.c_str()))
					return tex;
			}
		} catch (const std::exception &e) {
			errorstream << "[MineBoost] PhotoHudSettingsMenu: preview failed "
				"to load custom image: " << e.what() << std::endl;
		}
		// Falls through to the built-in fallback below.
	}

	if (!m_client->getTextureSource())
		return nullptr;

	for (const auto &img : PhotoHudBuiltinImages)
		if (selected == img.settings_key)
			return m_client->getTextureSource()->getTexture(img.texture_filename);
	return m_client->getTextureSource()->getTexture(PhotoHudBuiltinImages.front().texture_filename);
}

void PhotoHudSettingsMenu::draw(video::IVideoDriver *driver, gui::IGUIFont *font)
{
	if (!m_open || !driver)
		return;

	core::rect<s32> panel = getPanelRect();
	ModernUI::panel(driver, panel, ModernUI::Radius, video::SColor(230, 20, 20, 20), guiAccentColor());

	std::string selected = safeGetString("photo_hud_image", "face");
	const video::SColor selectedColor(255, 0, 130, 0);
	const video::SColor unselectedColor(180, 20, 20, 20);
	for (size_t i = 0; i < m_pick_buttons.size(); ++i)
		m_pick_buttons[i].setColor(
			selected == PhotoHudBuiltinImages[i].settings_key ? selectedColor : unselectedColor);

	if (font) {
		font->draw(L"PhotoHUD settings", core::rect<s32>(panel.UpperLeftCorner.X + 20, panel.UpperLeftCorner.Y + 15,
			panel.LowerRightCorner.X - 100, panel.UpperLeftCorner.Y + 40),
			video::SColor(255, 255, 255, 255));

		font->draw(L"Which photo should be shown?", core::rect<s32>(panel.UpperLeftCorner.X + 20, panel.UpperLeftCorner.Y + 50,
			panel.LowerRightCorner.X - 20, panel.UpperLeftCorner.Y + 70),
			video::SColor(255, 220, 220, 220));

		s32 rows = (s32)((m_pick_buttons.size() + 2) / 3);
		s32 label_y = panel.UpperLeftCorner.Y + 80 + rows * (32 + 15) + 12;
		font->draw(L"Custom image (full file path):", core::rect<s32>(panel.UpperLeftCorner.X + 20, label_y,
			panel.LowerRightCorner.X - 20, label_y + 12),
			video::SColor(220, 200, 200, 200));

		if (!m_status.empty() && porting::getTimeMs() < m_status_expire_ms) {
			bool ok = (m_status == "Applied");
			video::SColor status_color = ok ?
				video::SColor(255, 140, 220, 140) : video::SColor(255, 230, 130, 130);
			std::wstring wstatus = utf8_to_wide(m_status);
			font->draw(wstatus.c_str(), core::rect<s32>(panel.UpperLeftCorner.X + 20,
				label_y + 58, panel.LowerRightCorner.X - 20,
				label_y + 74), status_color);
		}

		font->draw(L"Only shown while a GUI (inventory, chest, menu...) is open, unless \"Show in game\" is on.",
			core::rect<s32>(panel.UpperLeftCorner.X + 20, panel.LowerRightCorner.Y - 68,
				panel.LowerRightCorner.X - 20, panel.LowerRightCorner.Y - 48),
			video::SColor(200, 190, 190, 190));

		font->draw(L"Drag it around via Move HUD; resize it with the HUD Size slider.",
			core::rect<s32>(panel.UpperLeftCorner.X + 20, panel.LowerRightCorner.Y - 48,
				panel.LowerRightCorner.X - 20, panel.LowerRightCorner.Y - 28),
			video::SColor(200, 190, 190, 190));
	}

	// Live preview of the currently selected photo -- tolerates failure
	// at every step without crashing (see resolvePreviewTexture()).
	video::ITexture *tex = resolvePreviewTexture(driver);
	if (tex) {
		core::dimension2du imgsize = tex->getOriginalSize();
		if (imgsize.Width > 0 && imgsize.Height > 0) {
			const s32 max_dim = 110;
			float scale = std::min((float)max_dim / (float)imgsize.Width,
				(float)max_dim / (float)imgsize.Height);
			s32 w = std::max<s32>(1, (s32)(imgsize.Width * scale));
			s32 h = std::max<s32>(1, (s32)(imgsize.Height * scale));
			s32 cx = panel.UpperLeftCorner.X + (panel.getWidth() - w) / 2;
			s32 cy = panel.LowerRightCorner.Y - 190;
			core::rect<s32> dest(cx, cy, cx + w, cy + h);
			core::rect<s32> src(0, 0, (s32)imgsize.Width, (s32)imgsize.Height);
			driver->draw2DImage(tex, dest, src, nullptr, nullptr, true);
		}
	}

	for (auto &button : m_pick_buttons)
		button.draw(driver);
	m_use_custom_button.draw(driver);
	m_show_in_game_button.draw(driver);
	m_close_button.draw(driver);
}
