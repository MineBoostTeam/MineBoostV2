// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2010-2013 blue42u, Jonathon Anderson <anderjon@umail.iu.edu>
// Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

#include "client/hud.h"
#include <string>
#include <utility>
#include <iostream>
#include <cmath>
#include <cwchar>
#include <cctype>
#include <algorithm>
#include "util/string.h"
#include "settings.h"
#include "util/numeric.h"
#include "log.h"
#include "client.h"
#include "inventory.h"
#include "itemdef.h"
#include "itemgroup.h"
#include "shader.h"
#include "client/tile.h"
#include "localplayer.h"
#include "camera.h"
#include "porting.h"
#include "fontengine.h"
#include "guiscalingfilter.h"
#include "mesh.h"
#include "gui/mainmenumanager.h"
#include "wieldmesh.h"
#include "client/renderingengine.h"
#include "client/minimap.h"
#include "client/texturesource.h"
#include "gui/touchcontrols.h"
#include "util/enriched_string.h"
#include "irrlicht_changes/CGUITTFont.h"
#include "IFileSystem.h"
#include "IReadFile.h"
#include "gui/custom_menu/ModernUI.h"

#define OBJECT_CROSSHAIR_LINE_SIZE 8
#define CROSSHAIR_LINE_SIZE 10

// Reads one of the "hud_color_*" settings (see defaultsettings.cpp) and
// returns it as an SColor with the given alpha. Used both as a panel
// background color (MusicHUD/InventoryHUD/CraftHUD/TargetHUD) and as an
// image tint multiplier (PhotoHUD) -- see callers below. Falls back to
// white (no visible change to a multiplied image, or a neutral panel
// color) if the setting is somehow missing/malformed.
static video::SColor getHudColorSetting(const std::string &setting, u32 alpha)
{
	v3f c = g_settings->getV3F(setting).value_or(v3f(255, 255, 255));
	u32 r = rangelim(myround(c.X), 0, 255);
	u32 g = rangelim(myround(c.Y), 0, 255);
	u32 b = rangelim(myround(c.Z), 0, 255);
	return video::SColor(alpha, r, g, b);
}

// Draws a HUD panel whose fill AND border both come from the same
// "hud_color_*" setting (border just at full alpha, fill translucent),
// so a single color pick in the "Colors" panel (see
// src/gui/custom_menu/Menu.cpp) recolors the whole box. Shadow is
// intentionally left off: ModernUI::dropShadow()'s rounded arcs poke out
// past small HUD boxes' corners as solid dark smears rather than a soft
// blur (it reads fine on the big 600x400 settings panels it was designed
// for, not on ~140x30 HUD boxes) -- see the "Colors panel" branch of the
// MineBoost changelog for the report.
static void drawHudColorPanel(video::IVideoDriver *driver,
		const core::rect<s32> &box, const std::string &setting)
{
	// Only the outline/border is user-colorable (see the "Colors" panel in
	// src/gui/custom_menu/Menu.cpp) -- the fill stays this fixed dark
	// shade for every HUD panel, same as it always was before per-element
	// colors existed.
	ModernUI::panel(driver, box, ModernUI::Radius,
		video::SColor(190, 22, 24, 30), getHudColorSetting(setting, 255),
		/*shadow=*/false);
}

static void setting_changed_callback(const std::string &name, void *data)
{
	static_cast<Hud*>(data)->readScalingSetting();
}

Hud::Hud(Client *client, LocalPlayer *player,
		Inventory *inventory)
{
	driver            = RenderingEngine::get_video_driver();
	this->client      = client;
	this->player      = player;
	this->inventory   = inventory;

	readScalingSetting();
	g_settings->registerChangedCallback("dpi_change_notifier", setting_changed_callback, this);
	g_settings->registerChangedCallback("display_density_factor", setting_changed_callback, this);
	g_settings->registerChangedCallback("hud_scaling", setting_changed_callback, this);

	for (auto &hbar_color : hbar_colors)
		hbar_color = video::SColor(255, 255, 255, 255);

	tsrc = client->getTextureSource();

	v3f crosshair_color = g_settings->getV3F("crosshair_color").value_or(v3f());
	u32 cross_r = rangelim(myround(crosshair_color.X), 0, 255);
	u32 cross_g = rangelim(myround(crosshair_color.Y), 0, 255);
	u32 cross_b = rangelim(myround(crosshair_color.Z), 0, 255);
	u32 cross_a = rangelim(g_settings->getS32("crosshair_alpha"), 0, 255);
	crosshair_argb = video::SColor(cross_a, cross_r, cross_g, cross_b);

	v3f selectionbox_color = g_settings->getV3F("selectionbox_color").value_or(v3f());
	u32 sbox_r = rangelim(myround(selectionbox_color.X), 0, 255);
	u32 sbox_g = rangelim(myround(selectionbox_color.Y), 0, 255);
	u32 sbox_b = rangelim(myround(selectionbox_color.Z), 0, 255);
	selectionbox_argb = video::SColor(255, sbox_r, sbox_g, sbox_b);

	use_crosshair_image = tsrc->isKnownSourceImage("crosshair.png");
	use_object_crosshair_image = tsrc->isKnownSourceImage("object_crosshair.png");

	m_selection_boxes.clear();
	m_halo_boxes.clear();

	std::string mode_setting = g_settings->get("node_highlighting");

	if (mode_setting == "halo") {
		m_mode = HIGHLIGHT_HALO;
	} else if (mode_setting == "none") {
		m_mode = HIGHLIGHT_NONE;
	} else {
		m_mode = HIGHLIGHT_BOX;
	}

	// Initialize m_selection_material
	IShaderSource *shdrsrc = client->getShaderSource();
	if (m_mode == HIGHLIGHT_HALO) {
		auto shader_id = shdrsrc->getShaderRaw("selection_shader", true);
		m_selection_material.MaterialType = shdrsrc->getShaderInfo(shader_id).material;
	} else {
		m_selection_material.MaterialType = video::EMT_SOLID;
	}

	if (m_mode == HIGHLIGHT_BOX) {
		m_selection_material.Thickness =
			rangelim(g_settings->getS16("selectionbox_width"), 1, 5);
	} else if (m_mode == HIGHLIGHT_HALO) {
		m_selection_material.setTexture(0, tsrc->getTextureForMesh("halo.png"));
		m_selection_material.BackfaceCulling = true;
	} else {
		m_selection_material.MaterialType = video::EMT_SOLID;
	}

	// Initialize m_block_bounds_material
	m_block_bounds_material.MaterialType = video::EMT_SOLID;
	m_block_bounds_material.Thickness =
			rangelim(g_settings->getS16("selectionbox_width"), 1, 5);

	// Prepare mesh for compass drawing
	m_rotation_mesh_buffer.reset(new scene::SMeshBuffer());
	auto *b = m_rotation_mesh_buffer.get();
	auto &vertices = b->Vertices->Data;
	auto &indices = b->Indices->Data;
	vertices.resize(4);
	indices.resize(6);

	video::SColor white(255, 255, 255, 255);
	v3f normal(0.f, 0.f, 1.f);

	vertices[0] = video::S3DVertex(v3f(-1.f, -1.f, 0.f), normal, white, v2f(0.f, 1.f));
	vertices[1] = video::S3DVertex(v3f(-1.f,  1.f, 0.f), normal, white, v2f(0.f, 0.f));
	vertices[2] = video::S3DVertex(v3f( 1.f,  1.f, 0.f), normal, white, v2f(1.f, 0.f));
	vertices[3] = video::S3DVertex(v3f( 1.f, -1.f, 0.f), normal, white, v2f(1.f, 1.f));

	indices[0] = 0;
	indices[1] = 1;
	indices[2] = 2;
	indices[3] = 2;
	indices[4] = 3;
	indices[5] = 0;

	b->getMaterial().MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL;
	b->setHardwareMappingHint(scene::EHM_STATIC);
}

void Hud::readScalingSetting()
{
	m_hud_scaling      = g_settings->getFloat("hud_scaling", 0.5f, 20.0f);
	m_scale_factor     = m_hud_scaling * RenderingEngine::getDisplayDensity();
	m_hotbar_imagesize = std::floor(HOTBAR_IMAGE_SIZE *
		RenderingEngine::getDisplayDensity() + 0.5f);
	m_hotbar_imagesize *= m_hud_scaling;
	m_padding = m_hotbar_imagesize / 12;
}

Hud::~Hud()
{
	g_settings->deregisterAllChangedCallbacks(this);

	if (m_selection_mesh)
		m_selection_mesh->drop();

	if (m_music_thumbnail_texture)
		driver->removeTexture(m_music_thumbnail_texture);
}

void Hud::drawItem(const ItemStack &item, const core::rect<s32>& rect,
		bool selected, bool draw_slot_bg)
{
	if (selected) {
		/* draw highlighting around selected item */
		if (use_hotbar_selected_image) {
			core::rect<s32> imgrect2 = rect;
			imgrect2.UpperLeftCorner.X  -= (m_padding*2);
			imgrect2.UpperLeftCorner.Y  -= (m_padding*2);
			imgrect2.LowerRightCorner.X += (m_padding*2);
			imgrect2.LowerRightCorner.Y += (m_padding*2);
				video::ITexture *texture = tsrc->getTexture(hotbar_selected_image);
				core::dimension2di imgsize(texture->getOriginalSize());
			draw2DImageFilterScaled(driver, texture, imgrect2,
					core::rect<s32>(core::position2d<s32>(0,0), imgsize),
					NULL, hbar_colors, true);
		} else {
			video::SColor c_outside(255,255,0,0);
			//video::SColor c_outside(255,0,0,0);
			//video::SColor c_inside(255,192,192,192);
			s32 x1 = rect.UpperLeftCorner.X;
			s32 y1 = rect.UpperLeftCorner.Y;
			s32 x2 = rect.LowerRightCorner.X;
			s32 y2 = rect.LowerRightCorner.Y;
			// Black base borders
			driver->draw2DRectangle(c_outside,
				core::rect<s32>(
				v2s32(x1 - m_padding, y1 - m_padding),
				v2s32(x2 + m_padding, y1)
				), NULL);
			driver->draw2DRectangle(c_outside,
				core::rect<s32>(
				v2s32(x1 - m_padding, y2),
				v2s32(x2 + m_padding, y2 + m_padding)
				), NULL);
			driver->draw2DRectangle(c_outside,
				core::rect<s32>(
				v2s32(x1 - m_padding, y1),
					v2s32(x1, y2)
				), NULL);
			driver->draw2DRectangle(c_outside,
				core::rect<s32>(
					v2s32(x2, y1),
				v2s32(x2 + m_padding, y2)
				), NULL);
			/*// Light inside borders
			driver->draw2DRectangle(c_inside,
				core::rect<s32>(
					v2s32(x1 - padding/2, y1 - padding/2),
					v2s32(x2 + padding/2, y1)
				), NULL);
			driver->draw2DRectangle(c_inside,
				core::rect<s32>(
					v2s32(x1 - padding/2, y2),
					v2s32(x2 + padding/2, y2 + padding/2)
				), NULL);
			driver->draw2DRectangle(c_inside,
				core::rect<s32>(
					v2s32(x1 - padding/2, y1),
					v2s32(x1, y2)
				), NULL);
			driver->draw2DRectangle(c_inside,
				core::rect<s32>(
					v2s32(x2, y1),
					v2s32(x2 + padding/2, y2)
				), NULL);
			*/
		}
	}

	video::SColor bgcolor2(128, 0, 0, 0);
	if (draw_slot_bg && !use_hotbar_image)
		driver->draw2DRectangle(bgcolor2, rect, NULL);
	drawItemStack(driver, g_fontengine->getFont(), item, rect, NULL,
		client, selected ? IT_ROT_SELECTED : IT_ROT_NONE);
}

