// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2010-2013 blue42u, Jonathon Anderson <anderjon@umail.iu.edu>
// Copyright (C) 2010-2013 kwolekr, Ryan Kwolek <kwolekr@minetest.net>

#include "client/hud.h"
#include "client/perfmonitor.h"
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
#include "filesys.h"
#include "gui/mainmenumanager.h"
#include "wieldmesh.h"
#include "client/renderingengine.h"
#include "client/minimap.h"
#include "client/texturesource.h"
#include "gui/touchcontrols.h"
#include "client/macrolist.h"
#include "gui/imgui_hud.h"
#include "imgui.h" // IM_COL32() for gatherItemCell()'s wear-bar color
#include "util/enriched_string.h"
#include "irrlicht_changes/CGUITTFont.h"
#include "IFileSystem.h"
#include "IReadFile.h"
#include "gui/custom_menu/ModernUI.h"

#define OBJECT_CROSSHAIR_LINE_SIZE 8
#define CROSSHAIR_LINE_SIZE 10

// getHudColorSetting()/drawHudColorPanel() (the Irrlicht-drawing
// versions of what src/gui/imgui_hud.cpp's hudColor()/drawPanel() now
// do) used to live here -- removed as dead code once every element that
// called them (KeyStroker/ShowCPS/Coords/ShowFPS/ShowPing/MusicHUD/
// ShowRP/ConsumptionHUD/TargetHUD/InventoryHUD/CraftHUD) had been
// rewritten onto ImGui.

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

	// See src/client/photohud.h -- must be called after both driver and
	// tsrc above are set.
	m_photo_hud.init(driver, tsrc);

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
	if (m_rp_screenshot_texture)
		driver->removeTexture(m_rp_screenshot_texture);
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
	// formatMinSec()/drawMarqueeLine() (the Irrlicht-drawing versions of
	// what src/gui/imgui_hud.cpp's formatMinSec()/drawMarqueeText() now
	// do) used to live here -- removed as dead code once drawMusicHud()/
	// drawShowRp() below stopped calling them (see those functions'
	// comments on the ImGuiHud rewrite).
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
	// Ground-up rewrite: this used to draw the panel/art/marquee text/
	// progress bar itself; now just polls NowPlaying and updates the
	// thumbnail texture cache (unchanged from before), then hands
	// everything to ImGuiHud (src/gui/imgui_hud.h/.cpp) to actually draw
	// later this frame. See the class comment on ImGuiHud for why.
	if (!g_settings->getBool("music_hud")) {
		ImGuiHud::get().updateMusicHud(ImGuiHud::MusicHudState());
		return;
	}

	const NowPlayingInfo &info = m_now_playing.poll();
	if (!info.active || info.title.empty()) {
		updateMusicThumbnail(NowPlayingInfo()); // drop any cached art
		ImGuiHud::get().updateMusicHud(ImGuiHud::MusicHudState());
		return;
	}

	updateMusicThumbnail(info);

	ImGuiHud::MusicHudState state;
	state.visible = true;
	state.source = info.source;
	state.title = info.title;
	state.artist = info.artist;
	state.art_texture = m_music_thumbnail_texture;
	state.has_progress = info.has_progress;
	state.position_seconds = info.position_seconds;
	state.duration_seconds = info.duration_seconds;
	// Global size multiplier for MineBoost's custom HUD elements,
	// combined with this HUD's own independent multiplier -- see
	// "hud_size" and "music_hud_size" in src/gui/custom_menu/
	// ImGuiMineBoostMenu.cpp ("HUD Size" slider and scroll-to-resize in
	// "Move HUD" edit mode, respectively).
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("music_hud_size"), 0.5f, 2.5f);
	ImGuiHud::get().updateMusicHud(state);
}


