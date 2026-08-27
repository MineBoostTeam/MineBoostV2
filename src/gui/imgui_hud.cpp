// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// MineBoostV2 -- implementation. See imgui_hud.h for the design notes
// (particularly the two-phase update()/render() split) this was built
// around.

#include "imgui_hud.h"

#include "imgui.h"
#include "ITexture.h"
#include "porting.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/string.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Same "hud_color_*" -> SColor resolution Hud::getHudColorSetting() (see
// src/client/hud.cpp) does, translated to ImGui's ImU32 packed-color
// format -- kept as its own local copy rather than exported from
// hud.cpp/shared, same reasoning as every other ImGui module in this
// project (PhotoHudPanel, ImGuiMineBoostMenu, ...): a handful of lines,
// not worth coupling files over.
ImU32 hudColor(const std::string &setting, int alpha)
{
	v3f c = g_settings->getV3F(setting).value_or(v3f(255.0f, 255.0f, 255.0f));
	int r = rangelim((int)myround(c.X), 0, 255);
	int g = rangelim((int)myround(c.Y), 0, 255);
	int b = rangelim((int)myround(c.Z), 0, 255);
	return IM_COL32(r, g, b, rangelim(alpha, 0, 255));
}

// Fill + border panel behind a HUD element, both driven by the same
// "hud_color_*" setting -- same convention as Hud::drawHudColorPanel().
// No drop shadow, same reasoning as that function's own comment on why
// (ModernUI::dropShadow()'s rounded corners look wrong on boxes this
// small).
void drawPanel(ImDrawList *dl, const ImVec2 &min, const ImVec2 &max,
		const std::string &color_setting, int fill_alpha = 190)
{
	dl->AddRectFilled(min, max, hudColor(color_setting, fill_alpha), 6.0f);
	dl->AddRect(min, max, hudColor(color_setting, 255), 6.0f, 0, 1.5f);
}

// Same continuous seamless-loop scrolling algorithm as the Irrlicht
// drawMarqueeLine() this replaces (see src/client/hud.cpp) -- same
// speed/gap constants too, so a track name scrolls at the same visual
// pace whether MusicHud is showing via this or (for anything not yet
// converted) the old code.
void drawMarqueeText(ImDrawList *dl, ImFont *font, float font_size,
		const std::string &text, const ImVec2 &pos, float avail_w, ImU32 color)
{
	if (text.empty())
		return;

	ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
	if (text_size.x <= avail_w) {
		dl->AddText(font, font_size, pos, color, text.c_str());
		return;
	}

	constexpr float gap_px = 40.0f;
	constexpr float px_per_sec = 30.0f;
	float cycle = text_size.x + gap_px;
	unsigned long long now_ms = porting::getTimeMs();
	float offset = std::fmod((float)((now_ms * (unsigned long long)px_per_sec / 1000ULL) %
		(unsigned long long)std::max(1.0f, cycle)), cycle);

	ImVec4 clip_rect(pos.x, pos.y, pos.x + avail_w, pos.y + text_size.y + 2.0f);
	dl->PushClipRect(ImVec2(clip_rect.x, clip_rect.y), ImVec2(clip_rect.z, clip_rect.w), true);
	dl->AddText(font, font_size, ImVec2(pos.x - offset, pos.y), color, text.c_str());
	dl->AddText(font, font_size, ImVec2(pos.x - offset + cycle, pos.y), color, text.c_str());
	dl->PopClipRect();
}

// Small helper: a background-less, undecorated, non-interactive,
// non-nav ImGui "window" purely as a canvas at a fixed screen position
// -- every element in this file uses one of these rather than a normal
// titled window, since these are HUD overlays, not menus. Returns the
// window's draw list, ready to add primitives/text to directly.
ImDrawList *beginHudCanvas(const char *id, const ImVec2 &pos, const ImVec2 &size)
{
	ImGui::SetNextWindowPos(pos);
	ImGui::SetNextWindowSize(size);
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::Begin(id, nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
		ImGuiWindowFlags_NoNav);
	return ImGui::GetWindowDrawList();
}

void endHudCanvas()
{
	ImGui::End();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
}