// NOTE: selectitem = 0 -> no selected; selectitem is 1-based
// mainlist can be NULL, but draw the frame anyway.
void Hud::drawItems(v2s32 screen_pos, v2s32 screen_offset, s32 itemcount, v2f alignment,
		s32 inv_offset, InventoryList *mainlist, u16 selectitem, u16 direction,
		bool is_hotbar)
{
	s32 height  = m_hotbar_imagesize + m_padding * 2;
	s32 width   = (itemcount - inv_offset) * (m_hotbar_imagesize + m_padding * 2);

	if (direction == HUD_DIR_TOP_BOTTOM || direction == HUD_DIR_BOTTOM_TOP) {
		s32 tmp = height;
		height = width;
		width = tmp;
	}

	// Position: screen_pos + screen_offset + alignment
	v2s32 pos(screen_offset.X * m_scale_factor, screen_offset.Y * m_scale_factor);
	pos += screen_pos;
	pos.X += (alignment.X - 1.0f) * (width * 0.5f);
	pos.Y += (alignment.Y - 1.0f) * (height * 0.5f);

	// Store hotbar_image in member variable, used by drawItem()
	if (hotbar_image != player->hotbar_image) {
		hotbar_image = player->hotbar_image;
		use_hotbar_image = !hotbar_image.empty();
	}

	// Store hotbar_selected_image in member variable, used by drawItem()
	if (hotbar_selected_image != player->hotbar_selected_image) {
		hotbar_selected_image = player->hotbar_selected_image;
		use_hotbar_selected_image = !hotbar_selected_image.empty();
	}

	// draw customized item background
	if (use_hotbar_image) {
		core::rect<s32> imgrect2(-m_padding/2, -m_padding/2,
			width+m_padding/2, height+m_padding/2);
		core::rect<s32> rect2 = imgrect2 + pos;
		video::ITexture *texture = tsrc->getTexture(hotbar_image);
		core::dimension2di imgsize(texture->getOriginalSize());
		draw2DImageFilterScaled(driver, texture, rect2,
			core::rect<s32>(core::position2d<s32>(0,0), imgsize),
			NULL, hbar_colors, true);
	}

	// Draw items
	core::rect<s32> imgrect(0, 0, m_hotbar_imagesize, m_hotbar_imagesize);
	const s32 list_max = std::min(itemcount, (s32) (mainlist ? mainlist->getSize() : 0 ));
	for (s32 i = inv_offset; i < list_max; i++) {
		s32 fullimglen = m_hotbar_imagesize + m_padding * 2;

		v2s32 steppos;
		switch (direction) {
		case HUD_DIR_RIGHT_LEFT:
			steppos = v2s32(m_padding + (list_max - 1 - i - inv_offset) * fullimglen, m_padding);
			break;
		case HUD_DIR_TOP_BOTTOM:
			steppos = v2s32(m_padding, m_padding + (i - inv_offset) * fullimglen);
			break;
		case HUD_DIR_BOTTOM_TOP:
			steppos = v2s32(m_padding, m_padding + (list_max - 1 - i - inv_offset) * fullimglen);
			break;
		default:
			steppos = v2s32(m_padding + (i - inv_offset) * fullimglen, m_padding);
			break;
		}

		core::rect<s32> item_rect = imgrect + pos + steppos;

		drawItem(mainlist->getItem(i), item_rect, (i + 1) == selectitem);

		if (is_hotbar && g_touchcontrols)
			g_touchcontrols->registerHotbarRect(i, item_rect);
	}
}

bool Hud::hasElementOfType(HudElementType type)
{
	for (size_t i = 0; i != player->maxHudId(); i++) {
		HudElement *e = player->getHud(i);
		if (!e)
			continue;
		if (e->type == type)
			return true;
	}
	return false;
}

// Calculates screen position of waypoint. Returns true if waypoint is visible (in front of the player), else false.
bool Hud::calculateScreenPos(const v3s16 &camera_offset, HudElement *e, v2s32 *pos)
{
	v3f w_pos = e->world_pos * BS;
	scene::ICameraSceneNode* camera =
		client->getSceneManager()->getActiveCamera();
	w_pos -= intToFloat(camera_offset, BS);
	core::matrix4 trans = camera->getProjectionMatrix();
	trans *= camera->getViewMatrix();
	f32 transformed_pos[4] = { w_pos.X, w_pos.Y, w_pos.Z, 1.0f };
	trans.multiplyWith1x4Matrix(transformed_pos);
	if (transformed_pos[3] < 0)
		return false;
	f32 zDiv = transformed_pos[3] == 0.0f ? 1.0f :
		core::reciprocal(transformed_pos[3]);
	pos->X = m_screensize.X * (0.5 * transformed_pos[0] * zDiv + 0.5);
	pos->Y = m_screensize.Y * (0.5 - transformed_pos[1] * zDiv * 0.5);
	return true;
}

void Hud::drawLuaElements(const v3s16 &camera_offset)
{
	const u32 text_height = g_fontengine->getTextHeight();
	gui::IGUIFont *const font = g_fontengine->getFont();

	// Reorder elements by z_index
	std::vector<HudElement*> elems;
	elems.reserve(player->maxHudId());

	// Add builtin elements if the server doesn't send them.
	// Declared here such that they have the same lifetime as the elems vector
	HudElement minimap;
	HudElement hotbar;
	if (client->getProtoVersion() < 44 && (player->hud_flags & HUD_FLAG_MINIMAP_VISIBLE)) {
		minimap = {HUD_ELEM_MINIMAP, v2f(1, 0), "", v2f(), "", 0 , 0, 0, v2f(-1, 1),
				v2f(-10, 10), v3f(), v2s32(256, 256), 0, "", 0};
		elems.push_back(&minimap);
	}
	if (client->getProtoVersion() < 46 && player->hud_flags & HUD_FLAG_HOTBAR_VISIBLE) {
		hotbar = {HUD_ELEM_HOTBAR, v2f(0.5, 1), "", v2f(), "", 0 , 0, 0, v2f(0, -1),
				v2f(0, -4), v3f(), v2s32(), 0, "", 0};
		elems.push_back(&hotbar);
	}

	for (size_t i = 0; i != player->maxHudId(); i++) {
		HudElement *e = player->getHud(i);
		if (!e)
			continue;

		auto it = elems.begin();
		while (it != elems.end() && (*it)->z_index <= e->z_index)
			++it;

		elems.insert(it, e);
	}

	for (HudElement *e : elems) {

		v2s32 pos(floor(e->pos.X * (float) m_screensize.X + 0.5),
				floor(e->pos.Y * (float) m_screensize.Y + 0.5));
		switch (e->type) {
			case HUD_ELEM_TEXT: {
				unsigned int font_size = g_fontengine->getDefaultFontSize();

				if (e->size.X > 0)
					font_size *= e->size.X;

#ifdef __ANDROID__
				// The text size on Android is not proportional with the actual scaling
				// FIXME: why do we have such a weird unportable hack??
				if (font_size > 3 && e->offset.X < -20)
					font_size -= 3;
#endif
				auto textfont = g_fontengine->getFont(FontSpec(font_size,
					(e->style & HUD_STYLE_MONO) ? FM_Mono : FM_Unspecified,
					e->style & HUD_STYLE_BOLD, e->style & HUD_STYLE_ITALIC));

				irr::gui::CGUITTFont *ttfont = nullptr;
				if (textfont->getType() == irr::gui::EGFT_CUSTOM)
					ttfont = static_cast<irr::gui::CGUITTFont *>(textfont);

				video::SColor color(255, (e->number >> 16) & 0xFF,
										 (e->number >> 8)  & 0xFF,
										 (e->number >> 0)  & 0xFF);
				EnrichedString text(unescape_string(utf8_to_wide(e->text)), color);
				core::dimension2d<u32> textsize = textfont->getDimension(text.c_str());

				v2s32 offset(0, (e->align.Y - 1.0) * (textsize.Height / 2));
				core::rect<s32> size(0, 0, e->scale.X * m_scale_factor,
						text_height * e->scale.Y * m_scale_factor);
				v2s32 offs(e->offset.X * m_scale_factor,
						e->offset.Y * m_scale_factor);

				// Draw each line
				// See also: GUIFormSpecMenu::parseLabel
				size_t str_pos = 0;
				while (str_pos < text.size()) {
					EnrichedString line = text.getNextLine(&str_pos);

					core::dimension2d<u32> linesize = textfont->getDimension(line.c_str());
					v2s32 line_offset((e->align.X - 1.0) * (linesize.Width / 2), 0);
					if (ttfont)
						ttfont->draw(line, size + pos + offset + offs + line_offset);
					else
						textfont->draw(line.c_str(), size + pos + offset + offs + line_offset, color);
					offset.Y += linesize.Height;
				}
				break; }
			case HUD_ELEM_STATBAR: {
				v2s32 offs(e->offset.X, e->offset.Y);
				drawStatbar(pos, HUD_CORNER_UPPER, e->dir, e->text, e->text2,
					e->number, e->item, offs, e->size);
				break; }
			case HUD_ELEM_INVENTORY: {
				InventoryList *inv = inventory->getList(e->text);
				if (!inv)
					warningstream << "HUD: Unknown inventory list. name=" << e->text << std::endl;
				drawItems(pos, v2s32(e->offset.X, e->offset.Y), e->number, e->align, 0,
					inv, e->item, e->dir, false);
				break; }
			case HUD_ELEM_WAYPOINT: {
				if (!calculateScreenPos(camera_offset, e, &pos))
					break;

				pos += v2s32(e->offset.X, e->offset.Y);
				video::SColor color(255, (e->number >> 16) & 0xFF,
										 (e->number >> 8)  & 0xFF,
										 (e->number >> 0)  & 0xFF);
				std::wstring text = unescape_translate(utf8_to_wide(e->name));
				const std::string &unit = e->text;
				// Waypoints reuse the item field to store precision,
				// item = precision + 1 and item = 0 <=> precision = 10 for backwards compatibility.
				// Also see `push_hud_element`.
				u32 item = e->item;
				float precision = (item == 0) ? 10.0f : (item - 1.f);
				bool draw_precision = precision > 0;

				core::rect<s32> bounds(0, 0, font->getDimension(text.c_str()).Width, (draw_precision ? 2:1) * text_height);
				pos.Y += (e->align.Y - 1.0) * bounds.getHeight() / 2;
				bounds += pos;
				font->draw(text.c_str(), bounds + v2s32((e->align.X - 1.0) * bounds.getWidth() / 2, 0), color);
				if (draw_precision) {
					std::ostringstream os;
					v3f p_pos = player->getPosition() / BS;
					float distance = std::floor(precision * p_pos.getDistanceFrom(e->world_pos)) / precision;
					os << distance << unit;
					text = unescape_translate(utf8_to_wide(os.str()));
					bounds.LowerRightCorner.X = bounds.UpperLeftCorner.X + font->getDimension(text.c_str()).Width;
					font->draw(text.c_str(), bounds + v2s32((e->align.X - 1.0f) * bounds.getWidth() / 2, text_height), color);
				}
				break; }
			case HUD_ELEM_IMAGE_WAYPOINT: {
				if (!calculateScreenPos(camera_offset, e, &pos))
					break;
				[[fallthrough]];
			}
			case HUD_ELEM_IMAGE: {
				video::ITexture *texture = tsrc->getTexture(e->text);
				if (!texture)
					continue;

				const video::SColor color(255, 255, 255, 255);
				const video::SColor colors[] = {color, color, color, color};
				core::dimension2di imgsize(texture->getOriginalSize());
				v2s32 dstsize(imgsize.Width * e->scale.X * m_scale_factor,
				              imgsize.Height * e->scale.Y * m_scale_factor);
				if (e->scale.X < 0)
					dstsize.X = m_screensize.X * (e->scale.X * -0.01);
				if (e->scale.Y < 0)
					dstsize.Y = m_screensize.Y * (e->scale.Y * -0.01);
				v2s32 offset((e->align.X - 1.0) * dstsize.X / 2,
				             (e->align.Y - 1.0) * dstsize.Y / 2);
				core::rect<s32> rect(0, 0, dstsize.X, dstsize.Y);
				rect += pos + offset + v2s32(e->offset.X * m_scale_factor,
				                             e->offset.Y * m_scale_factor);
				draw2DImageFilterScaled(driver, texture, rect,
					core::rect<s32>(core::position2d<s32>(0,0), imgsize),
					NULL, colors, true);
				break; }
			case HUD_ELEM_COMPASS: {
				video::ITexture *texture = tsrc->getTexture(e->text);
				if (!texture)
					continue;

				// Positionning :
				v2s32 dstsize(e->size.X, e->size.Y);
				if (e->size.X < 0)
					dstsize.X = m_screensize.X * (e->size.X * -0.01);
				if (e->size.Y < 0)
					dstsize.Y = m_screensize.Y * (e->size.Y * -0.01);

				if (dstsize.X <= 0 || dstsize.Y <= 0)
					return; // Avoid zero divides

				// Angle according to camera view
				scene::ICameraSceneNode *cam = client->getSceneManager()->getActiveCamera();
				v3f fore = cam->getAbsoluteTransformation()
						.rotateAndScaleVect(v3f(0.f, 0.f, 1.f));
				int angle = - fore.getHorizontalAngle().Y;

				// Limit angle and ajust with given offset
				angle = (angle + (int)e->number) % 360;

				core::rect<s32> dstrect(0, 0, dstsize.X, dstsize.Y);
				dstrect += pos + v2s32(
								(e->align.X - 1.0) * dstsize.X / 2,
								(e->align.Y - 1.0) * dstsize.Y / 2) +
						v2s32(e->offset.X * m_hud_scaling, e->offset.Y * m_hud_scaling);

				switch (e->dir) {
				case HUD_COMPASS_ROTATE:
					drawCompassRotate(e, texture, dstrect, angle);
					break;
				case HUD_COMPASS_ROTATE_REVERSE:
					drawCompassRotate(e, texture, dstrect, -angle);
					break;
				case HUD_COMPASS_TRANSLATE:
					drawCompassTranslate(e, texture, dstrect, angle);
					break;
				case HUD_COMPASS_TRANSLATE_REVERSE:
					drawCompassTranslate(e, texture, dstrect, -angle);
					break;
				default:
					break;
				}
				break; }
			case HUD_ELEM_MINIMAP: {
				if (!client->getMinimap())
					break;
				// Draw a minimap of size "size"
				v2s32 dstsize(e->size.X * m_scale_factor,
				              e->size.Y * m_scale_factor);

				// Only one percentage is supported to avoid distortion.
				if (e->size.X < 0)
					dstsize.X = dstsize.Y = m_screensize.X * (e->size.X * -0.01);
				else if (e->size.Y < 0)
					dstsize.X = dstsize.Y = m_screensize.Y * (e->size.Y * -0.01);

				if (dstsize.X <= 0 || dstsize.Y <= 0)
					return;

				v2s32 offset((e->align.X - 1.0) * dstsize.X / 2,
				             (e->align.Y - 1.0) * dstsize.Y / 2);
				core::rect<s32> rect(0, 0, dstsize.X, dstsize.Y);
				rect += pos + offset + v2s32(e->offset.X * m_scale_factor,
				                             e->offset.Y * m_scale_factor);
				client->getMinimap()->drawMinimap(rect);
				break; }
			case HUD_ELEM_HOTBAR: {
				drawHotbar(pos, e->offset, e->dir, e->align);
				break; }
			default:
				infostream << "Hud::drawLuaElements: ignoring drawform " << e->type
					<< " due to unrecognized type" << std::endl;
		}
	}
}

