#ifndef BUTTON_H
#define BUTTON_H

#include <irrlicht.h>
#include "IGUIEnvironment.h"
#include "gui/modalMenu.h"
#include <vector>
#include "client/client.h"
#include "irrlichttypes_extrabloated.h"
#include "IVideoDriver.h"
#include "client/game.h"
#include "client/fontengine.h"
#include "IGUIFont.h"

using namespace irr;
using namespace gui;

class Button
{
public:

    void addButton(core::rect<s32> position, std::wstring title);

    inline core::rect<s32> get_rect() const { return position; }

    void draw(video::IVideoDriver *driver);

    inline bool IsVisible() { return is_visible; }

    void setVisible(bool flag) { this->is_visible = flag; }

    bool isPressed(const irr::SEvent &event);

    void setColor(video::SColor color);

    void setFontColor(video::SColor font_color);

    void setOnClick(std::function<void()> callback)
    {
        onClickCallback = callback;
    }

    // Updates just the label -- e.g. an on/off toggle button whose text
    // reflects current state (see photo_show_in_game_button in
    // src/gui/custom_menu/Menu.h/.cpp). Position/size/visibility are
    // untouched, unlike calling addButton() again with the same rect.
    inline void setText(std::wstring new_title) { this->title = new_title; }

    // Used for category tab buttons (GUI/Render/Movement/Scrollbars) so the
    // currently-selected tab can be highlighted -- see
    // Menu::updateCategoryButtonActiveStates() in Menu.cpp.
    inline void setActive(bool flag) { this->is_active = flag; }
    inline bool isActive() const { return is_active; }

private:
    core::rect<s32> position;
    std::wstring title;
    bool is_visible = false;
    bool is_active = false;

    video::SColor color = video::SColor(255, 0, 0, 0);
    video::SColor font_color = video::SColor(255, 255, 255, 255);

    std::function<void()> onClickCallback;
};


#endif