std::string formatMinSec(int total_seconds)
{
	total_seconds = std::max(0, total_seconds);
	int m = total_seconds / 60;
	int s = total_seconds % 60;
	char buf[16];
	porting::mt_snprintf(buf, sizeof(buf), "%d:%02d", m, s);
	return buf;
}

} // namespace

ImGuiHud &ImGuiHud::get()
{
	static ImGuiHud instance;
	return instance;
}

void ImGuiHud::render()
{
	renderSimpleTextHud("##mb_coords_hud", m_coords);
	renderSimpleTextHud("##mb_fps_hud", m_fps);
	renderSimpleTextHud("##mb_ping_hud", m_ping);
	renderKeyStrokerHud();
	renderCpsHud();
	renderMusicHud();
	renderShowRp();
	renderConsumptionHud();
	renderTargetHud();
	renderItemGrid("##mb_inventory_hud", m_inventory);
	renderItemGrid("##mb_craft_hud", m_craft);
}

void *ImGuiHud::getIconTextureId(irr::video::ITexture *tex, int *out_w, int *out_h)
{
	if (!tex)
		return nullptr;
	return m_icon_cache[tex].get(tex, out_w, out_h);
}

void ImGuiHud::renderConsumptionHud()
{
	if (!m_consumption.visible || m_consumption.text.empty())
		return;

	ImGuiIO &io = ImGui::GetIO();
	float font_size = ImGui::GetFontSize() * rangelim(m_consumption.hud_size, 0.5f, 2.5f);
	std::string text = wide_to_utf8(m_consumption.text);
	// Fixed sample text, same box-size-never-depends-on-live-content
	// reasoning as the old code (see its own comment in
	// src/client/hud.cpp) -- keeps this in sync with the "Move HUD"
	// drag-preview box, which always shows this exact sample.
	ImVec2 text_size = ImGui::GetFont()->CalcTextSizeA(
		font_size, FLT_MAX, 0.0f, "RAM: 9999 MB  CPU: 100%  GPU: 100%");

	float pad = 8.0f * rangelim(m_consumption.hud_size, 0.5f, 2.5f);
	float box_w = text_size.x + pad * 2.0f;
	float box_h = text_size.y + pad * 2.0f;

	float pos_x = (float)g_settings->getS32("consumption_hud_x");
	float pos_y = (float)g_settings->getS32("consumption_hud_y");
	if (pos_x < 0)
		pos_x = 10.0f;

	ImDrawList *dl = beginHudCanvas("##mb_consumption_hud",
		ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_consumption");
	dl->AddText(ImGui::GetFont(), font_size, ImVec2(pos_x + pad, pos_y + pad),
		IM_COL32(220, 220, 220, 255), text.c_str());
	endHudCanvas();
	(void)io;
}

void ImGuiHud::renderShowRp()
{
	if (!m_rp.visible)
		return;

	float hud_size = rangelim(m_rp.hud_size, 0.5f, 2.5f);
	float font_size = ImGui::GetFontSize() * hud_size;
	ImFont *font = ImGui::GetFont();
	float pad = 8.0f * hud_size;
	float line_h = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "Ay").y;
	const int num_text_lines = 2; // fixed -- see the old code's own comment on this

	int art_w = 0, art_h = 0;
	void *art_id = m_rp.screenshot_texture ?
		m_rp_art_cache.get(m_rp.screenshot_texture, &art_w, &art_h) : nullptr;
	float art_size = art_id ? std::max(num_text_lines * line_h, 32.0f) : 0.0f;
	float art_gap = art_id ? pad : 0.0f;

	float content_h = std::max(art_size, num_text_lines * line_h);
	float text_area_w = line_h * 9.0f;
	float box_w = art_size + art_gap + text_area_w + pad * 2.0f;
	float box_h = pad + content_h + pad;

	float pos_x = (float)g_settings->getS32("rp_hud_x");
	float pos_y = (float)g_settings->getS32("rp_hud_y");
	if (pos_x < 0)
		pos_x = ImGui::GetIO().DisplaySize.x - box_w - 10.0f;

	ImDrawList *dl = beginHudCanvas("##mb_showrp_hud",
		ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_rp");

	float content_x = pos_x + pad;
	if (art_id) {
		dl->AddImage((ImTextureID)(intptr_t)art_id, ImVec2(content_x, pos_y + pad),
			ImVec2(content_x + art_size, pos_y + art_size + pad));
		content_x += art_size + art_gap;
	}

	float y = pos_y + pad;
	float text_avail_w = (pos_x + box_w - pad) - content_x;
	drawMarqueeText(dl, font, font_size, m_rp.title, ImVec2(content_x, y),
		text_avail_w, IM_COL32(220, 220, 220, 255));
	y += line_h;
	if (!m_rp.author.empty())
		drawMarqueeText(dl, font, font_size, "by " + m_rp.author, ImVec2(content_x, y),
			text_avail_w, IM_COL32(170, 170, 170, 255));

	endHudCanvas();
}

void ImGuiHud::renderMusicHud()
{
	if (!m_music.visible || m_music.title.empty())
		return;

	float hud_size = rangelim(m_music.hud_size, 0.5f, 2.5f);
	float font_size = ImGui::GetFontSize() * hud_size;
	ImFont *font = ImGui::GetFont();
	float pad = 8.0f * hud_size;
	float line_h = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "Ay").y;

	std::string line1 = m_music.source.empty() ? "" : ("[" + m_music.source + "]");
	int num_text_lines = 1 + (line1.empty() ? 0 : 1) + (m_music.artist.empty() ? 0 : 1);

	int art_w = 0, art_h = 0;
	void *art_id = m_music.art_texture ?
		m_music_art_cache.get(m_music.art_texture, &art_w, &art_h) : nullptr;
	float art_size = art_id ? std::max((float)num_text_lines * line_h, 32.0f) : 0.0f;
	float art_gap = art_id ? pad : 0.0f;

	bool show_progress = m_music.has_progress && m_music.duration_seconds > 0;
	float bar_h = 4.0f;
	float progress_block_h = show_progress ? (pad / 2.0f + bar_h + 2.0f + line_h) : 0.0f;

	float content_h = std::max(art_size, (float)num_text_lines * line_h);
	float text_area_w = line_h * 9.0f;
	float box_w = art_size + art_gap + text_area_w + pad * 2.0f;
	float box_h = pad + content_h + progress_block_h + pad;

	float pos_x = (float)g_settings->getS32("music_hud_x");
	float pos_y = (float)g_settings->getS32("music_hud_y");
	if (pos_x < 0)
		pos_x = ImGui::GetIO().DisplaySize.x - box_w - 10.0f;

	ImDrawList *dl = beginHudCanvas("##mb_music_hud",
		ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_music");

	ImU32 source_color = IM_COL32(200, 200, 200, 255);
	if (m_music.source == "Spotify") source_color = IM_COL32(30, 215, 96, 255);
	else if (m_music.source == "YouTube Music") source_color = IM_COL32(255, 0, 0, 255);
	else if (m_music.source == "SoundCloud") source_color = IM_COL32(255, 119, 0, 255);
	else if (m_music.source == "Yandex Music") source_color = IM_COL32(255, 204, 0, 255);
	ImU32 artist_color = IM_COL32(200, 200, 200, 255);

	float content_x = pos_x + pad;
	if (art_id) {
		dl->AddImage((ImTextureID)(intptr_t)art_id, ImVec2(content_x, pos_y + pad),
			ImVec2(content_x + art_size, pos_y + art_size + pad));
		content_x += art_size + art_gap;
	}

	float y = pos_y + pad;
	float text_avail_w = (pos_x + box_w - pad) - content_x;
	if (!line1.empty()) {
		drawMarqueeText(dl, font, font_size, line1, ImVec2(content_x, y), text_avail_w, source_color);
		y += line_h;
	}
	drawMarqueeText(dl, font, font_size, m_music.title, ImVec2(content_x, y), text_avail_w, source_color);
	y += line_h;
	if (!m_music.artist.empty())
		drawMarqueeText(dl, font, font_size, m_music.artist, ImVec2(content_x, y), text_avail_w, artist_color);

	if (show_progress) {
		float bar_y = pos_y + pad + content_h + pad / 2.0f;
		float bar_x = pos_x + pad;
		float bar_w = box_w - pad * 2.0f;

		dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
			IM_COL32(60, 60, 60, 200), bar_h / 2.0f);
		float ratio = rangelim((float)m_music.position_seconds / (float)m_music.duration_seconds, 0.0f, 1.0f);
		if (ratio > 0.0f)
			dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w * ratio, bar_y + bar_h),
				source_color, bar_h / 2.0f);

		float time_y = bar_y + bar_h + 2.0f;
		std::string pos_text = formatMinSec(m_music.position_seconds);
		std::string dur_text = formatMinSec(m_music.duration_seconds);
		float dur_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, dur_text.c_str()).x;
		dl->AddText(font, font_size, ImVec2(bar_x, time_y), IM_COL32(190, 190, 190, 255), pos_text.c_str());
		dl->AddText(font, font_size, ImVec2(bar_x + bar_w - dur_w, time_y),
			IM_COL32(190, 190, 190, 255), dur_text.c_str());
	}

	endHudCanvas();
}