void Hud::drawCompassTranslate(HudElement *e, video::ITexture *texture,
		const core::rect<s32> &rect, int angle)
{
	const video::SColor color(255, 255, 255, 255);
	const video::SColor colors[] = {color, color, color, color};

	// Compute source image scaling
	core::dimension2di imgsize(texture->getOriginalSize());
	core::rect<s32> srcrect(0, 0, imgsize.Width, imgsize.Height);

	v2s32 dstsize(rect.getHeight() * e->scale.X * imgsize.Width / imgsize.Height,
			rect.getHeight() * e->scale.Y);

	// Avoid infinite loop
	if (dstsize.X <= 0 || dstsize.Y <= 0)
		return;

	core::rect<s32> tgtrect(0, 0, dstsize.X, dstsize.Y);
	tgtrect +=  v2s32(
				(rect.getWidth() - dstsize.X) / 2,
				(rect.getHeight() - dstsize.Y) / 2) +
			rect.UpperLeftCorner;

	int offset = angle * dstsize.X / 360;

	tgtrect += v2s32(offset, 0);

	// Repeat image as much as needed
	while (tgtrect.UpperLeftCorner.X > rect.UpperLeftCorner.X)
		tgtrect -= v2s32(dstsize.X, 0);

	draw2DImageFilterScaled(driver, texture, tgtrect, srcrect, &rect, colors, true);
	tgtrect += v2s32(dstsize.X, 0);

	while (tgtrect.UpperLeftCorner.X < rect.LowerRightCorner.X) {
		draw2DImageFilterScaled(driver, texture, tgtrect, srcrect, &rect, colors, true);
		tgtrect += v2s32(dstsize.X, 0);
	}
}

void Hud::drawCompassRotate(HudElement *e, video::ITexture *texture,
		const core::rect<s32> &rect, int angle)
{
	core::rect<s32> oldViewPort = driver->getViewPort();
	core::matrix4 oldProjMat = driver->getTransform(video::ETS_PROJECTION);
	core::matrix4 oldViewMat = driver->getTransform(video::ETS_VIEW);

	core::matrix4 Matrix;
	Matrix.makeIdentity();
	Matrix.setRotationDegrees(v3f(0.f, 0.f, angle));

	driver->setViewPort(rect);
	driver->setTransform(video::ETS_PROJECTION, core::matrix4());
	driver->setTransform(video::ETS_VIEW, core::matrix4());
	driver->setTransform(video::ETS_WORLD, Matrix);

	auto &material = m_rotation_mesh_buffer->getMaterial();
	material.TextureLayers[0].Texture = texture;
	driver->setMaterial(material);
	driver->drawMeshBuffer(m_rotation_mesh_buffer.get());

	driver->setTransform(video::ETS_WORLD, core::matrix4());
	driver->setTransform(video::ETS_VIEW, oldViewMat);
	driver->setTransform(video::ETS_PROJECTION, oldProjMat);

	// restore the view area
	driver->setViewPort(oldViewPort);
}

void Hud::drawStatbar(v2s32 pos, u16 corner, u16 drawdir,
		const std::string &texture, const std::string &bgtexture,
		s32 count, s32 maxcount, v2s32 offset, v2s32 size)
{
	const video::SColor color(255, 255, 255, 255);
	const video::SColor colors[] = {color, color, color, color};

	video::ITexture *stat_texture = tsrc->getTexture(texture);
	if (!stat_texture)
		return;

	video::ITexture *stat_texture_bg = nullptr;
	if (!bgtexture.empty()) {
		stat_texture_bg = tsrc->getTexture(bgtexture);
	}

	core::dimension2di srcd(stat_texture->getOriginalSize());
	core::dimension2di dstd;
	if (size == v2s32()) {
		dstd = srcd;
		dstd.Height *= m_scale_factor;
		dstd.Width  *= m_scale_factor;
		offset.X *= m_scale_factor;
		offset.Y *= m_scale_factor;
	} else {
		dstd.Height = size.Y * m_scale_factor;
		dstd.Width  = size.X * m_scale_factor;
		offset.X *= m_scale_factor;
		offset.Y *= m_scale_factor;
	}

	v2s32 p = pos;
	if (corner & HUD_CORNER_LOWER)
		p -= dstd.Height;

	p += offset;

	v2s32 steppos;
	switch (drawdir) {
		case HUD_DIR_RIGHT_LEFT:
			steppos = v2s32(-1, 0);
			break;
		case HUD_DIR_TOP_BOTTOM:
			steppos = v2s32(0, 1);
			break;
		case HUD_DIR_BOTTOM_TOP:
			steppos = v2s32(0, -1);
			break;
		default:
			// From left to right
			steppos = v2s32(1, 0);
			break;
	}

	auto calculate_clipping_rect = [] (core::dimension2di src,
			v2s32 steppos) -> core::rect<s32> {

		// Create basic rectangle
		core::rect<s32> rect(0, 0,
			src.Width  - std::abs(steppos.X) * src.Width / 2,
			src.Height - std::abs(steppos.Y) * src.Height / 2
		);
		// Move rectangle left or down
		if (steppos.X == -1)
			rect += v2s32(src.Width / 2, 0);
		if (steppos.Y == -1)
			rect += v2s32(0, src.Height / 2);
		return rect;
	};
	// Rectangles for 1/2 the actual value to display
	core::rect<s32> srchalfrect, dsthalfrect;
	// Rectangles for 1/2 the "off state" texture
	core::rect<s32> srchalfrect2, dsthalfrect2;

	if (count % 2 == 1 || maxcount % 2 == 1) {
		// Need to draw halves: Calculate rectangles
		srchalfrect  = calculate_clipping_rect(srcd, steppos);
		dsthalfrect  = calculate_clipping_rect(dstd, steppos);
		srchalfrect2 = calculate_clipping_rect(srcd, steppos * -1);
		dsthalfrect2 = calculate_clipping_rect(dstd, steppos * -1);
	}

	steppos.X *= dstd.Width;
	steppos.Y *= dstd.Height;

	// Draw full textures
	for (s32 i = 0; i < count / 2; i++) {
		core::rect<s32> srcrect(0, 0, srcd.Width, srcd.Height);
		core::rect<s32> dstrect(0, 0, dstd.Width, dstd.Height);

		dstrect += p;
		draw2DImageFilterScaled(driver, stat_texture,
			dstrect, srcrect, NULL, colors, true);
		p += steppos;
	}

	if (count % 2 == 1) {
		// Draw half a texture
		draw2DImageFilterScaled(driver, stat_texture,
			dsthalfrect + p, srchalfrect, NULL, colors, true);

		if (stat_texture_bg && maxcount > count) {
			draw2DImageFilterScaled(driver, stat_texture_bg,
					dsthalfrect2 + p, srchalfrect2,
					NULL, colors, true);
			p += steppos;
		}
	}

	if (stat_texture_bg && maxcount > count) {
		// Draw "off state" textures
		s32 start_offset;
		if (count % 2 == 1)
			start_offset = count / 2 + 1;
		else
			start_offset = count / 2;
		for (s32 i = start_offset; i < maxcount / 2; i++) {
			core::rect<s32> srcrect(0, 0, srcd.Width, srcd.Height);
			core::rect<s32> dstrect(0, 0, dstd.Width, dstd.Height);

			dstrect += p;
			draw2DImageFilterScaled(driver, stat_texture_bg,
					dstrect, srcrect,
					NULL, colors, true);
			p += steppos;
		}

		if (maxcount % 2 == 1) {
			draw2DImageFilterScaled(driver, stat_texture_bg,
				dsthalfrect + p, srchalfrect, NULL, colors, true);
		}
	}
}
void Hud::drawHotbar(const v2s32 &pos, const v2f &offset, u16 dir, const v2f &align)
{
	if (g_touchcontrols)
		g_touchcontrols->resetHotbarRects();

	InventoryList *mainlist = inventory->getList("main");
	if (mainlist == NULL) {
		// Silently ignore this. We may not be initialized completely.
		return;
	}

	u16 playeritem = player->getWieldIndex();
	v2s32 screen_offset(offset.X, offset.Y);

	s32 hotbar_itemcount = player->getMaxHotbarItemcount();
	s32 width = hotbar_itemcount * (m_hotbar_imagesize + m_padding * 2);

	const v2u32 &window_size = RenderingEngine::getWindowSize();
	if ((float) width / (float) window_size.X <=
			g_settings->getFloat("hud_hotbar_max_width")) {
		drawItems(pos, screen_offset, hotbar_itemcount, align, 0,
			mainlist, playeritem + 1, dir, true);
	} else {
		v2s32 upper_pos = pos - v2s32(0, m_hotbar_imagesize + m_padding);

		drawItems(upper_pos, screen_offset, hotbar_itemcount / 2, align, 0,
			mainlist, playeritem + 1, dir, true);
		drawItems(pos, screen_offset, hotbar_itemcount, align,
			hotbar_itemcount / 2, mainlist, playeritem + 1, dir, true);
	}
}


void Hud::drawCrosshair()
{
	auto draw_image_crosshair = [this] (video::ITexture *tex) {
		core::dimension2di orig_size(tex->getOriginalSize());
		// Integer scaling to avoid artifacts, floor instead of round since too
		// small looks better than too large in this case.
		core::dimension2di scaled_size = orig_size * std::max(std::floor(m_scale_factor), 1.0f);

		core::rect<s32> src_rect(orig_size);
		core::position2d pos(m_displaycenter.X - scaled_size.Width / 2,
				m_displaycenter.Y - scaled_size.Height / 2);
		core::rect<s32> dest_rect(pos, scaled_size);

		video::SColor colors[] = { crosshair_argb, crosshair_argb,
				crosshair_argb, crosshair_argb };

		draw2DImageFilterScaled(driver, tex, dest_rect, src_rect,
				nullptr, colors, true);
	};

	if (pointing_at_object) {
		if (use_object_crosshair_image) {
			draw_image_crosshair(tsrc->getTexture("object_crosshair.png"));
		} else {
			s32 line_size = core::round32(OBJECT_CROSSHAIR_LINE_SIZE * m_scale_factor);

			driver->draw2DLine(
					m_displaycenter - v2s32(line_size, line_size),
					m_displaycenter + v2s32(line_size, line_size),
					crosshair_argb);
			driver->draw2DLine(
					m_displaycenter + v2s32(line_size, -line_size),
					m_displaycenter + v2s32(-line_size, line_size),
					crosshair_argb);
		}

		return;
	}

	if (use_crosshair_image) {
		draw_image_crosshair(tsrc->getTexture("crosshair.png"));
	} else {
		s32 line_size = core::round32(CROSSHAIR_LINE_SIZE * m_scale_factor);

		driver->draw2DLine(m_displaycenter - v2s32(line_size, 0),
				m_displaycenter + v2s32(line_size, 0), crosshair_argb);
		driver->draw2DLine(m_displaycenter - v2s32(0, line_size),
				m_displaycenter + v2s32(0, line_size), crosshair_argb);
	}
}

void Hud::setSelectionPos(const v3f &pos, const v3s16 &camera_offset)
{
	m_camera_offset = camera_offset;
	m_selection_pos = pos;
	m_selection_pos_with_offset = pos - intToFloat(camera_offset, BS);
}