// ShowRP: shows the currently active texture pack's own screenshot.png +
// texture_pack.conf metadata (title, author) -- the exact same files/
// convention a ContentDB package ships with and the main menu's content
// browser reads (see load_texture_packs() in
// builtin/mainmenu/content/pkgmgr.lua: "texture_path" is the active
// pack's directory, "title"/"author" come from its texture_pack.conf,
// falling back to the directory name for the title if that key isn't
// set, same as pkgmgr.lua does). Same panel style/layout as MusicHud
// just above (down to reusing its exact box-size formula), minus the
// playback progress line -- there's nothing to track progress of here.
//
// Layout is deliberately fixed at 2 text lines' worth of room (title +
// author) regardless of whether either is actually present for the
// current pack -- if this shrank/grew per-pack, the "Move HUD" drag-
// preview box in Menu.cpp (drawShowRpPreview(), which can't know any of
// that at edit-mode time) would drift out of sync with it, the same bug
// KeyStroker/ShowCPS/Coords all had before their size formulas were
// fixed to be exact-match rather than approximate.
void Hud::drawShowRp()
{
	// Ground-up rewrite: this used to draw the panel/screenshot/text
	// itself; now just resolves the active texture pack's metadata
	// (same caching -- only re-read when the pack actually changes) and
	// hands it to ImGuiHud (src/gui/imgui_hud.h/.cpp) to actually draw
	// later this frame. See the class comment on ImGuiHud for why.
	if (!g_settings->getBool("show_rp")) {
		ImGuiHud::get().updateShowRp(ImGuiHud::ShowRpState());
		return;
	}

	std::string texture_path = g_settings->get("texture_path");

	// Only re-read texture_pack.conf and reload the screenshot when the
	// active texture pack actually changes, not every frame.
	if (texture_path != m_rp_cached_texture_path) {
		m_rp_cached_texture_path = texture_path;
		m_rp_active = false;
		m_rp_title.clear();
		m_rp_author.clear();
		if (m_rp_screenshot_texture) {
			driver->removeTexture(m_rp_screenshot_texture);
			m_rp_screenshot_texture = nullptr;
		}

		if (!texture_path.empty()) {
			Settings pack_conf;
			pack_conf.readConfigFile(
				(texture_path + DIR_DELIM + "texture_pack.conf").c_str());

			// Fallback title if texture_pack.conf doesn't set one: the
			// pack's own directory name, same as pkgmgr.lua's
			// "local title = conf:get(\"title\") or item".
			std::string dir_name = texture_path;
			while (!dir_name.empty() &&
					(dir_name.back() == '/' || dir_name.back() == '\\'))
				dir_name.pop_back();
			size_t slash = dir_name.find_last_of("/\\");
			if (slash != std::string::npos)
				dir_name = dir_name.substr(slash + 1);

			m_rp_title = pack_conf.exists("title") ? pack_conf.get("title") : dir_name;
			m_rp_author = pack_conf.exists("author") ? pack_conf.get("author") : "";

			std::string screenshot_path = texture_path + DIR_DELIM + "screenshot.png";
			if (fs::PathExists(screenshot_path))
				m_rp_screenshot_texture = driver->getTexture(screenshot_path.c_str());

			m_rp_active = true;
		}
	}

	ImGuiHud::ShowRpState state;
	// Nothing to show for the built-in/base textures (texture_path
	// empty). Unlike an older version of this function, a pack with
	// neither a screenshot nor any metadata still shows the panel (with
	// its directory name as the title) rather than being skipped.
	state.visible = m_rp_active;
	if (state.visible) {
		state.title = m_rp_title;
		state.author = m_rp_author;
		state.screenshot_texture = m_rp_screenshot_texture;
		state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
			* rangelim(g_settings->getFloat("rp_hud_size"), 0.5f, 2.5f);
	}
	ImGuiHud::get().updateShowRp(state);
}

void Hud::drawConsumptionHud()
{
	// Ground-up rewrite: this used to draw the panel+text itself
	// (Irrlicht font->draw()/drawHudColorPanel()); now just gathers the
	// numbers and hands them to ImGuiHud (src/gui/imgui_hud.h/.cpp),
	// which does the actual drawing later this same frame, via ImGui.
	// See the class comment on ImGuiHud for exactly why this needs to
	// be a "gather now, draw later" split rather than drawing directly
	// from here.
	ImGuiHud::ConsumptionHudState state;
	state.visible = g_settings->getBool("consumption_hud");
	if (state.visible) {
		state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
			* rangelim(g_settings->getFloat("consumption_hud_size"), 0.5f, 2.5f);

		PerfMonitor &perf = PerfMonitor::get();
		float ram_mb = perf.getRamMb();
		float cpu_pct = perf.getCpuPercent();
		float gpu_pct = perf.getGpuPercent();

		// One line: "RAM: 512 MB  CPU: 12%  GPU: 34%" -- GPU segment
		// only appears when actually available (see PerfMonitor::
		// getGpuPercent() in src/client/perfmonitor.h: -1 means "not
		// sampled on this platform/build", currently anywhere but
		// Windows).
		wchar_t buf[96];
		if (gpu_pct >= 0.0f) {
			swprintf(buf, 96, L"RAM: %d MB  CPU: %d%%  GPU: %d%%",
				(int)(ram_mb + 0.5f), (int)(cpu_pct + 0.5f), (int)(gpu_pct + 0.5f));
		} else {
			swprintf(buf, 96, L"RAM: %d MB  CPU: %d%%",
				(int)(ram_mb + 0.5f), (int)(cpu_pct + 0.5f));
		}
		state.text = buf;
	}
	ImGuiHud::get().updateConsumptionHud(state);
}