void ImGuiHud::renderTargetHud()
{
	if (!m_target.visible)
		return;

	float hud_size = rangelim(m_target.hud_size, 0.5f, 2.5f);
	float font_size = ImGui::GetFontSize() * hud_size;
	ImFont *font = ImGui::GetFont();

	float bar_w = 160.0f * hud_size;
	float bar_h = 10.0f * hud_size;
	float pad = 6.0f * hud_size;
	float line_h = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "Ay").y;

	int avatar_w = 0, avatar_h = 0;
	void *avatar_id = m_target.avatar_texture ?
		getIconTextureId(m_target.avatar_texture, &avatar_w, &avatar_h) : nullptr;
	float avatar_size = line_h + bar_h + pad;
	float avatar_col_w = avatar_id ? avatar_size + pad : 0.0f;

	std::string name = m_target.name;
	float name_w = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, name.c_str()).x;
	float text_col_w = std::max(bar_w, name_w);
	float box_w = avatar_col_w + text_col_w + pad * 2.0f;
	float box_h = line_h + bar_h + pad * 3.0f;

	ImGuiIO &io = ImGui::GetIO();
	float pos_x = (float)g_settings->getS32("target_hud_x");
	float pos_y = (float)g_settings->getS32("target_hud_y");
	if (pos_x < 0)
		pos_x = (io.DisplaySize.x - box_w) / 2.0f;
	if (pos_y < 0)
		pos_y = io.DisplaySize.y * 0.16f;

	ImDrawList *dl = beginHudCanvas("##mb_target_hud", ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_target");

	float content_x = pos_x + pad;
	if (avatar_id) {
		dl->AddImage((ImTextureID)(intptr_t)avatar_id, ImVec2(content_x, pos_y + pad),
			ImVec2(content_x + avatar_size, pos_y + pad + avatar_size),
			ImVec2(m_target.avatar_uv0_x, m_target.avatar_uv0_y),
			ImVec2(m_target.avatar_uv1_x, m_target.avatar_uv1_y));
		if (m_target.avatar_has_overlay)
			dl->AddImage((ImTextureID)(intptr_t)avatar_id, ImVec2(content_x, pos_y + pad),
				ImVec2(content_x + avatar_size, pos_y + pad + avatar_size),
				ImVec2(m_target.overlay_uv0_x, m_target.overlay_uv0_y),
				ImVec2(m_target.overlay_uv1_x, m_target.overlay_uv1_y));
		content_x += avatar_size + pad;
	}

	dl->AddText(font, font_size, ImVec2(content_x, pos_y + pad),
		IM_COL32(255, 255, 255, 255), name.c_str());

	float bar_x = content_x + (text_col_w - bar_w) / 2.0f;
	float bar_y = pos_y + pad + line_h + pad;
	dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
		IM_COL32(40, 40, 40, 200), bar_h / 2.0f);

	float ratio = m_target.hp_max > 0 ? (float)m_target.hp / (float)m_target.hp_max : 0.0f;
	ratio = rangelim(ratio, 0.0f, 1.0f);
	ImU32 hp_color = ratio > 0.5f ? IM_COL32(60, 200, 60, 255) :
		ratio > 0.25f ? IM_COL32(230, 200, 40, 255) : IM_COL32(220, 50, 50, 255);
	if (ratio > 0.0f)
		dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w * ratio, bar_y + bar_h),
			hp_color, bar_h / 2.0f);

	endHudCanvas();
}