void Hud::drawSelectionMesh()
{
	if (m_mode == HIGHLIGHT_NONE || (m_mode == HIGHLIGHT_HALO && !m_selection_mesh))
		return;
	driver->setMaterial(m_selection_material);
	const core::matrix4 oldtransform = driver->getTransform(video::ETS_WORLD);

	core::matrix4 translate;
	translate.setTranslation(m_selection_pos_with_offset);
	core::matrix4 rotation;
	rotation.setRotationDegrees(m_selection_rotation);
	driver->setTransform(video::ETS_WORLD, translate * rotation);

	if (m_mode == HIGHLIGHT_BOX) {
		// Draw 3D selection boxes
		for (auto & selection_box : m_selection_boxes) {
			if(g_settings->getBool("node_illumination")){
				v3f color = g_settings->getV3F("node_color").value_or(v3f());
				u32 r = rangelim(myround(color.X), 0, 255);
				u32 g = rangelim(myround(color.Y), 0, 255);
				u32 b = rangelim(myround(color.Z), 0, 255);
				driver->draw3DBox(selection_box, video::SColor(255, r, g, b));
			} else {
				u32 r = (selectionbox_argb.getRed() *
						m_selection_mesh_color.getRed() / 255);
				u32 g = (selectionbox_argb.getGreen() *
						m_selection_mesh_color.getGreen() / 255);
				u32 b = (selectionbox_argb.getBlue() *
						m_selection_mesh_color.getBlue() / 255);
			driver->draw3DBox(selection_box, video::SColor(255, r, g, b));
			}
		}
	} else if (m_mode == HIGHLIGHT_HALO && m_selection_mesh) {
		// Draw selection mesh
		setMeshColor(m_selection_mesh, m_selection_mesh_color);
		video::SColor face_color(0,
			MYMIN(255, m_selection_mesh_color.getRed() * 1.5),
			MYMIN(255, m_selection_mesh_color.getGreen() * 1.5),
			MYMIN(255, m_selection_mesh_color.getBlue() * 1.5));
		setMeshColorByNormal(m_selection_mesh, m_selected_face_normal,
			face_color);
		u32 mc = m_selection_mesh->getMeshBufferCount();
		for (u32 i = 0; i < mc; i++) {
			scene::IMeshBuffer *buf = m_selection_mesh->getMeshBuffer(i);
			driver->drawMeshBuffer(buf);
		}
	}
	driver->setTransform(video::ETS_WORLD, oldtransform);
}

namespace {
	std::string formatMinSec(int total_seconds)
	{
		if (total_seconds < 0)
			total_seconds = 0;
		int m = total_seconds / 60;
		int s = total_seconds % 60;
		char buf[16];
		porting::mt_snprintf(buf, sizeof(buf), "%d:%02d", m, s);
		return buf;
	}

	// Draws a line of MusicHud text clipped to `rect`. If the text is
	// wider than the rect, it scrolls left in a continuous, seamless loop
	// instead of letting the HUD box grow to fit it -- this is what keeps
	// the box a fixed size regardless of how long the track/artist name
	// is.
	void drawMarqueeLine(video::IVideoDriver *driver, gui::IGUIFont *font,
			const std::wstring &wtext, const core::rect<s32> &rect,
			video::SColor color)
	{
		if (wtext.empty())
			return;

		s32 text_w = font->getDimension(wtext.c_str()).Width;
		s32 avail_w = rect.getWidth();

		if (text_w <= avail_w) {
			font->draw(wtext.c_str(), rect, color, false, true, &rect);
			return;
		}

		// Two copies of the text, `cycle` pixels apart, both scrolling
		// left together: as soon as the first copy has fully scrolled off
		// the left edge, the second is already lined up to take its
		// place, so the loop has no visible seam or pause. Integer math
		// (not a float timer) avoids any long-session precision drift.
		constexpr s32 gap_px = 40;
		constexpr unsigned long long px_per_sec = 30;
		s32 cycle = text_w + gap_px;
		unsigned long long now_ms = porting::getTimeMs();
		s32 offset = (s32)((now_ms * px_per_sec / 1000ULL) % (unsigned long long)cycle);

		core::rect<s32> r1(rect.UpperLeftCorner.X - offset, rect.UpperLeftCorner.Y,
			rect.UpperLeftCorner.X - offset + text_w, rect.LowerRightCorner.Y);
		core::rect<s32> r2(r1.UpperLeftCorner.X + cycle, r1.UpperLeftCorner.Y,
			r1.LowerRightCorner.X + cycle, r1.LowerRightCorner.Y);

		font->draw(wtext.c_str(), r1, color, false, true, &rect);
		font->draw(wtext.c_str(), r2, color, false, true, &rect);
	}
}

void Hud::updateMusicThumbnail(const NowPlayingInfo &info)
{
	if (!info.has_thumbnail) {
		if (m_music_thumbnail_texture) {
			driver->removeTexture(m_music_thumbnail_texture);
			m_music_thumbnail_texture = nullptr;
		}
		m_music_thumbnail_id = 0;
		return;
	}

	if (m_music_thumbnail_texture && info.thumbnail_id == m_music_thumbnail_id)
		return; // already showing this exact artwork

	if (m_music_thumbnail_texture) {
		driver->removeTexture(m_music_thumbnail_texture);
		m_music_thumbnail_texture = nullptr;
	}

	auto *device = RenderingEngine::get_raw_device();
	io::IFileSystem *fs = device->getFileSystem();
	io::IReadFile *memfile = fs->createMemoryReadFile(
		info.thumbnail_data.data(), (s32)info.thumbnail_data.size(), "[music_hud_thumb_tmp");
	if (!memfile)
		return;

	video::IImage *img = driver->createImageFromFile(memfile);
	memfile->drop();
	if (!img)
		return;

	// Unique name per artwork so Irrlicht's texture cache never confuses
	// this frame's art with a previous track's.
	std::string texname = "music_hud_thumb_" + std::to_string(info.thumbnail_id);
	m_music_thumbnail_texture = driver->addTexture(texname.c_str(), img);
	img->drop();
	m_music_thumbnail_id = info.thumbnail_id;
}

void Hud::drawMusicHud()
{
	if (!g_settings->getBool("music_hud"))
		return;

	const NowPlayingInfo &info = m_now_playing.poll();
	if (!info.active || info.title.empty()) {
		updateMusicThumbnail(NowPlayingInfo()); // drop any cached art
		return;
	}

	updateMusicThumbnail(info);

	// Global size multiplier for MineBoost's custom HUD elements, combined
	// with this HUD's own independent multiplier -- see "hud_size" and
	// "music_hud_size" in src/gui/custom_menu/Menu.cpp ("HUD Size" slider
	// and scroll-to-resize in "Move HUD" edit mode, respectively).
	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("music_hud_size"), 0.5f, 2.5f);
	unsigned int scaled_font_size = (unsigned int)(g_fontengine->getDefaultFontSize() * hud_size);
	gui::IGUIFont *font = g_fontengine->getFont(scaled_font_size);
	if (!font)
		return;

	std::string line1 = info.source.empty() ? "" : ("[" + info.source + "]");
	std::string line2 = info.title;
	std::string line3 = info.artist;

	std::wstring wline1 = utf8_to_wide(line1);
	std::wstring wline2 = utf8_to_wide(line2);
	std::wstring wline3 = utf8_to_wide(line3);

	const s32 pad = (s32)(8 * hud_size);
	const s32 line_h = font->getDimension(L"Ay").Height;
	int num_text_lines = 1 + (wline1.empty() ? 0 : 1) + (wline3.empty() ? 0 : 1);

	const bool show_art = m_music_thumbnail_texture != nullptr;
	const s32 art_size = show_art ? std::max<s32>(num_text_lines * line_h, 32) : 0;
	const s32 art_gap = show_art ? pad : 0;

	const bool show_progress = info.has_progress && info.duration_seconds > 0;
	const s32 bar_h = 4;
	const s32 progress_block_h = show_progress ? (pad / 2 + bar_h + 2 + line_h) : 0;

	const s32 content_h = std::max(art_size, num_text_lines * line_h);
	// Text column has a fixed width (scaled to the current font size) so
	// the box never grows to fit long track/artist names -- long text
	// scrolls in place via drawMarqueeLine() instead. This keeps the HUD
	// a consistent, predictable size no matter what's playing.
	const s32 text_area_w = line_h * 9;
	const s32 box_w = art_size + art_gap + text_area_w + pad * 2;
	const s32 box_h = pad + content_h + progress_block_h + pad;

	// Position is user-draggable via Shift+E edit mode in the MineBoost GUI
	// (see src/gui/custom_menu/Menu.cpp, music_sprite). A saved X of -1
	// means "never moved yet" -> default to the top-right corner.
	s32 pos_x = g_settings->getS32("music_hud_x");
	s32 pos_y = g_settings->getS32("music_hud_y");
	if (pos_x < 0)
		pos_x = (s32)m_screensize.X - box_w - 10;

	core::rect<s32> box(
		pos_x, pos_y,
		pos_x + box_w, pos_y + box_h);

	drawHudColorPanel(driver, box, "hud_color_music");

	video::SColor source_color(255, 200, 200, 200);
	if (info.source == "Spotify")
		source_color = video::SColor(255, 30, 215, 96);
	else if (info.source == "YouTube Music")
		source_color = video::SColor(255, 255, 0, 0);
	else if (info.source == "SoundCloud")
		source_color = video::SColor(255, 255, 119, 0);
	else if (info.source == "Yandex Music")
		source_color = video::SColor(255, 255, 204, 0);

	const video::SColor artist_color(255, 200, 200, 200);

	s32 content_x = box.UpperLeftCorner.X + pad;

	if (show_art) {
		core::dimension2d<u32> tex_size = m_music_thumbnail_texture->getOriginalSize();
		core::rect<s32> art_rect(content_x, box.UpperLeftCorner.Y + pad,
			content_x + art_size, box.UpperLeftCorner.Y + pad + art_size);
		core::rect<s32> src_rect(0, 0, (s32)tex_size.Width, (s32)tex_size.Height);
		driver->draw2DImage(m_music_thumbnail_texture, art_rect, src_rect,
			nullptr, nullptr, true);
		content_x += art_size + art_gap;
	}

	s32 y = box.UpperLeftCorner.Y + pad;
	s32 text_right = box.LowerRightCorner.X - pad;

	if (!wline1.empty()) {
		core::rect<s32> rect1(content_x, y, text_right, y + line_h);
		drawMarqueeLine(driver, font, wline1, rect1, source_color);
		y += line_h;
	}

	core::rect<s32> rect2(content_x, y, text_right, y + line_h);
	drawMarqueeLine(driver, font, wline2, rect2, source_color);
	y += line_h;

	if (!wline3.empty()) {
		core::rect<s32> rect3(content_x, y, text_right, y + line_h);
		drawMarqueeLine(driver, font, wline3, rect3, artist_color);
	}

	if (show_progress) {
		s32 bar_y = box.UpperLeftCorner.Y + pad + content_h + pad / 2;
		s32 bar_x = box.UpperLeftCorner.X + pad;
		s32 bar_w = box_w - pad * 2;

		core::rect<s32> bar_bg(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
		ModernUI::roundedRectFilled(driver, bar_bg, bar_h / 2, video::SColor(200, 60, 60, 60));

		f32 ratio = rangelim(
			(f32)info.position_seconds / (f32)info.duration_seconds, 0.0f, 1.0f);
		core::rect<s32> bar_fill(bar_x, bar_y, bar_x + (s32)(bar_w * ratio), bar_y + bar_h);
		if (bar_fill.LowerRightCorner.X > bar_fill.UpperLeftCorner.X)
			ModernUI::roundedRectFilled(driver, bar_fill, bar_h / 2, source_color);

		s32 time_y = bar_y + bar_h + 2;
		std::wstring wpos = utf8_to_wide(formatMinSec(info.position_seconds));
		std::wstring wdur = utf8_to_wide(formatMinSec(info.duration_seconds));
		s32 dur_w = font->getDimension(wdur.c_str()).Width;

		core::rect<s32> pos_rect(bar_x, time_y, bar_x + bar_w / 2, time_y + line_h);
		font->draw(wpos.c_str(), pos_rect, video::SColor(255, 190, 190, 190), false, true);

		core::rect<s32> dur_rect(bar_x + bar_w - dur_w, time_y, bar_x + bar_w, time_y + line_h);
		font->draw(wdur.c_str(), dur_rect, video::SColor(255, 190, 190, 190), false, true);
	}
}