void Hud::drawPhotoHud()
{
	// Thin forwarder -- see src/client/photohud.h/.cpp for the actual
	// implementation (a ground-up rewrite; this used to be ~130 lines of
	// PhotoHUD-specific logic inlined right here).
	m_photo_hud.draw(m_screensize, isMenuActive());
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
// Coords/FPS/Ping -- ground-up rewrite onto ImGui (see the class comment
// on ImGuiHud, src/gui/imgui_hud.h, and the "MineBoost:" comment on
// GameUI::update() in src/client/gameui.cpp, which used to own the
// actual text for these three). Split into 3 separate functions (rather
// than the old shared draw_box() lambda) specifically so each one is its
// own clean override point -- see the task this was written for.
void Hud::drawCoordsHud()
{
	if (!g_settings->getBool("show_coords")) {
		ImGuiHud::get().updateCoordsHud(ImGuiHud::SimpleTextHudState());
		return;
	}

	std::ostringstream os(std::ios_base::binary);
	os << std::setprecision(1) << std::fixed
		<< "(" << "X: " << (player->getPosition().X / BS)
		<< ", Y: " << (player->getPosition().Y / BS)
		<< ", Z: " << (player->getPosition().Z / BS) << ")";

	ImGuiHud::SimpleTextHudState state;
	state.visible = true;
	state.text = os.str();
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("coords_size"), 0.5f, 2.5f);
	state.color_setting = "hud_color_coords";
	state.combined_v2f = true;
	state.pos_setting = "coords_sprite";
	state.default_y_offset = 5.0f;
	ImGuiHud::get().updateCoordsHud(state);
}

void Hud::drawFpsHud()
{
	if (!g_settings->getBool("show_fps")) {
		ImGuiHud::get().updateFpsHud(ImGuiHud::SimpleTextHudState());
		m_fps_last_time_ms = 0;
		return;
	}

	// Hud doesn't have access to GameUI's own RunStats::dtime_jitter
	// (the smoothed frametime average GameUI::update() used to derive
	// FPS from) -- this is called from the render pipeline instead (see
	// src/client/render/plain.cpp), not anywhere RunStats is passed
	// through to. Computed independently here instead: a simple
	// exponential moving average of the time between calls to this
	// function (i.e. between frames), smoothed enough to be readable
	// without needing RunStats' own jitter-averaging machinery.
	u64 now_ms = porting::getTimeMs();
	if (m_fps_last_time_ms != 0) {
		float frame_dtime = std::max((now_ms - m_fps_last_time_ms) / 1000.0f, 0.0001f);
		float instant_fps = 1.0f / frame_dtime;
		m_fps_smoothed = m_fps_smoothed <= 0.0f ? instant_fps :
			m_fps_smoothed * 0.9f + instant_fps * 0.1f;
	}
	m_fps_last_time_ms = now_ms;

	ImGuiHud::SimpleTextHudState state;
	state.visible = true;
	state.text = "[FPS: " + std::to_string((int)(m_fps_smoothed + 0.5f)) + "]";
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("fps_size"), 0.5f, 2.5f);
	state.color_setting = "hud_color_fps";
	state.combined_v2f = true;
	state.pos_setting = "fov_coords";
	state.default_y_offset = 25.0f;
	ImGuiHud::get().updateFpsHud(state);
}