void ImGuiHud::renderItemGrid(const char *canvas_id, const ItemGridState &state)
{
	if (!state.visible || state.sections.empty())
		return;

	float hud_size = rangelim(state.hud_size, 0.5f, 2.5f);
	float font_size = ImGui::GetFontSize() * hud_size;
	ImFont *font = ImGui::GetFont();
	float pad = 8.0f * hud_size;
	float line_h = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "Ay").y;
	float title_h = line_h + pad;
	// Slot size follows the same "ImGui::GetFontSize() scales with
	// hud_size" idea the rest of this file uses, rather than reaching
	// into Hud's own m_hotbar_imagesize (a vanilla-Luanti hotbar concept
	// this class deliberately has no dependency on) -- visually close
	// enough to the old fixed hotbar-derived size to feel consistent,
	// without coupling this file to Hud's internals.
	float slot = font_size * 2.4f;
	float slot_pad = 4.0f * hud_size;
	float cell = slot + slot_pad * 2.0f;

	int max_cols = 0;
	float box_h = pad * 2.0f;
	for (const auto &sec : state.sections) {
		max_cols = std::max(max_cols, sec.cols);
		box_h += title_h + sec.rows * cell;
	}
	float box_w = max_cols * cell + pad * 2.0f;

	ImGuiIO &io = ImGui::GetIO();
	float pos_x = (float)g_settings->getS32(state.x_setting);
	float pos_y = (float)g_settings->getS32(state.y_setting);
	if (pos_x < 0)
		pos_x = (io.DisplaySize.x - box_w) / 2.0f + state.default_x_offset;
	if (pos_y < 0)
		pos_y = (io.DisplaySize.y - box_h) / 2.0f;

	ImDrawList *dl = beginHudCanvas(canvas_id, ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), state.color_setting);

	float section_y = pos_y;
	for (const auto &sec : state.sections) {
		dl->AddText(font, font_size, ImVec2(pos_x + pad, section_y + pad / 2.0f),
			IM_COL32(255, 255, 255, 255), sec.title.c_str());

		for (size_t i = 0; i < sec.cells.size(); ++i) {
			int col = (int)i % sec.cols;
			int row = (int)i / sec.cols;
			float cx = pos_x + pad + slot_pad + col * cell;
			float cy = section_y + title_h + pad + slot_pad + row * cell;

			dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + slot, cy + slot),
				IM_COL32(40, 42, 52, 100), 4.0f);
			dl->AddRect(ImVec2(cx, cy), ImVec2(cx + slot, cy + slot),
				IM_COL32(120, 150, 220, 150), 4.0f);

			const auto &c = sec.cells[i];
			int icon_w = 0, icon_h = 0;
			void *icon_id = c.icon_texture ? getIconTextureId(c.icon_texture, &icon_w, &icon_h) : nullptr;
			if (icon_id) {
				dl->AddImage((ImTextureID)(intptr_t)icon_id, ImVec2(cx, cy), ImVec2(cx + slot, cy + slot));
				if (c.overlay_texture) {
					int ow = 0, oh = 0;
					if (void *overlay_id = getIconTextureId(c.overlay_texture, &ow, &oh))
						dl->AddImage((ImTextureID)(intptr_t)overlay_id, ImVec2(cx, cy), ImVec2(cx + slot, cy + slot));
				}
			}

			if (c.has_wear) {
				float bar_h2 = std::max(2.0f, slot / 16.0f);
				float bar_pad = slot / 16.0f;
				float wear_x0 = cx + bar_pad;
				float wear_x1 = cx + slot - bar_pad;
				float wear_y = cy + slot - bar_pad - bar_h2;
				float mid = wear_x0 + (1.0f - c.wear_fraction) * (wear_x1 - wear_x0);
				dl->AddRectFilled(ImVec2(wear_x0, wear_y), ImVec2(mid, wear_y + bar_h2), c.wear_color);
				dl->AddRectFilled(ImVec2(mid, wear_y), ImVec2(wear_x1, wear_y + bar_h2), IM_COL32(0, 0, 0, 255));
			}

			if (!c.count_text.empty()) {
				float count_font = font_size * 0.85f;
				ImVec2 tsz = font->CalcTextSizeA(count_font, FLT_MAX, 0.0f, c.count_text.c_str());
				dl->AddText(font, count_font, ImVec2(cx + slot - tsz.x - 2.0f, cy + slot - tsz.y - 1.0f),
					IM_COL32(255, 255, 255, 255), c.count_text.c_str());
			}
		}

		section_y += title_h + sec.rows * cell;
	}

	endHudCanvas();
}