void Hud::drawPhotoHud()
{
	if (!g_settings->getBool("photo_hud"))
		return;

	// Only while some GUI (inventory/crafting/chest formspec, pause menu,
	// the MineBoost settings menu, etc.) is actually open -- this draws
	// before the GUI environment itself (see DrawHUD::run() in
	// src/client/render/plain.cpp), so it naturally ends up *behind*
	// whatever formspec is showing.
	if (!isMenuActive())
		return;

	// One of 5 fixed, built-in images (textures/base/pack/face.png,
	// cat_kuki.png, mellstroy.png, PawnWithBlackPeople.png,
	// PawnWithTwoBlackPeoples.png) rather than a player-chosen file path --
	// selected via "photo_hud_image" (see the Photo HUD picker panel in
	// src/gui/custom_menu/Menu.cpp), and loaded through the texture
	// source, like every other UI texture, so a texture pack can still
	// override it. Cheap to call every frame; the texture source does
	// its own caching.
	std::string image = g_settings->get("photo_hud_image");
	std::string texname = (image == "cat_kuki") ? "cat_kuki.png" :
		(image == "mellstroy") ? "mellstroy.png" :
		(image == "pawn_black") ? "PawnWithBlackPeople.png" :
		(image == "pawn_two_black") ? "PawnWithTwoBlackPeoples.png" : "face.png";
	video::ITexture *tex = tsrc->getTexture(texname);
	if (!tex)
		return;

	core::dimension2du imgsize = tex->getOriginalSize();
	if (imgsize.Width == 0 || imgsize.Height == 0)
		return;

	// Global size multiplier for MineBoost's custom HUD elements -- see
	// "hud_size" in src/gui/custom_menu/Menu.cpp ("HUD Size" slider).
	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f);

	// A small image next to/near the GUI, same draggable placement as
	// the other custom HUD elements.
	s32 max_dim = std::max<s32>(16, (s32)(g_settings->getS32("photo_hud_size") * hud_size));
	float scale = std::min(
		(float)max_dim / (float)imgsize.Width,
		(float)max_dim / (float)imgsize.Height);
	s32 draw_w = std::max<s32>(1, (s32)(imgsize.Width * scale));
	s32 draw_h = std::max<s32>(1, (s32)(imgsize.Height * scale));

	// Position is user-draggable via Shift+E edit mode in the MineBoost GUI
	// (see src/gui/custom_menu/Menu.cpp, photo_sprite). A saved X of -1
	// means "never moved yet" -> default to screen center.
	s32 pos_x = g_settings->getS32("photo_hud_x");
	s32 pos_y = g_settings->getS32("photo_hud_y");
	if (pos_x < 0)
		pos_x = ((s32)m_screensize.X - draw_w) / 2;
	if (pos_y < 0)
		pos_y = ((s32)m_screensize.Y - draw_h) / 2;

	core::rect<s32> dest(pos_x, pos_y, pos_x + draw_w, pos_y + draw_h);

	// Frame behind the photo -- only its outline is user-colorable via
	// "hud_color_photo" (see the "Colors" panel in
	// src/gui/custom_menu/Menu.cpp); drawHudColorPanel()'s fill is a fixed
	// shade, and the photo itself is drawn at its original colors below
	// (no tint) -- shadow off so it doesn't smear at the corners the way
	// ModernUI::dropShadow() used to on a box this small (see the comment
	// on drawHudColorPanel() near the top of this file). Padded a few px
	// outward so the frame is actually visible around an opaque photo.
	core::rect<s32> frame(dest.UpperLeftCorner.X - 6, dest.UpperLeftCorner.Y - 6,
		dest.LowerRightCorner.X + 6, dest.LowerRightCorner.Y + 6);
	drawHudColorPanel(driver, frame, "hud_color_photo");

	core::rect<s32> src(0, 0, imgsize.Width, imgsize.Height);
	driver->draw2DImage(tex, dest, src, nullptr, nullptr, true);
}

// Debug HUD backgrounds: draws the same fixed-size ModernUI panel behind
// the coords/FPS/ping debug text (set up in GameUI::update(), see
// src/client/gameui.cpp) that the "Move HUD" edit-mode preview shows for
// them -- see coords_sprite/fov_sprite/ping_sprite in
// src/gui/custom_menu/Menu.cpp for the matching preview boxes this mirrors
// (same 140x30 baseline size * hud_size * <element>_size, same position
// settings keys -- coords_sprite's box widens past that baseline to fit
// the actual coordinate text, matching this function's draw_box() below).
// Previously these three were bare IGUIStaticText labels with no backdrop
// at all in real gameplay, so they didn't match the bordered preview boxes
// shown while editing.
//
// Must run after GameUI::update() has positioned this frame's labels (it
// reads the same settings) but before the GUI environment draws the static
// text on top of it -- see DrawHUD::run() in src/client/render/plain.cpp.
void Hud::drawDebugTextBackgrounds()
{
	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f);
	const u32 text_h = g_fontengine->getTextHeight();

	auto draw_box = [&](const char *show_key, const char *pos_key,
			const char *size_key, s32 default_y_offset, const char *color_key,
			const wchar_t *dynamic_text = nullptr) {
		if (!g_settings->getBool(show_key))
			return;

		float size = hud_size * rangelim(g_settings->getFloat(size_key), 0.5f, 2.5f);
		s32 w = (s32)(140 * size);
		s32 h = (s32)(30 * size);

		// Coordinates vary a lot in digit count -- a couple of short
		// digits near spawn vs. a long negative number far out -- so
		// instead of always using the fixed 140px width above, measure
		// the actual text at this frame's font size (same font/size
		// GameUI::update() applies to m_guitext_coords, since "size" here
		// is computed the exact same way coords_size is there) and widen
		// the box to fit it, with some padding. Never shrinks below the
		// 140px baseline, so short coordinates still get a normal-looking
		// box instead of one clipped tight to 3-4 digits.
		if (dynamic_text) {
			gui::IGUIFont *font = g_fontengine->getFont(
				(unsigned int)(g_fontengine->getDefaultFontSize() * size), FM_Unspecified);
			if (font) {
				s32 text_w = (s32)font->getDimension(dynamic_text).Width;
				w = std::max(w, text_w + (s32)(24 * size));
			}
		}

		s32 x, y;
		if (g_settings->exists(pos_key)) {
			v2f data = g_settings->getV2F(pos_key);
			x = (s32)data.X;
			y = (s32)data.Y;
		} else {
			// Same fallback formula as GameUI::update() in gameui.cpp, so
			// the backdrop lines up with the real text before it's ever
			// been dragged.
			x = 5;
			y = (s32)m_screensize.Y - default_y_offset - (s32)text_h;
		}

		core::rect<s32> box(x, y, x + w, y + h);
		drawHudColorPanel(driver, box, color_key);
	};

	std::ostringstream os(std::ios_base::binary);
	os << std::setprecision(1) << std::fixed
		<< "(" << "X: " << (player->getPosition().X / BS)
		<< ", Y: " << (player->getPosition().Y / BS)
		<< ", Z: " << (player->getPosition().Z / BS) << ")";
	std::wstring coords_text = utf8_to_wide(os.str());

	draw_box("show_coords", "coords_sprite", "coords_size", 5, "hud_color_coords", coords_text.c_str());
	draw_box("show_fps", "fov_coords", "fps_size", 25, "hud_color_fps");
	draw_box("show_ping", "ping_coords", "ping_size", 45, "hud_color_ping");
}

// KeyStroker/ShowCPS background panels. These used to be a single baked
// image (textures/base/pack/keys_panel_bg.png / cps_panel_bg.png) drawn
// from builtin/client/keystroker.lua, tinted as a whole by
// "hud_color_keystroker_border"/"hud_color_cps_border" -- but the border in that PNG is
// baked-in pixel color (a fixed blue, confirmed by sampling it directly),
// so a multiply-tint could only ever darken/shift that exact blue, never
// actually recolor it to whatever the Colors panel says. Drawn here as a
// real ModernUI panel instead (fixed fill + a genuinely separate,
// genuinely colorable border, exactly like every other MineBoost HUD
// panel), matching the same size/position math the Lua side already
// derived: "keys_x"/"keys_y" ("cps_x"/"cps_y") are that panel's own
// top-left corner (see the comment on base_pos in
// update_hud_positions()/update_cps_hud_position() there), and its
// rendered size is native_texture_size * BG_BASE_SCALE(2) * hud_size --
// 160x160 for keys (80x80 native), 180x54 for cps (90x27 native). The
// individual key icons/CPS text themselves are unaffected -- still drawn
// by keystroker.lua as before, just with nothing behind them anymore.
void Hud::drawKeyStrokerCpsBackgrounds()
{
	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f);

	if (g_settings->getBool("show_keys")) {
		float size = hud_size * rangelim(g_settings->getFloat("keys_size"), 0.5f, 2.5f);
		s32 x = g_settings->getS32("keys_x");
		s32 y = g_settings->getS32("keys_y");
		s32 w = (s32)(160 * size);
		s32 h = (s32)(160 * size);
		core::rect<s32> box(x, y, x + w, y + h);
		drawHudColorPanel(driver, box, "hud_color_keystroker_border");
	}

	if (g_settings->getBool("show_cps")) {
		float size = hud_size * rangelim(g_settings->getFloat("cps_size"), 0.5f, 2.5f);
		s32 x = g_settings->getS32("cps_x");
		s32 y = g_settings->getS32("cps_y");
		s32 w = (s32)(180 * size);
		s32 h = (s32)(54 * size);
		core::rect<s32> box(x, y, x + w, y + h);
		drawHudColorPanel(driver, box, "hud_color_cps_border");
	}
}

void Hud::drawTargetHud()
{
	if (!target_hud_active || !g_settings->getBool("target_hud"))
		return;

	// Global size multiplier for MineBoost's custom HUD elements, combined
	// with this HUD's own independent multiplier -- see "hud_size" and
	// "target_hud_size" in src/gui/custom_menu/Menu.cpp ("HUD Size" slider
	// and scroll-to-resize in "Move HUD" edit mode, respectively).
	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("target_hud_size"), 0.5f, 2.5f);
	unsigned int scaled_font_size = (unsigned int)(g_fontengine->getDefaultFontSize() * hud_size);
	gui::IGUIFont *font = g_fontengine->getFont(scaled_font_size);
	if (!font)
		return;

	const s32 bar_w = (s32)(160 * hud_size);
	const s32 bar_h = (s32)(10 * hud_size);
	const s32 pad = (s32)(6 * hud_size);
	const s32 line_h = font->getDimension(L"Ay").Height;

	// Small avatar crop of the target's skin, shown to the left of the
	// name/HP column. Player skins use the same head-front UV layout as
	// Minecraft skins: an 8x8 square at (8,8) in a 64-px-wide texture
	// (scaled up proportionally for higher-resolution skins), plus a
	// second "hat/hair" overlay layer at (40,8), which we composite on
	// top so hats, hair and facial accessories actually show up.
	video::ITexture *avatar_texture = nullptr;
	core::rect<s32> avatar_src;
	core::rect<s32> avatar_overlay_src;
	bool avatar_has_overlay = false;
	s32 avatar_size = line_h + bar_h + pad; // roughly square, matches box content height
	if (!target_hud_skin.empty()) {
		avatar_texture = tsrc->getTexture(target_hud_skin);
		if (avatar_texture) {
			core::dimension2du sz = avatar_texture->getOriginalSize();
			f32 scale = sz.Width > 0 ? sz.Width / 64.0f : 1.0f;
			s32 fx = (s32)(8 * scale);
			s32 fy = (s32)(8 * scale);
			s32 fw = std::max<s32>(1, (s32)(8 * scale));
			s32 fh = std::max<s32>(1, (s32)(8 * scale));
			avatar_src = core::rect<s32>(fx, fy, fx + fw, fy + fh);

			// The hat/hair layer sits to the right of the base head on
			// the same row. Only use it if the texture is tall enough
			// to actually contain that row (old 32-px-tall legacy skins
			// without a second layer would otherwise sample garbage).
			s32 ox = (s32)(40 * scale);
			s32 oy = fy;
			if (ox + fw <= (s32)sz.Width && oy + fh <= (s32)sz.Height) {
				avatar_overlay_src = core::rect<s32>(ox, oy, ox + fw, oy + fh);
				avatar_has_overlay = true;
			}
		}
	}
	s32 avatar_col_w = avatar_texture ? avatar_size + pad : 0;

	std::wstring wname = utf8_to_wide(target_hud_name);
	s32 name_w = font->getDimension(wname.c_str()).Width;
	s32 text_col_w = std::max<s32>(bar_w, name_w);
	s32 box_w = avatar_col_w + text_col_w + pad * 2;
	s32 box_h = line_h + bar_h + pad * 3;

	s32 x = m_displaycenter.X - box_w / 2;
	s32 y = (s32)(m_screensize.Y * 0.16f); // default: a bit below the top, above the crosshair

	// Position is user-draggable via the "Move HUD" corner button in the
	// MineBoost GUI (see src/gui/custom_menu/Menu.cpp, target_hud_sprite).
	s32 saved_x = g_settings->getS32("target_hud_x");
	s32 saved_y = g_settings->getS32("target_hud_y");
	if (saved_x >= 0)
		x = saved_x;
	if (saved_y >= 0)
		y = saved_y;

	core::rect<s32> box(x, y, x + box_w, y + box_h);
	drawHudColorPanel(driver, box, "hud_color_target");

	if (avatar_texture) {
		core::rect<s32> avatar_dst(
			box.UpperLeftCorner.X + pad, box.UpperLeftCorner.Y + pad,
			box.UpperLeftCorner.X + pad + avatar_size, box.UpperLeftCorner.Y + pad + avatar_size);
		draw2DImageFilterScaled(driver, avatar_texture, avatar_dst, avatar_src);
		if (avatar_has_overlay) {
			draw2DImageFilterScaled(driver, avatar_texture, avatar_dst,
				avatar_overlay_src, nullptr, nullptr, true);
		}
	}

	s32 text_x = box.UpperLeftCorner.X + pad + avatar_col_w;
	core::rect<s32> name_rect(text_x, box.UpperLeftCorner.Y + pad,
		box.LowerRightCorner.X - pad, box.UpperLeftCorner.Y + pad + line_h);
	font->draw(wname.c_str(), name_rect, video::SColor(255, 255, 255, 255), true, true);

	s32 bar_x = text_x + (text_col_w - bar_w) / 2;
	s32 bar_y = box.UpperLeftCorner.Y + pad + line_h + pad;
	core::rect<s32> bar_bg(bar_x, bar_y, bar_x + bar_w, bar_y + bar_h);
	ModernUI::roundedRectFilled(driver, bar_bg, bar_h / 2, video::SColor(200, 40, 40, 40));

	f32 ratio = target_hud_hp_max > 0 ? (f32)target_hud_hp / (f32)target_hud_hp_max : 0.0f;
	ratio = rangelim(ratio, 0.0f, 1.0f);
	video::SColor hp_color = ratio > 0.5f ? video::SColor(255, 60, 200, 60) :
		ratio > 0.25f ? video::SColor(255, 230, 200, 40) : video::SColor(255, 220, 50, 50);
	core::rect<s32> bar_fill(bar_x, bar_y, bar_x + (s32)(bar_w * ratio), bar_y + bar_h);
	if (bar_fill.LowerRightCorner.X > bar_fill.UpperLeftCorner.X)
		ModernUI::roundedRectFilled(driver, bar_fill, bar_h / 2, hp_color);
}