void Hud::drawPingHud()
{
	if (!g_settings->getBool("show_ping")) {
		ImGuiHud::get().updatePingHud(ImGuiHud::SimpleTextHudState());
		return;
	}

	ImGuiHud::SimpleTextHudState state;
	state.visible = true;
	state.text = "[Ping: " + std::to_string((int)(client->getRTT() * 1000.0f)) + " ms]";
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("ping_size"), 0.5f, 2.5f);
	state.color_setting = "hud_color_ping";
	state.combined_v2f = true;
	state.pos_setting = "ping_coords";
	state.default_y_offset = 45.0f;
	ImGuiHud::get().updatePingHud(state);
}

// KeyStroker/ShowCPS -- ground-up rewrite onto ImGui (see the class
// comment on ImGuiHud). Key state read directly from LocalPlayer's own
// control struct -- the same up/down/left/right/jump/aux1/sneak/dig/
// place fields builtin/client/keystroker.lua used to read via
// minetest.localplayer:get_control(), just from the C++ side instead
// (see the "if false then" wrapping that file's own now-unused HUD
// elements, with a comment explaining why and how to restore it).
void Hud::drawKeyStrokerHud()
{
	if (!g_settings->getBool("show_keys")) {
		ImGuiHud::get().updateKeyStrokerHud(ImGuiHud::KeyStrokerState());
		return;
	}

	const PlayerControl &ctl = player->getPlayerControl();
	ImGuiHud::KeyStrokerState state;
	state.visible = true;
	state.up = ctl.direction_keys & 1;
	state.down = ctl.direction_keys & 2;
	state.left = ctl.direction_keys & 4;
	state.right = ctl.direction_keys & 8;
	state.jump = ctl.jump;
	state.aux1 = ctl.aux1;
	state.sneak = ctl.sneak;
	state.dig = ctl.dig;
	state.place = ctl.place;
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("keys_size"), 0.5f, 2.5f);
	ImGuiHud::get().updateKeyStrokerHud(state);
}

void Hud::drawCpsHud()
{
	if (!g_settings->getBool("show_cps")) {
		ImGuiHud::get().updateCpsHud(ImGuiHud::CpsState());
		m_cps_lmb_clicks = 0;
		m_cps_rmb_clicks = 0;
		m_cps_lmb_timer = 0.0f;
		m_cps_rmb_timer = 0.0f;
		return;
	}

	// Same rising-edge-detect-then-reset-every-1s algorithm
	// keystroker.lua's track_lmb_clicks()/track_rmb_clicks() used --
	// dtime isn't directly available here (this is called from the
	// render pipeline, see src/client/render/plain.cpp, not Client::step()),
	// so it's derived from the same time source everything else in this
	// file already uses for per-frame timing (porting::getTimeMs()).
	u64 now_ms = porting::getTimeMs();
	float dtime = m_cps_last_time_ms == 0 ? 0.0f :
		std::min((now_ms - m_cps_last_time_ms) / 1000.0f, 1.0f);
	m_cps_last_time_ms = now_ms;

	const PlayerControl &ctl = player->getPlayerControl();
	if (ctl.dig && !m_cps_lmb_was_down)
		m_cps_lmb_clicks++;
	m_cps_lmb_was_down = ctl.dig;
	if (ctl.place && !m_cps_rmb_was_down)
		m_cps_rmb_clicks++;
	m_cps_rmb_was_down = ctl.place;

	m_cps_lmb_timer += dtime;
	if (m_cps_lmb_timer >= 1.0f) {
		m_cps_lmb_clicks = 0;
		m_cps_lmb_timer = 0.0f;
	}
	m_cps_rmb_timer += dtime;
	if (m_cps_rmb_timer >= 1.0f) {
		m_cps_rmb_clicks = 0;
		m_cps_rmb_timer = 0.0f;
	}

	ImGuiHud::CpsState state;
	state.visible = true;
	state.lmb_cps = m_cps_lmb_clicks;
	state.rmb_cps = m_cps_rmb_clicks;
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("cps_size"), 0.5f, 2.5f);
	ImGuiHud::get().updateCpsHud(state);
}