void ImGuiHud::renderSimpleTextHud(const char *canvas_id, const SimpleTextHudState &state)
{
	if (!state.visible || state.text.empty())
		return;

	float font_size = ImGui::GetFontSize() * rangelim(state.hud_size, 0.5f, 2.5f);
	ImFont *font = ImGui::GetFont();
	ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, state.text.c_str());

	float pad = 8.0f * rangelim(state.hud_size, 0.5f, 2.5f);
	float box_w = text_size.x + pad * 2.0f;
	float box_h = text_size.y + pad * 2.0f;

	ImGuiIO &io = ImGui::GetIO();
	float pos_x, pos_y;
	if (state.combined_v2f) {
		v2f pos(-1.0f, -1.0f);
		g_settings->getV2FNoEx(state.pos_setting, pos);
		pos_x = pos.X;
		pos_y = pos.Y;
	} else {
		pos_x = (float)g_settings->getS32(state.x_setting);
		pos_y = (float)g_settings->getS32(state.y_setting);
	}
	if (pos_x < 0) {
		pos_x = 5.0f;
		pos_y = io.DisplaySize.y - state.default_y_offset - text_size.y - pad;
	}

	ImDrawList *dl = beginHudCanvas(canvas_id, ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), state.color_setting);
	dl->AddText(font, font_size, ImVec2(pos_x + pad, pos_y + pad),
		IM_COL32(220, 220, 220, 255), state.text.c_str());
	endHudCanvas();
}