void Hud::drawInventoryHud()
{
	if (!g_settings->getBool("inventory_hud"))
		return;

	InventoryList *mainlist = inventory->getList("main");
	if (!mainlist || mainlist->getSize() == 0) {
		// Log this once rather than every frame: with inventory_hud
		// enabled but nothing to draw, it's otherwise indistinguishable
		// from the feature being broken.
		static bool warned = false;
		if (!warned) {
			warned = true;
			warningstream << "InventoryHud: inventory_hud is enabled but the "
				"server isn't sending a \"main\" inventory list (or it's "
				"empty). Nothing to draw." << std::endl;

			const std::vector<InventoryList *> &lists = inventory->getLists();
			if (lists.empty()) {
				warningstream << "InventoryHud: this player has no inventory "
					"lists at all yet (maybe too early in the connection)." << std::endl;
			} else {
				warningstream << "InventoryHud: available inventory lists from this server: ";
				for (size_t i = 0; i < lists.size(); i++) {
					if (i > 0)
						warningstream << ", ";
					warningstream << "\"" << lists[i]->getName() << "\" (size "
						<< lists[i]->getSize() << ")";
				}
				warningstream << std::endl;
			}
		}
		return;
	}

	gui::IGUIFont *font = g_fontengine->getFont();
	if (!font) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			warningstream << "InventoryHud: inventory_hud is enabled and the "
				"\"main\" list has items, but g_fontengine->getFont() "
				"returned null -- can't draw without a font." << std::endl;
		}
		return;
	}

	// Reuse the hotbar's slot size/spacing so the grid matches the rest
	// of the HUD visually, regardless of hud_scaling -- then apply this
	// HUD's own independent size multiplier on top (see "inventory_hud_size"
	// in src/gui/custom_menu/Menu.cpp, adjustable by scrolling over this
	// HUD while in "Move HUD" edit mode).
	float inventory_hud_size = rangelim(g_settings->getFloat("inventory_hud_size"), 0.5f, 2.5f);
	const s32 slot = (s32)(m_hotbar_imagesize * inventory_hud_size);
	const s32 slot_pad = (s32)(m_padding * inventory_hud_size);
	const s32 pad = (s32)(8 * inventory_hud_size);
	gui::IGUIFont *scaled_font = g_fontengine->getFont(
		(unsigned int)(g_fontengine->getDefaultFontSize() * inventory_hud_size));
	if (scaled_font)
		font = scaled_font;
	const s32 title_h = font->getDimension(L"Ay").Height + pad;
	const s32 cell = slot + slot_pad * 2;

	// One section per inventory list being displayed: always "main"
	// ("Inventory"), plus whichever extra lists inventory_hud_extra_lists
	// names and this server actually sends non-empty (e.g. "armor" if you
	// want equipped armor folded in here instead of a separate HUD box).
	// "craft"/"craftpreview" are intentionally never shown here even if
	// listed -- the crafting grid has its own dedicated box, see
	// Hud::drawCraftHud() below.
	//
	// Titles are pre-converted to wide strings here (rather than every
	// frame in the render loop below) and the extra-list setting is only
	// re-parsed when its raw value actually changes -- this setting is
	// effectively static at runtime, so redoing str_split()/trim()/
	// utf8_to_wide() on it hundreds of times a second was pure waste,
	// especially noticeable on weak/low-end hardware.
	struct Section {
		std::wstring wtitle;
		InventoryList *list;
		s32 cols, rows;
	};
	std::vector<Section> sections;

	auto addSection = [&](const std::wstring &wtitle, InventoryList *list) {
		if (!list || list->getSize() == 0)
			return;
		s32 count = (s32)list->getSize();
		s32 cols = std::min<s32>(8, count);
		s32 rows = (count + cols - 1) / cols;
		sections.push_back({wtitle, list, cols, rows});
	};

	static const std::wstring inventory_wtitle = utf8_to_wide("Inventory");
	addSection(inventory_wtitle, mainlist);

	static std::string cached_extra_lists_raw;
	static std::vector<std::pair<std::string, std::wstring>> cached_extra_lists;
	std::string extra_lists_raw = g_settings->get("inventory_hud_extra_lists");
	if (extra_lists_raw != cached_extra_lists_raw) {
		cached_extra_lists_raw = extra_lists_raw;
		cached_extra_lists.clear();
		for (const std::string &raw_name : str_split(extra_lists_raw, ',')) {
			std::string name(trim(raw_name));
			if (name.empty() || name == "main" || name == "craft" || name == "craftpreview")
				continue; // "main" is already the first section above; craft
				          // lists belong to CraftHud instead
			std::string title = name;
			title[0] = std::toupper((unsigned char)title[0]);
			cached_extra_lists.emplace_back(name, utf8_to_wide(title));
		}
	}
	for (const auto &entry : cached_extra_lists)
		addSection(entry.second, inventory->getList(entry.first));

	if (sections.empty())
		return;

	// Box width follows whichever section is widest (in columns); every
	// section is left-aligned within that width rather than individually
	// centered, so the sections visually line up as one coherent panel.
	s32 max_cols = 0;
	s32 box_h = pad * 2;
	for (const Section &sec : sections) {
		max_cols = std::max(max_cols, sec.cols);
		box_h += title_h + sec.rows * cell;
	}
	const s32 box_w = max_cols * cell + pad * 2;

	s32 x = m_displaycenter.X - box_w / 2;
	s32 y = m_displaycenter.Y - box_h / 2;

	// Position is user-draggable via the "Move HUD" corner button in the
	// MineBoost GUI (see src/gui/custom_menu/Menu.cpp, inventory_hud_sprite).
	s32 saved_x = g_settings->getS32("inventory_hud_x");
	s32 saved_y = g_settings->getS32("inventory_hud_y");
	if (saved_x >= 0)
		x = saved_x;
	if (saved_y >= 0)
		y = saved_y;

	core::rect<s32> box(x, y, x + box_w, y + box_h);
	drawHudColorPanel(driver, box, "hud_color_inventory");

	s32 section_y = box.UpperLeftCorner.Y;
	for (const Section &sec : sections) {
		core::rect<s32> title_rect(box.UpperLeftCorner.X + pad, section_y + pad / 2,
			box.LowerRightCorner.X - pad, section_y + title_h);
		font->draw(sec.wtitle.c_str(), title_rect, video::SColor(255, 255, 255, 255), false, true);

		s32 count = (s32)sec.list->getSize();
		for (s32 i = 0; i < count; i++) {
			s32 col = i % sec.cols;
			s32 row = i / sec.cols;
			core::rect<s32> item_rect(0, 0, slot, slot);
			item_rect += core::vector2d<s32>(
				box.UpperLeftCorner.X + pad + slot_pad + col * cell,
				section_y + title_h + pad + slot_pad + row * cell);
			// Draw a distinct slot background + outline for every cell
			// (even empty ones) so the grid actually reads as a set of
			// slots instead of one undivided panel; drawItem() is told
			// not to draw its own flat background on top of this.
			ModernUI::panel(driver, item_rect, ModernUI::RadiusSmall, video::SColor(130, 40, 42, 52), video::SColor(150, 120, 150, 220), /*shadow=*/false);
			drawItem(sec.list->getItem(i), item_rect, false, false);
		}

		section_y += title_h + sec.rows * cell;
	}
}

void Hud::drawCraftHud()
{
	if (!g_settings->getBool("craft_hud"))
		return;

	InventoryList *craftlist = inventory->getList("craft");
	InventoryList *resultlist = inventory->getList("craftpreview");

	auto listHasAnyItem = [](InventoryList *list) {
		if (!list)
			return false;
		for (u32 i = 0; i < list->getSize(); i++) {
			if (!list->getItem(i).empty())
				return true;
		}
		return false;
	};

	// Only show the box while there's actually something in the craft
	// grid or a result to show -- this is meant to answer "what's in my
	// craft right now", not sit on screen as an empty panel at all times.
	if (!listHasAnyItem(craftlist) && !listHasAnyItem(resultlist))
		return;

	gui::IGUIFont *font = g_fontengine->getFont();
	if (!font)
		return;

	// See "craft_hud_size" in src/gui/custom_menu/Menu.cpp -- scroll over
	// this HUD in "Move HUD" edit mode to resize it independently of the
	// native hotbar/inventory scaling.
	float craft_hud_size = rangelim(g_settings->getFloat("craft_hud_size"), 0.5f, 2.5f);
	const s32 slot = (s32)(m_hotbar_imagesize * craft_hud_size);
	const s32 slot_pad = (s32)(m_padding * craft_hud_size);
	const s32 pad = (s32)(8 * craft_hud_size);
	gui::IGUIFont *scaled_font = g_fontengine->getFont(
		(unsigned int)(g_fontengine->getDefaultFontSize() * craft_hud_size));
	if (scaled_font)
		font = scaled_font;
	const s32 title_h = font->getDimension(L"Ay").Height + pad;
	const s32 cell = slot + slot_pad * 2;

	struct Section {
		std::wstring wtitle;
		InventoryList *list;
		s32 cols, rows;
	};
	std::vector<Section> sections;

	auto addSection = [&](const std::wstring &wtitle, InventoryList *list) {
		if (!list || list->getSize() == 0)
			return;
		s32 count = (s32)list->getSize();
		s32 cols = std::min<s32>(8, count);
		s32 rows = (count + cols - 1) / cols;
		sections.push_back({wtitle, list, cols, rows});
	};

	static const std::wstring craft_wtitle = utf8_to_wide("Craft");
	static const std::wstring result_wtitle = utf8_to_wide("Result");
	addSection(craft_wtitle, craftlist);
	addSection(result_wtitle, resultlist);

	if (sections.empty())
		return;

	s32 max_cols = 0;
	s32 box_h = pad * 2;
	for (const Section &sec : sections) {
		max_cols = std::max(max_cols, sec.cols);
		box_h += title_h + sec.rows * cell;
	}
	const s32 box_w = max_cols * cell + pad * 2;

	// Default position: to the right of screen center, so it doesn't
	// overlap InventoryHud's default (centered) position out of the box.
	s32 x = m_displaycenter.X + cell;
	s32 y = m_displaycenter.Y - box_h / 2;

	// Position is user-draggable via the "Move HUD" corner button in the
	// MineBoost GUI (see src/gui/custom_menu/Menu.cpp, craft_hud_sprite).
	s32 saved_x = g_settings->getS32("craft_hud_x");
	s32 saved_y = g_settings->getS32("craft_hud_y");
	if (saved_x >= 0)
		x = saved_x;
	if (saved_y >= 0)
		y = saved_y;

	core::rect<s32> box(x, y, x + box_w, y + box_h);
	drawHudColorPanel(driver, box, "hud_color_craft");

	s32 section_y = box.UpperLeftCorner.Y;
	for (const Section &sec : sections) {
		core::rect<s32> title_rect(box.UpperLeftCorner.X + pad, section_y + pad / 2,
			box.LowerRightCorner.X - pad, section_y + title_h);
		font->draw(sec.wtitle.c_str(), title_rect, video::SColor(255, 255, 255, 255), false, true);

		s32 count = (s32)sec.list->getSize();
		for (s32 i = 0; i < count; i++) {
			s32 col = i % sec.cols;
			s32 row = i / sec.cols;
			core::rect<s32> item_rect(0, 0, slot, slot);
			item_rect += core::vector2d<s32>(
				box.UpperLeftCorner.X + pad + slot_pad + col * cell,
				section_y + title_h + pad + slot_pad + row * cell);
			ModernUI::panel(driver, item_rect, ModernUI::RadiusSmall, video::SColor(130, 40, 42, 52), video::SColor(150, 120, 150, 220), /*shadow=*/false);
			drawItem(sec.list->getItem(i), item_rect, false, false);
		}

		section_y += title_h + sec.rows * cell;
	}
}