void Hud::drawMacroWheel()
{
	if (!macro_wheel_open)
		return;

	const auto &macros = MacroList::get().getAll();
	if (macros.empty())
		return;

	float hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f);
	unsigned int scaled_font_size = (unsigned int)(g_fontengine->getDefaultFontSize() * hud_size);
	gui::IGUIFont *font = g_fontengine->getFont(scaled_font_size);
	if (!font)
		return;

	s32 cx = (s32)m_screensize.X / 2;
	s32 cy = (s32)m_screensize.Y / 2;

	// Dim the world behind the wheel so the selection reads clearly.
	driver->draw2DRectangle(video::SColor(90, 0, 0, 0),
		core::rect<s32>(0, 0, (s32)m_screensize.X, (s32)m_screensize.Y));

	s32 radius = (s32)(180 * hud_size);
	s32 box_w = (s32)(170 * hud_size);
	s32 box_h = (s32)(36 * hud_size);
	int selected = ((macro_wheel_selected % (int)macros.size()) + (int)macros.size())
		% (int)macros.size();

	for (size_t i = 0; i < macros.size(); i++) {
		float angle = (-90.0f + i * (360.0f / macros.size())) * M_PI / 180.0f;
		s32 ix = cx + (s32)(radius * std::cos(angle));
		s32 iy = cy + (s32)(radius * std::sin(angle));

		core::rect<s32> box(ix - box_w / 2, iy - box_h / 2, ix + box_w / 2, iy + box_h / 2);
		bool is_selected = ((int)i == selected);

		ModernUI::panel(driver, box, ModernUI::RadiusSmall,
			is_selected ? video::SColor(220, 30, 40, 55) : video::SColor(190, 24, 26, 34),
			is_selected ? ModernUI::PanelBorder : ModernUI::PanelBorderDim,
			/*shadow=*/false);

		if (is_selected)
			driver->draw2DLine(core::position2d<s32>(cx, cy),
				core::position2d<s32>(ix, iy), video::SColor(140, 120, 255, 120));

		std::wstring label = utf8_to_wide(macros[i]);
		// Truncate long commands so they don't spill out of the box --
		// full text is still visible via ".macro list" in chat.
		const size_t max_chars = 22;
		if (label.size() > max_chars)
			label = label.substr(0, max_chars - 1) + L"\u2026";

		font->draw(label.c_str(), box, video::SColor(255, 255, 255, 255), true, true);
	}

	// Small center dot so the ring has a clear focal point.
	s32 dot = (s32)(4 * hud_size);
	driver->draw2DRectangle(video::SColor(220, 255, 255, 255),
		core::rect<s32>(cx - dot, cy - dot, cx + dot, cy + dot));
}

void Hud::drawTargetHud()
{
	// Ground-up rewrite: this used to draw the panel/avatar/name/HP bar
	// itself; now just resolves the avatar texture + UV crops (still
	// Irrlicht-side, since only this code knows the skin texture's own
	// resolution) and hands everything to ImGuiHud (src/gui/imgui_hud.h/
	// .cpp) to actually draw later this frame. See the class comment on
	// ImGuiHud for why.
	if (!target_hud_active || !g_settings->getBool("target_hud")) {
		ImGuiHud::get().updateTargetHud(ImGuiHud::TargetHudState());
		return;
	}

	ImGuiHud::TargetHudState state;
	state.visible = true;
	state.name = target_hud_name;
	state.hp = target_hud_hp;
	state.hp_max = target_hud_hp_max;
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("target_hud_size"), 0.5f, 2.5f);

	// Small avatar crop of the target's skin. Player skins use the same
	// head-front UV layout as Minecraft skins: an 8x8 square at (8,8) in
	// a 64-px-wide texture (scaled up proportionally for higher-
	// resolution skins), plus a second "hat/hair" overlay layer at
	// (40,8), composited on top so hats/hair/facial accessories actually
	// show up. UVs are normalized (0..1) here, not pixel rects, since
	// that's what ImGui::Image()/ImDrawList::AddImage() take -- see the
	// comment on TargetHudState in imgui_hud.h.
	if (!target_hud_skin.empty()) {
		if (video::ITexture *avatar_texture = tsrc->getTexture(target_hud_skin)) {
			core::dimension2du sz = avatar_texture->getOriginalSize();
			if (sz.Width > 0 && sz.Height > 0) {
				f32 scale = sz.Width / 64.0f;
				f32 fx = 8.0f * scale, fy = 8.0f * scale;
				f32 fw = std::max(1.0f, 8.0f * scale), fh = std::max(1.0f, 8.0f * scale);

				state.avatar_texture = avatar_texture;
				state.avatar_uv0_x = fx / sz.Width;
				state.avatar_uv0_y = fy / sz.Height;
				state.avatar_uv1_x = (fx + fw) / sz.Width;
				state.avatar_uv1_y = (fy + fh) / sz.Height;

				// The hat/hair layer sits to the right of the base head
				// on the same row. Only use it if the texture is tall
				// enough to actually contain that row (old 32-px-tall
				// legacy skins without a second layer would otherwise
				// sample garbage).
				f32 ox = 40.0f * scale, oy = fy;
				if (ox + fw <= sz.Width && oy + fh <= sz.Height) {
					state.avatar_has_overlay = true;
					state.overlay_uv0_x = ox / sz.Width;
					state.overlay_uv0_y = oy / sz.Height;
					state.overlay_uv1_x = (ox + fw) / sz.Width;
					state.overlay_uv1_y = (oy + fh) / sz.Height;
				}
			}
		}
	}

	ImGuiHud::get().updateTargetHud(state);
}

