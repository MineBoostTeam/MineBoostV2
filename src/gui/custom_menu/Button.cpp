#include "Button.h"
#include "gui/custom_menu/ModernUI.h"
#include "log.h"

void Button::addButton(core::rect<s32> position, std::wstring title)
{
    this->position = position;
    this->title = title;
}

void Button::setColor(video::SColor color)
{
    this->color = color;
}

void Button::setFontColor(video::SColor font_color)
{
    this->font_color = font_color;
}

void Button::draw(video::IVideoDriver *driver)
{
    gui::IGUIFont* font = g_fontengine->getFont(FONT_SIZE_UNSPECIFIED, FM_Standard);

    if (IsVisible() && font) {
        video::SColor fill = is_active ?
            video::SColor(230, 34, 40, 58) : color;
        video::SColor border = is_active ?
            ModernUI::PanelBorder : ModernUI::PanelBorderDim;
        s32 thickness = is_active ? 2 : 1;

        ModernUI::panel(driver, position, ModernUI::RadiusSmall, fill, border,
            /*shadow=*/false, thickness);

        core::dimension2du textSize = font->getDimension(title.c_str());
        s32 textX = position.UpperLeftCorner.X + (position.getWidth() - textSize.Width) / 2;
        s32 textY = position.UpperLeftCorner.Y + (position.getHeight() - textSize.Height) / 2;

        font->draw(title.c_str(), core::rect<s32>(textX, textY, textX + textSize.Width, textY + textSize.Height),
         font_color);
    }
}

bool Button::isPressed(const irr::SEvent &event)
{
    // Invisible buttons keep their last on-screen rect (addButton() only
    // ever repositions, see Button::addButton() above) -- without this
    // check, a click landing on a now-hidden button's stale rect still
    // fired its callback. That's how the PhotoHUD/HandView advanced-
    // settings panels could misfire each other's buttons: they share the
    // exact same screen rect (getHandViewSettingsPanelRect(), src/gui/
    // custom_menu/Menu.cpp, returns PhotoHudSettingsMenu::getPanelRect()),
    // and until PhotoHudSettingsMenu::open()/Menu::openHandViewSettings()
    // were made mutually exclusive (see those functions), both could
    // legitimately be considered "open" at once with all their buttons
    // still
    // isPressed()-reachable regardless of which panel was actually drawn
    // on top.
    if (!is_visible)
        return false;

    if (event.EventType == irr::EET_MOUSE_INPUT_EVENT) {
        if (event.MouseInput.Event == irr::EMIE_LMOUSE_PRESSED_DOWN) {
            if (position.isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y))) {
                setColor(video::SColor(166, 0, 0, 0));
                if (onClickCallback) {
                    // A click handler throwing (a std::exception from a
                    // settings read with a missing default, a bad_alloc,
                    // std::filesystem errors from a user-supplied path,
                    // ...) used to propagate straight up through Irrlicht's
                    // event loop, which has no catch of its own -- an
                    // uncaught exception there is std::terminate(), i.e. an
                    // immediate hard crash of the whole client, for every
                    // button that shares this class (including PhotoHUD's
                    // "Use" and ClientChat's "Login with Discord"). This is
                    // the single choke point every one of those callbacks
                    // already goes through, so it's also the right place to
                    // guarantee none of them can ever take the client down
                    // with them -- whatever went wrong inside the callback
                    // is a bug to fix in that callback, not a reason to
                    // crash the whole game out from under the player.
                    try {
                        onClickCallback();
                    } catch (const std::exception &e) {
                        errorstream << "[MineBoost] Button click handler threw: "
                            << e.what() << std::endl;
                    } catch (...) {
                        errorstream << "[MineBoost] Button click handler threw a "
                            "non-std::exception" << std::endl;
                    }
                }
                return true;
            }
        }
    }

    if (event.EventType == irr::EET_MOUSE_INPUT_EVENT &&
            event.MouseInput.Event == irr::EMIE_LMOUSE_LEFT_UP) {
        setColor(video::SColor(105, 0, 0, 0));
        if (position.isPointInside(core::vector2d<s32>(event.MouseInput.X, event.MouseInput.Y)))
            return true;
        return false;
    }

    return false;
}