// ArmorHUD temporarily disabled -- entire implementation commented out
// below. Re-enable by removing the #if 0 / #endif wrapper (and the
// matching commented-out call in render/plain.cpp, declaration in
// hud.h, defaultsettings.cpp entries, and Menu.cpp/Menu.h GUI wiring).
#if 0
void Hud::drawArmorHud()
{
	if (!g_settings->getBool("armor_hud"))
		return;

	gui::IGUIFont *font = g_fontengine->getFont();
	if (!font)
		return;

	auto *idef = client->idef();

	// Which inventory list to treat as "equipped armor". "armor" is the
	// convention used by 3d_armor and Mineclonia/MineClone2's mcl_armor;
	// a few other names some forks/derivatives use are included too.
	// Add more via the comma-separated "armor_hud_lists" setting without
	// needing a rebuild.
	std::vector<std::string> armor_list_names;
	{
		std::string extra = g_settings->get("armor_hud_lists");
		for (std::string &name : str_split(extra, ',')) {
			name = trim(name);
			if (!name.empty())
				armor_list_names.push_back(name);
		}
	}
	armor_list_names.push_back("armor");
	armor_list_names.push_back("armor_inventory");
	armor_list_names.push_back("3d_armor_inventory");
	armor_list_names.push_back("gemarmor");
	armor_list_names.push_back("gemarmor_inventory");
	armor_list_names.push_back("gem_armor");
	armor_list_names.push_back("gem_armor_inventory");

	InventoryList *armorlist = nullptr;
	std::string used_list_name;
	for (const std::string &name : armor_list_names) {
		InventoryList *list = inventory->getList(name);
		if (list) {
			armorlist = list;
			used_list_name = name;
			break;
		}
	}

	// Rows to draw: icon + label + durability, one per piece. Mirrors the
	// layout used by Mineclonia-oriented clients (e.g. Lunarchy's
	// EquipmentHUD): a fixed Helmet/Chestplate/Leggings/Boots order, with
	// unequipped slots simply omitted rather than shown empty.
	struct ArmorRow {
		const ItemStack *item;
		std::wstring label;
	};
	std::vector<ArmorRow> rows;

	static const wchar_t *canonical_labels[4] = {
		L"Helmet", L"Chestplate", L"Leggings", L"Boots"
	};

	if (armorlist) {
		u32 size = armorlist->getSize();
		// Some armor mods (Mineclonia's mcl_armor among them) reserve
		// slot 0 for a "quick equip" item and start real armor at slot
		// 1, giving a list of size 5 for 4 pieces. If the list is
		// exactly that shape, skip slot 0; otherwise assume pieces
		// start at slot 0.
		u32 start = (size == 5) ? 1 : 0;
		for (u32 i = 0; i < 4 && start + i < size; i++) {
			const ItemStack &item = armorlist->getItem(start + i);
			if (!item.empty())
				rows.push_back({ &armorlist->getItem(start + i), canonical_labels[i] });
		}
	} else {
		// Fallback: this server has no dedicated equip-list under any
		// name we know (checked above). Some homegrown armor mods don't
		// use one at all -- armor just lives in "main" like any other
		// item, with no separate server-side signal for "worn" vs
		// "carried". Since there's nothing more reliable to go on,
		// match by name/description against a configurable keyword list
		// instead ("armor_hud_keywords", comma-separated). NOTE: without
		// a real equip-list, this can't distinguish armor sitting unused
		// in your inventory from armor actually worn.
		//
		// Important: only the FIRST item matching each keyword is kept
		// (e.g. one "helmet" row, not one per spare helmet you happen to
		// be carrying) -- otherwise every duplicate/spare piece in your
		// inventory would get its own row, which is unreadable clutter
		// rather than a useful HUD.
		std::vector<std::string> keywords;
		for (std::string &kw : str_split(g_settings->get("armor_hud_keywords"), ',')) {
			kw = lowercase(trim(kw));
			if (!kw.empty())
				keywords.push_back(kw);
		}

		if (!keywords.empty()) {
			InventoryList *mainlist = inventory->getList("main");
			if (mainlist) {
				std::vector<const ItemStack *> best_per_keyword(keywords.size(), nullptr);

				for (u32 j = 0; j < mainlist->getSize(); j++) {
					const ItemStack &item = mainlist->getItem(j);
					if (item.empty())
						continue;

					std::string haystack = lowercase(item.name);
					haystack += ' ';
					haystack += lowercase(item.getDefinition(idef).description);

					for (size_t k = 0; k < keywords.size(); k++) {
						if (!best_per_keyword[k] &&
								haystack.find(keywords[k]) != std::string::npos) {
							best_per_keyword[k] = &mainlist->getItem(j);
							break;
						}
					}
				}

				for (size_t k = 0; k < keywords.size(); k++) {
					if (!best_per_keyword[k])
						continue;
					const ItemStack &item = *best_per_keyword[k];
					std::wstring label = utf8_to_wide(item.getDefinition(idef).description);
					if (label.empty())
						label = utf8_to_wide(item.name);
					rows.push_back({ best_per_keyword[k], label });
				}
			}
		}
	}

	// Optional held-item row, drawn above the armor pieces (same idea as
	// Lunarchy's EquipmentHUD, which always includes the wielded item).
	ItemStack wielded, hand;
	if (g_settings->getBool("armor_hud_show_held") && player) {
		ItemStack &effective = player->getWieldedItem(&wielded, &hand);
		if (!effective.empty()) {
			// wielded/hand are locals, but getWieldedItem returns a
			// reference into one of them (or the player's inventory) --
			// copy it somewhere stable for the rest of this call.
			wielded = effective;
			rows.insert(rows.begin(), { &wielded, std::wstring(L"Held item") });
		}
	}

	if (rows.empty()) {
		// Log this once rather than every frame: with armor_hud enabled
		// but nothing to draw, it's otherwise indistinguishable from the
		// feature being broken.
		static bool warned = false;
		if (!warned) {
			warned = true;
			if (used_list_name.empty()) {
				warningstream << "ArmorHud: armor_hud is enabled, but none of "
					"the known equipped-armor inventory lists were found, and "
					"no items in \"main\" matched armor_hud_keywords." << std::endl;
			} else {
				warningstream << "ArmorHud: armor_hud is enabled, found list \""
					<< used_list_name << "\" but it's empty (no armor equipped)." << std::endl;
			}

			const std::vector<InventoryList *> &lists = inventory->getLists();
			if (lists.empty()) {
				warningstream << "ArmorHud: this player has no inventory lists "
					"at all yet (maybe too early in the connection)." << std::endl;
			} else {
				warningstream << "ArmorHud: available inventory lists from this server: ";
				for (size_t i = 0; i < lists.size(); i++) {
					if (i > 0)
						warningstream << ", ";
					warningstream << "\"" << lists[i]->getName() << "\" (size "
						<< lists[i]->getSize() << ")";
				}
				warningstream << std::endl;
			}

			InventoryList *mainlist = inventory->getList("main");
			if (mainlist) {
				warningstream << "ArmorHud: items currently in \"main\": ";
				bool first = true;
				for (u32 j = 0; j < mainlist->getSize(); j++) {
					const ItemStack &item = mainlist->getItem(j);
					if (item.empty())
						continue;
					if (!first)
						warningstream << ", ";
					first = false;
					warningstream << "\"" << item.name << "\" (\""
						<< item.getDefinition(idef).description << "\")";
				}
				if (first)
					warningstream << "(none)";
				warningstream << std::endl;
			}
		}
		return;
	}

	// --- Layout: a vertical list, icon on the left, name + durability
	// text to the right -- same arrangement as Lunarchy's EquipmentHUD.
	const s32 pad = 8;
	const s32 line_h = g_fontengine->getTextHeight();
	const s32 icon_size = std::max<s32>(16, std::min<s32>(line_h + 10, 28));
	const s32 row_gap = 3;
	const s32 row_h = std::max<s32>(icon_size, line_h);
	const s32 icon_text_gap = 6;

	s32 max_row_text_w = 0;
	std::vector<std::wstring> row_texts(rows.size());
	std::vector<video::SColor> row_bar_colors(rows.size());
	for (size_t i = 0; i < rows.size(); i++) {
		const ItemStack &item = *rows[i].item;

		std::wstring name = rows[i].label;
		if (!item.empty()) {
			std::string desc = item.getDefinition(idef).description;
			if (!desc.empty())
				name = utf8_to_wide(desc);
		}

		std::wstring durability_text;
		video::SColor bar_color(255, 60, 200, 60);
		if (!item.empty() && item.wear != 0) {
			f32 ratio = 1.0f - (f32)item.wear / 65535.0f;
			int pct = (int)std::round(ratio * 100.0f);
			wchar_t buf[16];
			swprintf(buf, sizeof(buf) / sizeof(wchar_t), L" (%d%%)", pct);
			durability_text = buf;

			if (ratio > 0.5f)
				bar_color = video::SColor(255, 60, 200, 60);
			else if (ratio > 0.25f)
				bar_color = video::SColor(255, 230, 160, 40);
			else
				bar_color = video::SColor(255, 220, 50, 50);
		}

		row_texts[i] = name + durability_text;
		row_bar_colors[i] = bar_color;

		core::dimension2d<u32> text_size = font->getDimension(row_texts[i].c_str());
		max_row_text_w = std::max<s32>(max_row_text_w, (s32)text_size.Width);
	}

	const s32 content_w = icon_size + icon_text_gap + max_row_text_w;
	const s32 content_h = (s32)rows.size() * row_h +
		std::max<s32>(0, (s32)rows.size() - 1) * row_gap;
	const s32 box_w = content_w + pad * 2;
	const s32 box_h = content_h + pad * 2;

	// Position is user-draggable via the "Move HUD" corner button in the
	// MineBoost GUI (see src/gui/custom_menu/Menu.cpp, armor_hud_sprite).
	s32 x = g_settings->getS32("armor_hud_x");
	s32 y = g_settings->getS32("armor_hud_y");
	if (x < 0)
		x = m_displaycenter.X - box_w / 2;
	if (y < 0)
		y = (s32)(m_screensize.Y * 0.6f);

	core::rect<s32> box(x, y, x + box_w, y + box_h);
	ModernUI::panel(driver, box, ModernUI::Radius, video::SColor(190, 22, 24, 30), ModernUI::PanelBorderDim, /*shadow=*/false);

	for (size_t i = 0; i < rows.size(); i++) {
		const ItemStack &item = *rows[i].item;
		s32 top = box.UpperLeftCorner.Y + pad + (s32)i * (row_h + row_gap);

		core::rect<s32> icon_rect(
			box.UpperLeftCorner.X + pad,
			top + (row_h - icon_size) / 2,
			box.UpperLeftCorner.X + pad + icon_size,
			top + (row_h - icon_size) / 2 + icon_size);
		if (!item.empty())
			drawItem(item, icon_rect, false);

		s32 text_x = icon_rect.LowerRightCorner.X + icon_text_gap;
		core::dimension2d<u32> text_size = font->getDimension(row_texts[i].c_str());
		s32 text_y = top + (row_h - (s32)text_size.Height) / 2;
		core::rect<s32> text_rect(text_x, text_y,
			text_x + (s32)text_size.Width, text_y + (s32)text_size.Height);
		font->draw(row_texts[i].c_str(), text_rect, video::SColor(255, 255, 255, 255), false, false);
	}
}
#endif // ArmorHUD disabled


enum Hud::BlockBoundsMode Hud::toggleBlockBounds()
{
	m_block_bounds_mode = static_cast<BlockBoundsMode>(m_block_bounds_mode + 1);

	if (m_block_bounds_mode > BLOCK_BOUNDS_NEAR) {
		m_block_bounds_mode = BLOCK_BOUNDS_OFF;
	}
	return m_block_bounds_mode;
}

void Hud::disableBlockBounds()
{
	m_block_bounds_mode = BLOCK_BOUNDS_OFF;
}

void Hud::drawBlockBounds()
{
	if (m_block_bounds_mode == BLOCK_BOUNDS_OFF) {
		return;
	}

	driver->setMaterial(m_block_bounds_material);

	u16 mesh_chunk_size = std::max<u16>(1, g_settings->getU16("client_mesh_chunk"));

	v3s16 block_pos = getContainerPos(player->getStandingNodePos(), MAP_BLOCKSIZE);

	v3f cam_offset = intToFloat(client->getCamera()->getOffset(), BS);

	v3f half_node = v3f(BS, BS, BS) / 2.0f;
	v3f base_corner = intToFloat(block_pos * MAP_BLOCKSIZE, BS) - cam_offset - half_node;

	s16 radius = m_block_bounds_mode == BLOCK_BOUNDS_NEAR ?
			rangelim(g_settings->getU16("show_block_bounds_radius_near"), 0, 1000) : 0;

	for (s16 x = -radius; x <= radius + 1; x++)
	for (s16 y = -radius; y <= radius + 1; y++) {
		// Red for mesh chunk edges, yellow for other block edges.
		auto choose_color = [&](s16 x_base, s16 y_base) {
			// See also MeshGrid::isMeshPos().
			// If the block is mesh pos, it means it's at the (-,-,-) corner of
			// the mesh. And we're drawing a (-,-) edge of this block. Hence,
			// it is an edge of the mesh grid.
			return (x + x_base) % mesh_chunk_size == 0
					&& (y + y_base) % mesh_chunk_size == 0 ?
				video::SColor(255, 255, 0, 0) :
				video::SColor(255, 255, 255, 0);
		};

		v3f pmin = v3f(x, y,    -radius) * MAP_BLOCKSIZE * BS;
		v3f pmax = v3f(x, y, 1 + radius) * MAP_BLOCKSIZE * BS;

		driver->draw3DLine(
			base_corner + v3f(pmin.X, pmin.Y, pmin.Z),
			base_corner + v3f(pmax.X, pmax.Y, pmax.Z),
			choose_color(block_pos.X, block_pos.Y)
		);
		driver->draw3DLine(
			base_corner + v3f(pmin.X, pmin.Z, pmin.Y),
			base_corner + v3f(pmax.X, pmax.Z, pmax.Y),
			choose_color(block_pos.X, block_pos.Z)
		);
		driver->draw3DLine(
			base_corner + v3f(pmin.Z, pmin.X, pmin.Y),
			base_corner + v3f(pmax.Z, pmax.X, pmax.Y),
			choose_color(block_pos.Y, block_pos.Z)
		);
	}
}

