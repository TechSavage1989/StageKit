/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "../shadow/shadow-backdrop.hpp"
#include "morph-paint.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/tooltip.hpp"

#include <graphics/vec4.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace stagekit {

Quad between(const Quad &p, const Quad &q, double t)
{
	auto mix = [t](double x, double y) {
		return x + (y - x) * t;
	};
	return Quad{mix(p.a, q.a), mix(p.b, q.b), mix(p.c, q.c), mix(p.d, q.d), mix(p.tx, q.tx), mix(p.ty, q.ty)};
}

bool shown_of(obs_sceneitem_t *item, Shown &out)
{
	out.source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!out.source)
		return false;
	out.sw = obs_source_get_width(out.source);
	out.sh = obs_source_get_height(out.source);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	out.crop = Crop{(double)crop.left, (double)crop.top, (double)crop.right, (double)crop.bottom};
	const double cw = out.sw - out.crop.l - out.crop.r, ch = out.sh - out.crop.t - out.crop.b;
	if (cw < 1.0 || ch < 1.0)
		return false;
	// The scene's own sum for the item: from the pixels of the cropped
	// source to the screen, bounding box, alignment and rotation included.
	matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	out.quad = Quad{cw * m.x.x, cw * m.x.y, ch * m.y.x, ch * m.y.y, m.t.x, m.t.y};
	return true;
}

Bounds bounds_of(const Quad &q)
{
	const double xs[4] = {q.tx, q.tx + q.a, q.tx + q.c, q.tx + q.a + q.c};
	const double ys[4] = {q.ty, q.ty + q.b, q.ty + q.d, q.ty + q.b + q.d};
	return Bounds{*std::min_element(xs, xs + 4), *std::min_element(ys, ys + 4), *std::max_element(xs, xs + 4),
		      *std::max_element(ys, ys + 4)};
}

void RoomCentre::add(const Bounds &b)
{
	const double area = std::max(0.0, b.x1 - b.x0) * std::max(0.0, b.y1 - b.y0);
	x += area * (b.x0 + b.x1) / 2.0;
	y += area * (b.y0 + b.y1) / 2.0;
	weight += area;
}

RoomEdge room_edge(const Bounds &b, double w, double h, const RoomCentre *from)
{
	// How far each way until it is all off the screen.
	const double far[4] = {b.x1, w - b.x0, b.y1, h - b.y0};
	bool away[4] = {true, true, true, true};
	if (from && from->any()) {
		const double dx = (b.x0 + b.x1) / 2.0 - from->cx(), dy = (b.y0 + b.y1) / 2.0 - from->cy();
		const double len = std::hypot(dx, dy);
		// (Under a hundredth of the screen's diagonal: no way to speak of.
		// Each way must hold more than a fiftieth of the line, so a source
		// a pixel off straight above goes up, not sideways.)
		if (len > 0.01 * std::hypot(w, h)) {
			const double hair = 0.02 * len;
			away[0] = -dx > hair;
			away[1] = dx > hair;
			away[2] = -dy > hair;
			away[3] = dy > hair;
		}
	}
	int best = -1;
	for (int i = 0; i < 4; i++)
		if (away[i] && (best < 0 || far[i] < far[best]))
			best = i;
	return static_cast<RoomEdge>(best < 0 ? 0 : best);
}

Quad room_moved(const Quad &q, const Bounds &b, RoomEdge edge, double gone, double w, double h)
{
	Quad out = q;
	switch (edge) {
	case RoomEdge::Left:
		out.tx -= b.x1 * gone;
		break;
	case RoomEdge::Right:
		out.tx += (w - b.x0) * gone;
		break;
	case RoomEdge::Up:
		out.ty -= b.y1 * gone;
		break;
	case RoomEdge::Down:
		out.ty += (h - b.y0) * gone;
		break;
	}
	return out;
}

MorphLinks morph_links_read(obs_data_t *settings, const char *setting)
{
	MorphLinks links;
	for (const std::string &line : pick_list_read(settings, setting, false)) {
		const size_t mark = line.find(kLinkMark);
		if (mark != std::string::npos && mark > 0 && mark + strlen(kLinkMark) < line.size())
			links.emplace_back(line.substr(0, mark), line.substr(mark + strlen(kLinkMark)));
	}
	return links;
}

bool morph_linked(const MorphLinks &links, const char *a, const char *b)
{
	if (!a || !b || links.empty())
		return false;
	std::vector<std::string> reached{a};
	for (size_t at = 0; at < reached.size(); at++) {
		for (const auto &link : links) {
			const std::string *other = link.first == reached[at]    ? &link.second
						   : link.second == reached[at] ? &link.first
										: nullptr;
			if (!other || std::find(reached.begin(), reached.end(), *other) != reached.end())
				continue;
			if (*other == b)
				return true;
			reached.push_back(*other);
		}
	}
	return false;
}