// Resolves one inventory slot to a flat-icon-only ImGuiHud::ItemCellState
// -- shared by drawInventoryHud()/drawCraftHud() below. See the class
// comment on ImGuiHud (src/gui/imgui_hud.h) for why this only handles
// the flat-2D-icon case, not items that would need a 3D wielditem mesh
// rendered into the slot (animated items / items with no inventory
// image at all) -- those simply show no icon here, same as an empty
// slot, rather than attempting something a 2D immediate-mode GUI
// library has no way to actually do.
static ImGuiHud::ItemCellState gatherItemCell(const ItemStack &item, Client *client, ITextureSource *tsrc)
{
	ImGuiHud::ItemCellState cell;
	if (item.empty())
		return cell;

	auto *idef = client->idef();
	const ItemDefinition &def = item.getDefinition(idef);

	std::string inventory_image = item.getInventoryImage(idef);
	if (!inventory_image.empty())
		cell.icon_texture = tsrc->getTexture(inventory_image);

	std::string inventory_overlay = item.getInventoryOverlay(idef);
	if (cell.icon_texture && !inventory_overlay.empty())
		cell.overlay_texture = tsrc->getTexture(inventory_overlay);

	if (def.type == ITEM_TOOL && item.wear != 0) {
		cell.has_wear = true;
		float wear = item.wear / 65535.0f;
		cell.wear_fraction = wear;

		// Same color gradient/WearBarParams handling as the old
		// drawItemStack() (still used by the vanilla formspec/hotbar --
		// see hud.cpp above) -- green -> yellow -> red as the tool wears
		// out, or a mod-defined gradient via WearBarParams if the item
		// registered one.
		auto barParams = item.getWearBarParams(client->idef());
		if (barParams.has_value()) {
			f32 durability = 1.0f - wear;
			video::SColor c = barParams->getWearBarColor(durability);
			cell.wear_color = IM_COL32(c.getRed(), c.getGreen(), c.getBlue(), 255);
		} else {
			int wear_i = std::min((int)std::floor(wear * 600), 511);
			wear_i = std::min(wear_i + 10, 511);
			if (wear_i <= 255)
				cell.wear_color = IM_COL32(wear_i, 255, 0, 255);
			else
				cell.wear_color = IM_COL32(255, 511 - wear_i, 0, 255);
		}
	}

	const std::string &count_text = item.metadata.getString("count_meta");
	if (item.count >= 2 || !count_text.empty())
		cell.count_text = count_text.empty() ? itos(item.count) : count_text;
	// Note: unlike the old drawItemStack(), a "count_alignment" metadata
	// override (a formspec-only customization, rarely set outside
	// custom crafted UI) is not honored here -- the count badge is
	// always bottom-right, same as the vast majority of items already
	// show. Not worth the extra complexity in a HUD-overlay grid, which
	// (unlike a formspec slot) doesn't have per-slot layout metadata
	// driving anything else about it either.

	return cell;
}

// Builds the ImGuiHud::ItemGridState sections for one or more inventory
// lists -- shared by drawInventoryHud()/drawCraftHud() below (identical
// "grid of sections" shape either way, just different lists/titles/
// settings).
static void gatherItemGridSection(std::vector<ImGuiHud::ItemGridSection> &sections,
		const std::string &title, InventoryList *list, Client *client, ITextureSource *tsrc)
{
	if (!list || list->getSize() == 0)
		return;

	ImGuiHud::ItemGridSection sec;
	sec.title = title;
	s32 count = (s32)list->getSize();
	sec.cols = std::min<s32>(8, count);
	sec.rows = (count + sec.cols - 1) / sec.cols;
	sec.cells.reserve(count);
	for (s32 i = 0; i < count; i++)
		sec.cells.push_back(gatherItemCell(list->getItem(i), client, tsrc));

	sections.push_back(std::move(sec));
}