namespace {

// One key-cap in the KeyStroker grid -- column/row are in cell units
// (see renderKeyStrokerHud() below for the grid layout this describes).
// This is MineBoostV2's own clean grid, not a pixel-for-pixel
// reproduction of the old baked key_*.png icon positions (see the
// comment on this in renderKeyStrokerHud()).
struct KeyCap { const char *label; float col; float row; bool ImGuiHud::KeyStrokerState::*pressed; };

constexpr KeyCap kKeyCaps[] = {
	{"E",     1.0f, 0.0f, &ImGuiHud::KeyStrokerState::aux1},
	{"W",     0.0f, 1.0f, &ImGuiHud::KeyStrokerState::up},
	{"Shift", -1.0f, 1.0f, &ImGuiHud::KeyStrokerState::sneak},
	{"A",     -1.0f, 2.0f, &ImGuiHud::KeyStrokerState::left},
	{"S",     0.0f, 2.0f, &ImGuiHud::KeyStrokerState::down},
	{"D",     1.0f, 2.0f, &ImGuiHud::KeyStrokerState::right},
	{"LMB",   -1.0f, 3.0f, &ImGuiHud::KeyStrokerState::dig},
	{"Space", 0.0f, 3.0f, &ImGuiHud::KeyStrokerState::jump},
	{"RMB",   1.0f, 3.0f, &ImGuiHud::KeyStrokerState::place},
};

} // namespace