namespace {

// Every source and scene there is, for the two boxes a link is made from.
void add_sources(obs_property_t *box)
{
	std::vector<std::string> names;
	const auto add = [](void *param, obs_source_t *source) {
		const char *name = obs_source_get_name(source);
		if (name && *name && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
			static_cast<std::vector<std::string> *>(param)->push_back(name);
		return true;
	};
	obs_enum_sources(add, &names);
	obs_enum_scenes(add, &names);
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
	obs_property_list_add_string(box, obs_module_text("List.Choose"), "");
	for (const std::string &name : names)
		obs_property_list_add_string(box, name.c_str(), name.c_str());
}

// "Add this link": the two sources chosen become one line of the list. The
// button is named `prefix` + "link_add", which is where the prefix is from.
bool add_link(obs_properties_t *props, obs_property_t *button, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	const char *button_name = obs_property_name(button);
	const size_t tail = strlen("link_add");
	if (!owner || !button_name || strlen(button_name) < tail)
		return false;
	const std::string prefix(button_name, strlen(button_name) - tail);
	const std::string name_a = prefix + "link_a", name_b = prefix + "link_b", name_list = prefix + "links";
	obs_data_t *settings = obs_source_get_settings(owner);
	const std::string a = obs_data_get_string(settings, name_a.c_str()),
			  b = obs_data_get_string(settings, name_b.c_str());
	bool added = false;
	if (!a.empty() && !b.empty() && a != b) {
		std::vector<std::string> lines = pick_list_read(settings, name_list.c_str(), false);
		const std::string line = a + kLinkMark + b, other_way = b + kLinkMark + a;
		if (std::find(lines.begin(), lines.end(), line) == lines.end() &&
		    std::find(lines.begin(), lines.end(), other_way) == lines.end()) {
			lines.push_back(line);
			pick_list_write(settings, name_list.c_str(), false, lines);
		}
		obs_data_set_string(settings, name_a.c_str(), "");
		obs_data_set_string(settings, name_b.c_str(), "");
		obs_source_update(owner, settings);
		added = true;
	}
	obs_data_release(settings);
	if (added)
		pick_list_refresh(props, owner, name_list.c_str(), false);
	return added;
}

} // namespace

void add_morph_link_properties(obs_properties_t *props, obs_source_t *owner, const char *prefix)
{
	const std::string name_a = std::string(prefix) + "link_a", name_b = std::string(prefix) + "link_b",
			  name_add = std::string(prefix) + "link_add", name_list = std::string(prefix) + "links";
	obs_property_t *link_a = obs_properties_add_list(props, name_a.c_str(), obs_module_text("Morph.LinkA"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_sources(link_a);
	set_tip(link_a, obs_module_text("Morph.Links.Tip"));
	obs_property_t *link_b = obs_properties_add_list(props, name_b.c_str(), obs_module_text("Morph.LinkB"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_sources(link_b);
	set_tip(link_b, obs_module_text("Morph.Links.Tip"));
	obs_properties_add_button2(props, name_add.c_str(), obs_module_text("Morph.LinkAdd"), add_link, owner);
	// (The list keeps its setting's name for as long as the window is open.)
	static std::set<std::string> names;
	PickList links;
	links.setting = names.insert(name_list).first->c_str();
	links.title = "Morph.Links";
	links.tip = "Morph.Links.Tip";
	pick_list_add(props, owner, links);
}

MorphPaint::MorphPaint()
{
	char *path = obs_module_file("effects/stagekit-morph.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Morph: failed to load effect '%s': %s. It will cut from scene to scene.",
			path ? path : "(missing)", errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_image2_ = gs_effect_get_param_by_name(effect_, "image2");
		p_opacity_ = gs_effect_get_param_by_name(effect_, "opacity");
		p_amount_ = gs_effect_get_param_by_name(effect_, "amount");
	}
	obs_leave_graphics();
	bfree(path);
	since_ = os_gettime_ns();
}

MorphPaint::~MorphPaint()
{
	obs_enter_graphics();
	gs_effect_destroy(effect_);
	for (gs_texrender_t *tr : spares_)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
}

Quad part_quad(const Shown &shown, const Crop &part)
{
	const double cw = shown.sw - shown.crop.l - shown.crop.r, ch = shown.sh - shown.crop.t - shown.crop.b;
	if (cw < 1.0 || ch < 1.0)
		return shown.quad;
	const double u0 = (part.l - shown.crop.l) / cw, v0 = (part.t - shown.crop.t) / ch;
	const double us = (shown.sw - part.l - part.r) / cw, vs = (shown.sh - part.t - part.b) / ch;
	const Quad &q = shown.quad;
	return Quad{q.a * us, q.b * us, q.c * vs, q.d * vs, q.tx + q.a * u0 + q.c * v0, q.ty + q.b * u0 + q.d * v0};
}

Crop without_room(const Shown &shown, const Crop &part)
{
	double room[4] = {0.0, 0.0, 0.0, 0.0};
	shadow_room(shown.source, room);
	Crop out{std::max(part.l, room[0]), std::max(part.t, room[1]), std::max(part.r, room[2]),
		 std::max(part.b, room[3])};
	// (Nothing left: then there is nothing but the effect, and it is the part.)
	if (shown.sw - out.l - out.r < 1.0 || shown.sh - out.t - out.b < 1.0)
		return part;
	return out;
}

void match_parts(const Shown &a, const Crop &match_a, const Crop &whole_a, const Shown &b, const Crop &match_b,
		 const Crop &whole_b, Crop &draw_a, Crop &draw_b)
{
	const double aw = std::max(1.0, a.sw - match_a.l - match_a.r), ah = std::max(1.0, a.sh - match_a.t - match_a.b);
	const double bw = std::max(1.0, b.sw - match_b.l - match_b.r), bh = std::max(1.0, b.sh - match_b.t - match_b.b);
	// How far each one's whole reaches past its matched part, as a share of
	// the matched part's size; the larger of the two on each side.
	const double l = std::max({0.0, (match_a.l - whole_a.l) / aw, (match_b.l - whole_b.l) / bw});
	const double t = std::max({0.0, (match_a.t - whole_a.t) / ah, (match_b.t - whole_b.t) / bh});
	const double r = std::max({0.0, (match_a.r - whole_a.r) / aw, (match_b.r - whole_b.r) / bw});
	const double bt = std::max({0.0, (match_a.b - whole_a.b) / ah, (match_b.b - whole_b.b) / bh});
	draw_a = Crop{match_a.l - l * aw, match_a.t - t * ah, match_a.r - r * aw, match_a.b - bt * ah};
	draw_b = Crop{match_b.l - l * bw, match_b.t - t * bh, match_b.r - r * bw, match_b.b - bt * bh};
}

void MorphPaint::forget_seen()
{
	seen_.clear();
}

Crop MorphPaint::seen_crop(obs_sceneitem_t *item, const Shown &shown)
{
	const auto found = seen_.find(item);
	if (found != seen_.end())
		return found->second;
	Crop out = shown.crop;
	const double cw = shown.sw - shown.crop.l - shown.crop.r, ch = shown.sh - shown.crop.t - shown.crop.b;
	if (cw >= 2.0 && ch >= 2.0) {
		// A small copy is enough: its edges are taken a pixel generously.
		const double k = std::min(1.0, 256.0 / std::max(cw, ch));
		const uint32_t w = (uint32_t)std::max(2.0, std::round(cw * k)),
			       h = (uint32_t)std::max(2.0, std::round(ch * k));
		gs_texrender_t *into = spare();
		gs_texrender_reset(into);
		gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
		if (stage && gs_texrender_begin(into, w, h)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho((float)shown.crop.l, (float)(shown.crop.l + cw), (float)shown.crop.t,
				 (float)(shown.crop.t + ch), -100.0f, 100.0f);
			obs_source_video_render(shown.source);
			gs_texrender_end(into);
			gs_stage_texture(stage, gs_texrender_get_texture(into));
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(stage, &data, &linesize)) {
				uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
				for (uint32_t y = 0; y < h; y++) {
					const uint8_t *row = data + (size_t)y * linesize;
					for (uint32_t x = 0; x < w; x++) {
						if (row[x * 4 + 3] <= 12) // all but invisible
							continue;
						x0 = std::min(x0, x);
						x1 = std::max(x1, x);
						y0 = std::min(y0, y);
						y1 = std::max(y1, y);
					}
				}
				gs_stagesurface_unmap(stage);
				if (x1 >= x0 && y1 >= y0) {
					out.l = shown.crop.l + (double)x0 / k;
					out.t = shown.crop.t + (double)y0 / k;
					out.r = shown.crop.r + std::max(0.0, cw - (double)(x1 + 1) / k);
					out.b = shown.crop.b + std::max(0.0, ch - (double)(y1 + 1) / k);
				}
			}
		}
		gs_stagesurface_destroy(stage);
	}
	seen_[item] = out;
	return out;
}

void MorphPaint::forget_sizes()
{
	seen_.clear();
	sized_.clear();
	since_ = os_gettime_ns();
}

// OBS works an item's place out when its scene is drawn, from the size its
// source has then, and while Morph draws the items itself neither scene is
// drawn. So it is worked out here: when the item is first met (its scene may
// never have been drawn) and again whenever its source changes size. A
// display capture is stopped while no scene shows it and has no size until
// it has started again, a moment into the change; its place in a bounding
// box, worked out from no size, is nowhere. Left at that, the owner's display
// capture was missing for the whole change and came in at a stroke when it
// ended.
bool MorphPaint::look(obs_sceneitem_t *item, Shown &out)
{
	obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!source)
		return false;
	const uint32_t w = obs_source_get_width(source), h = obs_source_get_height(source);
	auto found = sized_.find(item);
	if (found == sized_.end()) {
		refresh_item(item);
		sized_[item] = Sized{w, h, w && h};
	} else if (found->second.w != w || found->second.h != h) {
		refresh_item(item);
		if (!found->second.had && w && h) {
			// (For the log, should a capture still be seen to come in late.)
			obs_log(LOG_INFO, "Morph: '%s' had no picture for the first %d ms of the change",
				obs_source_get_name(source), (int)((os_gettime_ns() - since_) / 1000000ULL));
			found->second.had = true;
		}
		found->second.w = w;
		found->second.h = h;
	}
	return shown_of(item, out);
}

gs_texrender_t *MorphPaint::spare()
{
	if (spare_ >= spares_.size())
		spares_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	return spares_[spare_++];
}

// The source with `crop` cut off it, as a picture of its own.
gs_texture_t *MorphPaint::cropped(const Shown &shown, const Crop &crop)
{
	const double cw = shown.sw - crop.l - crop.r, ch = shown.sh - crop.t - crop.b;
	if (cw < 1.0 || ch < 1.0)
		return nullptr;
	gs_texrender_t *into = spare();
	gs_texrender_reset(into);
	if (!gs_texrender_begin_with_color_space(into, (uint32_t)std::lround(cw), (uint32_t)std::lround(ch),
						 GS_CS_SRGB))
		return nullptr;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho((float)crop.l, (float)(crop.l + cw), (float)crop.t, (float)(crop.t + ch), -100.0f, 100.0f);
	obs_source_video_render(shown.source);
	gs_texrender_end(into);
	return gs_texrender_get_texture(into);
}

void set_quad(const Quad &q)
{
	matrix4 m;
	vec4_set(&m.x, (float)q.a, (float)q.b, 0.0f, 0.0f);
	vec4_set(&m.y, (float)q.c, (float)q.d, 0.0f, 0.0f);
	vec4_set(&m.z, 0.0f, 0.0f, 1.0f, 0.0f);
	vec4_set(&m.t, (float)q.tx, (float)q.ty, 0.0f, 1.0f);
	gs_matrix_mul(&m);
}

// One picture, or two mixed (`amount` of the second), laid onto a quad.
void MorphPaint::draw_texture(gs_texture_t *first, gs_texture_t *second, const Quad &quad, float amount, float opacity)
{
	if (!effect_ || !first)
		return;
	const bool linear = gs_get_linear_srgb();
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	auto set = [linear](gs_eparam_t *param, gs_texture_t *texture) {
		if (linear)
			gs_effect_set_texture_srgb(param, texture);
		else
			gs_effect_set_texture(param, texture);
	};
	set(p_image_, first);
	if (second)
		set(p_image2_, second);
	gs_effect_set_float(p_opacity_, opacity);
	gs_effect_set_float(p_amount_, amount);
	gs_matrix_push();
	set_quad(quad);
	while (gs_effect_loop(effect_, second ? "Mix" : "Draw"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

// An item's source on a quad, with a crop: straight onto what is being drawn
// when nothing is cut off, as a scene draws it, and by way of a picture of
// its own when something is.
void MorphPaint::draw_item(const Shown &shown, const Quad &quad, const Crop &crop)
{
	if (!crop.any()) {
		gs_matrix_push();
		set_quad(quad);
		gs_matrix_scale3f((float)(1.0 / shown.sw), (float)(1.0 / shown.sh), 1.0f);
		obs_source_video_render(shown.source);
		gs_matrix_pop();
		return;
	}
	if (gs_texture_t *texture = cropped(shown, crop))
		draw_texture(texture, nullptr, quad, 0.0f);
}

void MorphPaint::draw_faded(gs_texture_t *texture, uint32_t cx, uint32_t cy, float opacity, double zoom)
{
	if (!effect_ || !texture)
		return;
	const bool was_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(p_image_, texture);
	gs_effect_set_float(p_opacity_, opacity);
	gs_matrix_push();
	// Round its middle.
	gs_matrix_translate3f((float)(cx * (1.0 - zoom) / 2.0), (float)(cy * (1.0 - zoom) / 2.0), 0.0f);
	gs_matrix_scale3f((float)zoom, (float)zoom, 1.0f);
	while (gs_effect_loop(effect_, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(was_srgb);
}

} // namespace stagekit