void Hud::drawInventoryHud()
{
	// Ground-up rewrite: this used to draw the whole grid itself; now
	// just resolves each visible list's items to flat icons/wear/count
	// (gatherItemGridSection() above) and hands the result to ImGuiHud
	// (src/gui/imgui_hud.h/.cpp) to actually draw later this frame. See
	// the class comment on ImGuiHud for why, and for the one real
	// limitation (no 3D wielditem mesh rendering) this rewrite has.
	if (!g_settings->getBool("inventory_hud")) {
		ImGuiHud::get().updateInventoryHud(ImGuiHud::ItemGridState());
		return;
	}

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
		ImGuiHud::get().updateInventoryHud(ImGuiHud::ItemGridState());
		return;
	}

	ImGuiHud::ItemGridState state;
	gatherItemGridSection(state.sections, "Inventory", mainlist, client, tsrc);

	// See the extra-list setting name below -- only re-parsed when its
	// raw value actually changes, same caching this had before, since
	// it's effectively static at runtime.
	static std::string cached_extra_lists_raw;
	static std::vector<std::pair<std::string, std::string>> cached_extra_lists;
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
			cached_extra_lists.emplace_back(name, title);
		}
	}
	for (const auto &entry : cached_extra_lists)
		gatherItemGridSection(state.sections, entry.second, inventory->getList(entry.first), client, tsrc);

	if (state.sections.empty()) {
		ImGuiHud::get().updateInventoryHud(ImGuiHud::ItemGridState());
		return;
	}

	state.visible = true;
	// Reuse the hotbar's independent size multiplier convention -- see
	// "inventory_hud_size" in src/gui/custom_menu/ImGuiMineBoostMenu.cpp
	// ("Move HUD" edit mode scroll-to-resize).
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("inventory_hud_size"), 0.5f, 2.5f);
	state.color_setting = "hud_color_inventory";
	state.x_setting = "inventory_hud_x";
	state.y_setting = "inventory_hud_y";
	ImGuiHud::get().updateInventoryHud(state);
}

void Hud::drawCraftHud()
{
	// Ground-up rewrite -- see drawInventoryHud() above and the class
	// comment on ImGuiHud for the full story; InventoryHud/CraftHud
	// share gatherItemCell()/gatherItemGridSection() above since they're
	// otherwise identical "grid of item sections" layouts.
	if (!g_settings->getBool("craft_hud")) {
		ImGuiHud::get().updateCraftHud(ImGuiHud::ItemGridState());
		return;
	}

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
	if (!listHasAnyItem(craftlist) && !listHasAnyItem(resultlist)) {
		ImGuiHud::get().updateCraftHud(ImGuiHud::ItemGridState());
		return;
	}

	ImGuiHud::ItemGridState state;
	gatherItemGridSection(state.sections, "Craft", craftlist, client, tsrc);
	gatherItemGridSection(state.sections, "Result", resultlist, client, tsrc);

	if (state.sections.empty()) {
		ImGuiHud::get().updateCraftHud(ImGuiHud::ItemGridState());
		return;
	}

	state.visible = true;
	// See "craft_hud_size" in src/gui/custom_menu/ImGuiMineBoostMenu.cpp
	// -- scroll over this HUD in "Move HUD" edit mode to resize it
	// independently of the native hotbar/inventory scaling.
	state.hud_size = rangelim(g_settings->getFloat("hud_size"), 0.5f, 2.5f)
		* rangelim(g_settings->getFloat("craft_hud_size"), 0.5f, 2.5f);
	state.color_setting = "hud_color_craft";
	state.x_setting = "craft_hud_x";
	state.y_setting = "craft_hud_y";
	// Default position: to the right of screen center, so it doesn't
	// overlap InventoryHud's default (centered) position -- see
	// renderItemGrid() in src/gui/imgui_hud.cpp.
	state.default_x_offset = 80.0f;
	ImGuiHud::get().updateCraftHud(state);
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
