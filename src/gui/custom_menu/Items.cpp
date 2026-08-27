#include "Items.h"
#include "Menu.h"
#include "gui/custom_menu/ModernUI.h"
#include "../settings.h"

Items::Items(core::rect<s32> position)
{
    this->position = position;
}

void Items::set_color(irr::video::SColor color)
{
    color_def = color;
}

irr::core::rect<s32> Items::get_rect()
{
    return position;
}

void Items::drawSetting(video::IVideoDriver* driver)
{
    s32 screenWidth = driver->getScreenSize().Width;
    s32 screenHeight = driver->getScreenSize().Height;
    s32 rectWidth = 600;
    s32 rectHeight = 400;

    s32 posX = (screenWidth - rectWidth) / 2;
    s32 posY = (screenHeight - rectHeight) / 2;
    if (this->show_setting) {
        driver->draw2DRectangle(video::SColor(155, 0, 255, 0), core::rect<s32>(posX, posY, posX + rectWidth, posY + rectHeight));
    }
}

void Items::set_title(std::wstring title)
{
    this->title = title;
}

bool Items::isPressed(const irr::SEvent& event)
{
    if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
        if (event.MouseInput.Event == irr::EMIE_LMOUSE_PRESSED_DOWN) {
            if (position.isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                if (g_settings->exists(this->setting_item)) {
                    bool currentValue = g_settings->getBool(this->setting_item);
                    g_settings->setBool(this->setting_item, !currentValue);
                }
                return true;
            }
        }
    }

    return false;
}

std::string Items::get_title()
{
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    return converter.to_bytes(title);
}

void Items::setSetting(Setting &setting)
{
    this->setting = setting;
}

void Items::draw(irr::video::IVideoDriver *driver, s32 screenW, s32 screenH)
{
    this->screenW = screenW;
    this->screenH = screenH;
    // g_settings->getBool() (unlike isPressed()'s own read just above, which
    // already checks exists() first) throws SettingNotFoundException for
    // any setting that has neither a saved value nor a registered default
    // -- uncaught here, that's a hard crash of the whole client the moment
    // this tile's category tab is drawn, indistinguishable from a genuine
    // segfault to a player (see e.g. the "use_custom_fog_color" default
    // bug fixed in src/defaultsettings.cpp -- this class had no defense
    // against that ever happening again for THIS or any other tile).
    // exists() is itself layer-aware (checks the defaults layer too, not
    // just a saved value), so this only ever falls back to `false` for a
    // setting_item that's missing from both -- a config bug elsewhere to
    // fix, not something this tile should ever crash the client over.
    bool enabled = g_settings->exists(this->setting_item) &&
        g_settings->getBool(this->setting_item);
    // Same dark ModernUI fill either way (keeps the grid visually calm),
    // but a green accent border when the setting is on vs. the usual dim
    // blue when it's off -- readable at a glance without the old
    // saturated green/blue debug outlines clashing with the rest of the UI.
    irr::video::SColor border = enabled ?
        irr::video::SColor(255, 70, 210, 130) : ModernUI::PanelBorderDim;
    ModernUI::panel(driver, position, ModernUI::RadiusSmall, color_def, border,
        /*shadow=*/false);

    gui::IGUIFont* font = g_fontengine->getFont(FONT_SIZE_UNSPECIFIED, FM_Standard);
    core::dimension2du textSize = font->getDimension(title.c_str());
    // textSize.Width/Height are u32 (Irrlicht's dimension2du). Mixing those
    // with the signed position math below via plain "-" silently promotes
    // the whole expression to unsigned: for any label wider than the tile
    // (position.getWidth() - textSize.Width < 0), that wraps to a huge u32
    // instead of a small negative number, and dividing THAT by 2 doesn't
    // recover the correct value (unsigned division of a wrapped bit
    // pattern isn't the same as the intended signed division) -- so the
    // label silently renders miles off-screen instead of just overflowing
    // the tile a bit. Only bit anyone hit before was "Fast place" (fits
    // fine within the 120px tile), so it went unnoticed until a longer
    // single-word label ("NoFriendDamage") came along. Explicit s32 casts
    // make this ordinary signed arithmetic regardless of label length.
    s32 textW = (s32)textSize.Width, textH = (s32)textSize.Height;
    s32 textX = position.UpperLeftCorner.X + (position.getWidth() - textW) / 2;
    s32 textY = position.UpperLeftCorner.Y + (position.getHeight() - textH) / 5;

    font->draw(title.c_str(), core::rect<s32>(textX, textY, textX + textW, textY + textH),
               irr::video::SColor(255, 255, 255, 255));

    if (has_advanced_settings) {
        const wchar_t *hint = L"RMB: settings";
        core::dimension2du hintSize = font->getDimension(hint);
        s32 hintW = (s32)hintSize.Width, hintH = (s32)hintSize.Height;
        s32 hintX = position.UpperLeftCorner.X + (position.getWidth() - hintW) / 2;
        s32 hintY = position.LowerRightCorner.Y - hintH - 4;
        font->draw(hint, core::rect<s32>(hintX, hintY, hintX + hintW, hintY + hintH),
                   irr::video::SColor(200, 200, 200, 200));
    }

    drawSetting(driver);
}

Items::~Items()
{

}