void ImGuiHud::renderKeyStrokerHud()
{
	if (!m_keystroker.visible)
		return;

	float hud_size = rangelim(m_keystroker.hud_size, 0.5f, 2.5f);
	float cell = 34.0f * hud_size;
	float key_size = cell - 4.0f * hud_size;
	float pad = 8.0f * hud_size;

	float box_w = 3.0f * cell + pad * 2.0f;
	float box_h = 4.0f * cell + pad * 2.0f;

	float pos_x = (float)g_settings->getS32("keys_x");
	float pos_y = (float)g_settings->getS32("keys_y");
	// -1 = never positioned yet (same sentinel drawHudEditOverlay() uses,
	// see ImGuiMineBoostMenu.cpp) -- without this, a fresh install draws
	// the panel at (-1,-1), effectively off-screen, until the player
	// happens to drag it once via Move HUD.
	if (pos_x < 0 || pos_y < 0) {
		pos_x = 10.0f;
		pos_y = ImGui::GetIO().DisplaySize.y - box_h - 10.0f;
	}

	ImDrawList *dl = beginHudCanvas("##mb_keystroker_hud", ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_keystroker_border");

	ImFont *font = ImGui::GetFont();
	float font_size = ImGui::GetFontSize() * hud_size * 0.8f;
	float origin_x = pos_x + pad + cell; // column 0 sits one cell in, so column -1 (Shift/A/LMB) fits
	float origin_y = pos_y + pad;

	for (const KeyCap &kc : kKeyCaps) {
		bool pressed = m_keystroker.*kc.pressed;
		float x = origin_x + kc.col * cell;
		float y = origin_y + kc.row * cell;
		ImU32 fill = pressed ? IM_COL32(90, 150, 250, 230) : IM_COL32(50, 52, 62, 180);
		ImU32 border = pressed ? IM_COL32(160, 200, 255, 255) : IM_COL32(120, 120, 130, 150);
		dl->AddRectFilled(ImVec2(x, y), ImVec2(x + key_size, y + key_size), fill, 4.0f);
		dl->AddRect(ImVec2(x, y), ImVec2(x + key_size, y + key_size), border, 4.0f, 0, 1.5f);

		ImVec2 tsz = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, kc.label);
		dl->AddText(font, font_size,
			ImVec2(x + (key_size - tsz.x) / 2.0f, y + (key_size - tsz.y) / 2.0f),
			IM_COL32(255, 255, 255, 255), kc.label);
	}

	endHudCanvas();
}

void ImGuiHud::renderCpsHud()
{
	if (!m_cps.visible)
		return;

	float hud_size = rangelim(m_cps.hud_size, 0.5f, 2.5f);
	float font_size = ImGui::GetFontSize() * hud_size;
	ImFont *font = ImGui::GetFont();
	float pad = 8.0f * hud_size;
	float line_h = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, "Ay").y;

	std::string lmb_text = "LMB CPS: " + std::to_string(m_cps.lmb_cps);
	std::string rmb_text = "RMB CPS: " + std::to_string(m_cps.rmb_cps);
	float text_w = std::max(
		font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, lmb_text.c_str()).x,
		font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, rmb_text.c_str()).x);

	float box_w = text_w + pad * 2.0f;
	float box_h = line_h * 2.0f + pad * 2.0f;

	float pos_x = (float)g_settings->getS32("cps_x");
	float pos_y = (float)g_settings->getS32("cps_y");
	// -1 = never positioned yet -- see the identical comment in
	// renderKeyStrokerHud() above.
	if (pos_x < 0 || pos_y < 0) {
		pos_x = ImGui::GetIO().DisplaySize.x - box_w - 10.0f;
		pos_y = ImGui::GetIO().DisplaySize.y - box_h - 10.0f;
	}

	ImDrawList *dl = beginHudCanvas("##mb_cps_hud", ImVec2(pos_x, pos_y), ImVec2(box_w, box_h));
	drawPanel(dl, ImVec2(pos_x, pos_y), ImVec2(pos_x + box_w, pos_y + box_h), "hud_color_cps_border");
	dl->AddText(font, font_size, ImVec2(pos_x + pad, pos_y + pad), IM_COL32(255, 255, 255, 255), lmb_text.c_str());
	dl->AddText(font, font_size, ImVec2(pos_x + pad, pos_y + pad + line_h), IM_COL32(255, 255, 255, 255), rmb_text.c_str());
	endHudCanvas();
}