void Hud::updateSelectionMesh(const v3s16 &camera_offset)
{
	m_camera_offset = camera_offset;
	if (m_mode != HIGHLIGHT_HALO)
		return;

	if (m_selection_mesh) {
		m_selection_mesh->drop();
		m_selection_mesh = NULL;
	}

	if (m_selection_boxes.empty()) {
		// No pointed object
		return;
	}

	// New pointed object, create new mesh.

	// Texture UV coordinates for selection boxes
	static f32 texture_uv[24] = {
		0,0,1,1,
		0,0,1,1,
		0,0,1,1,
		0,0,1,1,
		0,0,1,1,
		0,0,1,1
	};

	// Use single halo box instead of multiple overlapping boxes.
	// Temporary solution - problem can be solved with multiple
	// rendering targets, or some method to remove inner surfaces.
	// Thats because of halo transparency.

	aabb3f halo_box(100.0, 100.0, 100.0, -100.0, -100.0, -100.0);
	m_halo_boxes.clear();

	for (const auto &selection_box : m_selection_boxes) {
		halo_box.addInternalBox(selection_box);
	}

	m_halo_boxes.push_back(halo_box);
	m_selection_mesh = convertNodeboxesToMesh(
		m_halo_boxes, texture_uv, 0.5);
}

void Hud::resizeHotbar() {
	const v2u32 &window_size = RenderingEngine::getWindowSize();

	if (m_screensize != window_size) {
		m_hotbar_imagesize = floor(HOTBAR_IMAGE_SIZE *
			RenderingEngine::getDisplayDensity() + 0.5);
		m_hotbar_imagesize *= m_hud_scaling;
		m_padding = m_hotbar_imagesize / 12;
		m_screensize = window_size;
		m_displaycenter = v2s32(m_screensize.X/2,m_screensize.Y/2);
	}
}

struct MeshTimeInfo {
	u64 time;
	scene::IMesh *mesh = nullptr;
};

void drawItemStack(
		video::IVideoDriver *driver,
		gui::IGUIFont *font,
		const ItemStack &item,
		const core::rect<s32> &rect,
		const core::rect<s32> *clip,
		Client *client,
		ItemRotationKind rotation_kind,
		const v3s16 &angle,
		const v3s16 &rotation_speed)
{
	static MeshTimeInfo rotation_time_infos[IT_ROT_NONE];

	if (item.empty()) {
		if (rotation_kind < IT_ROT_NONE && rotation_kind != IT_ROT_OTHER) {
			rotation_time_infos[rotation_kind].mesh = NULL;
		}
		return;
	}

	const bool enable_animations = g_settings->getBool("inventory_items_animations");

	auto *idef = client->idef();
	const ItemDefinition &def = item.getDefinition(idef);

	bool draw_overlay = false;

	const std::string inventory_image = item.getInventoryImage(idef);
	const std::string inventory_overlay = item.getInventoryOverlay(idef);

	bool has_mesh = false;
	ItemMesh *imesh;

	core::rect<s32> viewrect = rect;
	if (clip != nullptr)
		viewrect.clipAgainst(*clip);

	// Render as mesh if animated or no inventory image
	if ((enable_animations && rotation_kind < IT_ROT_NONE) || inventory_image.empty()) {
		imesh = idef->getWieldMesh(item, client);
		has_mesh = imesh && imesh->mesh;
	}
	if (has_mesh) {
		scene::IMesh *mesh = imesh->mesh;
		driver->clearBuffers(video::ECBF_DEPTH);
		s32 delta = 0;
		if (rotation_kind < IT_ROT_NONE) {
			MeshTimeInfo &ti = rotation_time_infos[rotation_kind];
			if (mesh != ti.mesh && rotation_kind != IT_ROT_OTHER) {
				ti.mesh = mesh;
				ti.time = porting::getTimeMs();
			} else {
				delta = porting::getDeltaMs(ti.time, porting::getTimeMs()) % 100000;
			}
		}
		core::rect<s32> oldViewPort = driver->getViewPort();
		core::matrix4 oldProjMat = driver->getTransform(video::ETS_PROJECTION);
		core::matrix4 oldViewMat = driver->getTransform(video::ETS_VIEW);

		core::matrix4 ProjMatrix;
		ProjMatrix.buildProjectionMatrixOrthoLH(2.0f, 2.0f, -1.0f, 100.0f);

		core::matrix4 ViewMatrix;
		ViewMatrix.buildProjectionMatrixOrthoLH(
			2.0f * viewrect.getWidth() / rect.getWidth(),
			2.0f * viewrect.getHeight() / rect.getHeight(),
			-1.0f,
			100.0f);
		ViewMatrix.setTranslation(core::vector3df(
			1.0f * (rect.LowerRightCorner.X + rect.UpperLeftCorner.X -
					viewrect.LowerRightCorner.X - viewrect.UpperLeftCorner.X) /
					viewrect.getWidth(),
			1.0f * (viewrect.LowerRightCorner.Y + viewrect.UpperLeftCorner.Y -
					rect.LowerRightCorner.Y - rect.UpperLeftCorner.Y) /
					viewrect.getHeight(),
			0.0f));

		driver->setTransform(video::ETS_PROJECTION, ProjMatrix);
		driver->setTransform(video::ETS_VIEW, ViewMatrix);

		core::matrix4 matrix;
		matrix.makeIdentity();

		if (enable_animations) {
			float timer_f = (float) delta / 5000.f;
			matrix.setRotationDegrees(v3f(
				angle.X + rotation_speed.X * 3.60f * timer_f,
				angle.Y + rotation_speed.Y * 3.60f * timer_f,
				angle.Z + rotation_speed.Z * 3.60f * timer_f)
			);
		}

		driver->setTransform(video::ETS_WORLD, matrix);
		driver->setViewPort(viewrect);

		video::SColor basecolor =
			client->idef()->getItemstackColor(item, client);

		const u32 mc = mesh->getMeshBufferCount();
		if (mc > imesh->buffer_colors.size())
			imesh->buffer_colors.resize(mc);
		for (u32 j = 0; j < mc; ++j) {
			scene::IMeshBuffer *buf = mesh->getMeshBuffer(j);
			video::SColor c = basecolor;

			auto &p = imesh->buffer_colors[j];
			p.applyOverride(c);

			// TODO: could be moved to a shader
			if (p.needColorize(c)) {
				buf->setDirty(scene::EBT_VERTEX);
				if (imesh->needs_shading)
					colorizeMeshBuffer(buf, &c);
				else
					setMeshBufferColor(buf, c);
			}

			video::SMaterial &material = buf->getMaterial();
			material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF;
			driver->setMaterial(material);
			driver->drawMeshBuffer(buf);
		}

		driver->setTransform(video::ETS_VIEW, oldViewMat);
		driver->setTransform(video::ETS_PROJECTION, oldProjMat);
		driver->setViewPort(oldViewPort);

		draw_overlay = def.type == ITEM_NODE && inventory_image.empty();
	} else { // Otherwise just draw as 2D
		video::ITexture *texture = client->idef()->getInventoryTexture(item, client);
		video::SColor color;
		if (texture) {
			color = client->idef()->getItemstackColor(item, client);
		} else {
			color = video::SColor(255, 255, 255, 255);
			ITextureSource *tsrc = client->getTextureSource();
			texture = tsrc->getTexture("no_texture.png");
			if (!texture)
				return;
		}

		const video::SColor colors[] = { color, color, color, color };

		draw2DImageFilterScaled(driver, texture, rect,
			core::rect<s32>({0, 0}, core::dimension2di(texture->getOriginalSize())),
			clip, colors, true);

		draw_overlay = true;
	}

	// draw the inventory_overlay
	if (!inventory_overlay.empty() && draw_overlay) {
		ITextureSource *tsrc = client->getTextureSource();
		video::ITexture *overlay_texture = tsrc->getTexture(inventory_overlay);
		core::dimension2d<u32> dimens = overlay_texture->getOriginalSize();
		core::rect<s32> srcrect(0, 0, dimens.Width, dimens.Height);
		draw2DImageFilterScaled(driver, overlay_texture, rect, srcrect, clip, 0, true);
	}

	if (def.type == ITEM_TOOL && item.wear != 0) {
		// Draw a progressbar
		float barheight = static_cast<float>(rect.getHeight()) / 16;
		float barpad_x = static_cast<float>(rect.getWidth()) / 16;
		float barpad_y = static_cast<float>(rect.getHeight()) / 16;

		core::rect<s32> progressrect(
			rect.UpperLeftCorner.X + barpad_x,
			rect.LowerRightCorner.Y - barpad_y - barheight,
			rect.LowerRightCorner.X - barpad_x,
			rect.LowerRightCorner.Y - barpad_y);

		// Shrink progressrect by amount of tool damage
		float wear = item.wear / 65535.0f;
		int progressmid =
			wear * progressrect.UpperLeftCorner.X +
			(1 - wear) * progressrect.LowerRightCorner.X;

		// Compute progressbar color
		// default scheme:
		//   wear = 0.0: green
		//   wear = 0.5: yellow
		//   wear = 1.0: red

		video::SColor color;
		auto barParams = item.getWearBarParams(client->idef());
		if (barParams.has_value()) {
			f32 durabilityPercent = 1.0 - wear;
			color = barParams->getWearBarColor(durabilityPercent);
		} else {
			color = video::SColor(255, 255, 255, 255);
			int wear_i = MYMIN(std::floor(wear * 600), 511);
			wear_i = MYMIN(wear_i + 10, 511);

			if (wear_i <= 255)
				color.set(255, wear_i, 255, 0);
			else
				color.set(255, 255, 511 - wear_i, 0);
		}

		core::rect<s32> progressrect2 = progressrect;
		progressrect2.LowerRightCorner.X = progressmid;
		driver->draw2DRectangle(color, progressrect2, clip);

		color = video::SColor(255, 0, 0, 0);
		progressrect2 = progressrect;
		progressrect2.UpperLeftCorner.X = progressmid;
		driver->draw2DRectangle(color, progressrect2, clip);
	}

	const std::string &count_text = item.metadata.getString("count_meta");
	if (font != nullptr && (item.count >= 2 || !count_text.empty())) {
		// Get the item count as a string
		std::string text = count_text.empty() ? itos(item.count) : count_text;
		v2u32 dim = font->getDimension(utf8_to_wide(unescape_enriched(text)).c_str());
		v2s32 sdim(dim.X, dim.Y);

		core::rect<s32> rect2(
			rect.LowerRightCorner - sdim,
			rect.LowerRightCorner
		);

		// get the count alignment
		s32 count_alignment = stoi(item.metadata.getString("count_alignment"));
		if (count_alignment != 0) {
			s32 a_x = count_alignment & 3;
			s32 a_y = (count_alignment >> 2) & 3;

			s32 x1, x2, y1, y2;
			switch (a_x) {
			case 1: // left
				x1 = rect.UpperLeftCorner.X;
				x2 = x1 + sdim.X;
				break;
			case 2: // middle
				x1 = (rect.UpperLeftCorner.X + rect.LowerRightCorner.X - sdim.X) / 2;
				x2 = x1 + sdim.X;
				break;
			case 3: // right
				x2 = rect.LowerRightCorner.X;
				x1 = x2 - sdim.X;
				break;
			default: // 0 = default
				x1 = rect2.UpperLeftCorner.X;
				x2 = rect2.LowerRightCorner.X;
				break;
			}

			switch (a_y) {
			case 1: // up
				y1 = rect.UpperLeftCorner.Y;
				y2 = y1 + sdim.Y;
				break;
			case 2: // middle
				y1 = (rect.UpperLeftCorner.Y + rect.LowerRightCorner.Y - sdim.Y) / 2;
				y2 = y1 + sdim.Y;
				break;
			case 3: // down
				y2 = rect.LowerRightCorner.Y;
				y1 = y2 - sdim.Y;
				break;
			default: // 0 = default
				y1 = rect2.UpperLeftCorner.Y;
				y2 = rect2.LowerRightCorner.Y;
				break;
			}

			rect2 = core::rect<s32>(x1, y1, x2, y2);
		}

		video::SColor color(255, 255, 255, 255);
		font->draw(utf8_to_wide(text).c_str(), rect2, color, false, false, &viewrect);
	}
}

void drawItemStack(
		video::IVideoDriver *driver,
		gui::IGUIFont *font,
		const ItemStack &item,
		const core::rect<s32> &rect,
		const core::rect<s32> *clip,
		Client *client,
		ItemRotationKind rotation_kind)
{
	drawItemStack(driver, font, item, rect, clip, client, rotation_kind,
		v3s16(0, 0, 0), v3s16(0, 100, 0));
}
