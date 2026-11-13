/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "arrange.hpp"

#include "../common/shown-scale.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace stagekit {

namespace {

// The part of a source that has something drawn in it, in the source's own
// pixels. Drawn small and read back once; the caller asked for this, so it
// is not something that runs every frame.
struct Drawn {
	double x = 0, y = 0, w = 0, h = 0;
	bool any = false;
};

// `bare`: the source drawn without any of its filters.
Drawn drawn_part(obs_source_t *source, bool bare = false)
{
	Drawn out;
	const uint32_t sw = bare ? obs_source_get_base_width(source) : obs_source_get_width(source);
	const uint32_t sh = bare ? obs_source_get_base_height(source) : obs_source_get_height(source);
	if (!sw || !sh)
		return out;
	// Edges are found to within a source pixel up to 1024 wide; beyond that
	// to within a couple of pixels, which the full-size box then rounds.
	const double shrink = std::min(1.0, 1024.0 / std::max(sw, sh));
	const uint32_t w = std::max<uint32_t>(1, (uint32_t)std::lround(sw * shrink));
	const uint32_t h = std::max<uint32_t>(1, (uint32_t)std::lround(sh * shrink));

	obs_enter_graphics();
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	uint32_t min_x = w, min_y = h, max_x = 0, max_y = 0;
	if (gs_texrender_begin(render, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (bare)
			obs_source_default_render(source);
		else
			obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(render);
		gs_stage_texture(stage, gs_texrender_get_texture(render));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			for (uint32_t y = 0; y < h; y++) {
				const uint8_t *row = data + (size_t)y * linesize;
				for (uint32_t x = 0; x < w; x++) {
					if (row[x * 4 + 3] <= 12) // all but invisible
						continue;
					min_x = std::min(min_x, x);
					max_x = std::max(max_x, x);
					min_y = std::min(min_y, y);
					max_y = std::max(max_y, y);
				}
			}
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
	obs_leave_graphics();

	if (max_x < min_x || max_y < min_y)
		return out; // nothing drawn at all
	out.any = true;
	out.x = min_x / shrink;
	out.y = min_y / shrink;
	out.w = (max_x + 1 - min_x) / shrink;
	out.h = (max_y + 1 - min_y) / shrink;
	return out;
}

double edge_of(const Box &b, Edge edge)
{
	switch (edge) {
	case Edge::Left:
		return b.x;
	case Edge::Center:
		return b.x + b.w / 2.0;
	case Edge::Right:
		return b.x + b.w;
	case Edge::Top:
		return b.y;
	case Edge::Middle:
		return b.y + b.h / 2.0;
	default:
		return b.y + b.h;
	}
}

bool across(Edge edge)
{
	return edge == Edge::Left || edge == Edge::Center || edge == Edge::Right;
}

} // namespace

Box measure_item(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how)
{
	refresh_item(item);
	const Box whole = scene_box(scene, item);
	if (!how.visible_pixels)
		return whole;
	obs_source_t *source = obs_sceneitem_get_source(item);
	Drawn part;
	bool measured = false;
	ShadowFree free;
	if (how.leave_out_shadows && shadow_free(source, free)) {
		// The picture as it is before its shadow, glow or outline is added,
		// then put where the shadow filter puts it. A source that plays
		// video frames cannot be drawn without its filters, so for one of
		// those the shadow's room is cut off the edges instead (below).
		const bool can_draw = !free.bare || !(obs_source_get_output_flags(free.draw) & OBS_SOURCE_ASYNC);
		if (can_draw) {
			part = drawn_part(free.draw, free.bare);
			measured = true;
			if (part.any) {
				part.x += free.left;
				part.y += free.top;
			}
		}
		obs_source_release(free.draw);
	}
	if (!measured) {
		part = drawn_part(source);
		if (part.any && how.leave_out_shadows) {
			double room[4];
			shadow_room(source, room);
			const double sw = obs_source_get_width(source), sh = obs_source_get_height(source);
			const double x0 = std::max(part.x, room[0]), y0 = std::max(part.y, room[1]);
			const double x1 = std::min(part.x + part.w, sw - room[2]);
			const double y1 = std::min(part.y + part.h, sh - room[3]);
			if (x1 > x0 && y1 > y0)
				part = {x0, y0, x1 - x0, y1 - y0, true};
		}
	}
	if (!part.any)
		return whole;
	// From the source's pixels to the canvas: the item's own drawing
	// transform (which already allows for its crop), then its group's.
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	const Space space = item_space(scene, item);
	double min_x = 1e18, min_y = 1e18, max_x = -1e18, max_y = -1e18;
	for (int i = 0; i < 4; i++) {
		vec3 corner, at;
		vec3_set(&corner, (float)(part.x - crop.left + ((i & 1) ? part.w : 0.0)),
			 (float)(part.y - crop.top + ((i & 2) ? part.h : 0.0)), 0.0f);
		vec3_transform(&at, &corner, &draw);
		const double x = space.ox + at.x * space.sx, y = space.oy + at.y * space.sy;
		min_x = std::min(min_x, x);
		max_x = std::max(max_x, x);
		min_y = std::min(min_y, y);
		max_y = std::max(max_y, y);
	}
	// What an item's crop or bounding box cuts off is not visible either.
	const double x0 = std::max(min_x, whole.x), y0 = std::max(min_y, whole.y);
	const double x1 = std::min(max_x, whole.x + whole.w), y1 = std::min(max_y, whole.y + whole.h);
	if (x1 <= x0 || y1 <= y0)
		return whole;
	return {x0, y0, x1 - x0, y1 - y0};
}

std::vector<obs_sceneitem_t *> selected_items(obs_source_t *scene)
{
	std::vector<obs_sceneitem_t *> out;
	obs_scene_t *s = scene_of(scene);
	if (!s)
		return out;
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item))
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
		return true;
	};
	obs_scene_enum_items(
		s,
		[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
			// A selected group moves as one thing; otherwise the selected
			// items inside it count on their own.
			if (obs_sceneitem_is_group(item) && !obs_sceneitem_selected(item))
				obs_sceneitem_group_enum_items(item, add, param);
			add(sc, item, param);
			return true;
		},
		&out);
	return out;
}

void shift_item(obs_source_t *scene, obs_sceneitem_t *item, double dx, double dy)
{
	if (obs_sceneitem_locked(item) || (std::fabs(dx) < 0.01 && std::fabs(dy) < 0.01))
		return;
	const Space space = item_space(scene, item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += (float)(dx / space.sx);
	pos.y += (float)(dy / space.sy);
	obs_sceneitem_set_pos(item, &pos);
}

int align_items(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, Edge edge, Against against,
		const Measure &how, double *line)
{
	if (items.empty())
		return 0;
	std::vector<Box> boxes;
	for (obs_sceneitem_t *item : items)
		boxes.push_back(measure_item(scene, item, how));

	// What to line up with: the screen, or the outline of all the items.
	Box target{0, 0, (double)obs_source_get_width(scene), (double)obs_source_get_height(scene)};
	if ((against == Against::First || against == Against::Last) && items.size() > 1) {
		// The items come in the order they were selected.
		target = against == Against::First ? boxes.front() : boxes.back();
	} else if (against == Against::Selection && items.size() > 1) {
		double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
		for (const Box &b : boxes) {
			x0 = std::min(x0, b.x);
			y0 = std::min(y0, b.y);
			x1 = std::max(x1, b.x + b.w);
			y1 = std::max(y1, b.y + b.h);
		}
		target = {x0, y0, x1 - x0, y1 - y0};
	}
	const double want = edge_of(target, edge);
	if (line)
		*line = want;
	int moved = 0;
	for (size_t i = 0; i < items.size(); i++) {
		const double delta = want - edge_of(boxes[i], edge);
		if (std::fabs(delta) < 0.01 || obs_sceneitem_locked(items[i]))
			continue;
		shift_item(scene, items[i], across(edge) ? delta : 0.0, across(edge) ? 0.0 : delta);
		moved++;
	}
	return moved;
}

int distribute_items(obs_source_t *scene, std::vector<obs_sceneitem_t *> items, bool horizontal, const Measure &how)
{
	if (items.size() < 3)
		return 0;
	struct Entry {
		obs_sceneitem_t *item;
		Box box;
	};
	std::vector<Entry> entries;
	for (obs_sceneitem_t *item : items)
		entries.push_back({item, measure_item(scene, item, how)});
	auto start = [&](const Box &b) {
		return horizontal ? b.x : b.y;
	};
	auto size = [&](const Box &b) {
		return horizontal ? b.w : b.h;
	};
	std::sort(entries.begin(), entries.end(),
		  [&](const Entry &a, const Entry &b) { return start(a.box) < start(b.box); });
	// The two outer items stay; the room left over is shared out as equal gaps.
	double used = 0.0;
	for (const Entry &e : entries)
		used += size(e.box);
	const double span = start(entries.back().box) + size(entries.back().box) - start(entries.front().box);
	const double gap = (span - used) / (double)(entries.size() - 1);
	double at = start(entries.front().box);
	int moved = 0;
	for (Entry &e : entries) {
		const double delta = at - start(e.box);
		if (std::fabs(delta) >= 0.01 && !obs_sceneitem_locked(e.item)) {
			shift_item(scene, e.item, horizontal ? delta : 0.0, horizontal ? 0.0 : delta);
			moved++;
		}
		at += size(e.box) + gap;
	}
	return moved;
}

int match_size(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, bool width, bool height,
	       bool keep_shape, const Measure &how)
{
	if (items.size() < 2 || (!width && !height))
		return 0;
	// The last item in the list is the one the others follow.
	const Box model = measure_item(scene, items.back(), how);
	Measure picture_only = how;
	picture_only.leave_out_shadows = true;
	int changed = 0;
	for (size_t i = 0; i + 1 < items.size(); i++) {
		obs_sceneitem_t *item = items[i];
		const Box before = measure_item(scene, item, how);
		if (before.w <= 0.0 || before.h <= 0.0 || model.w <= 0.0 || model.h <= 0.0 ||
		    obs_sceneitem_locked(item))
			continue;
		double fx = width ? model.w / before.w : 1.0;
		double fy = height ? model.h / before.h : 1.0;
		// When shadows count, the item's own shadow is part of the size to
		// reach. A shadow sized in screen pixels stays that size whatever
		// the item's size, so only the picture grows or shrinks to make up
		// the difference.
		ShadowFree free;
		if (how.visible_pixels && !how.leave_out_shadows && shadow_free(obs_sceneitem_get_source(item), free)) {
			obs_source_release(free.draw);
			if (free.fixed_on_screen) {
				const Box picture = measure_item(scene, item, picture_only);
				const double room_w = before.w - picture.w, room_h = before.h - picture.h;
				if (width && picture.w > 0.0 && model.w > room_w)
					fx = (model.w - room_w) / picture.w;
				if (height && picture.h > 0.0 && model.h > room_h)
					fy = (model.h - room_h) / picture.h;
			}
		}
		if (keep_shape) {
			// Both directions grow by the same amount. Asked to match both,
			// it fits inside the other's size.
			const double f = width && height ? std::min(fx, fy) : width ? fx : fy;
			fx = fy = f;
		}
		if (std::fabs(fx - 1.0) < 1e-4 && std::fabs(fy - 1.0) < 1e-4)
			continue;
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 bounds;
			obs_sceneitem_get_bounds(item, &bounds);
			if (fx != fy) {
				// A box that keeps the picture's shape cannot stretch it: the box
				// becomes exactly what is drawn now, and stretches from there.
				obs_source_t *source = obs_sceneitem_get_source(item);
				obs_sceneitem_crop crop;
				obs_sceneitem_get_crop(item, &crop);
				const double w = (double)obs_source_get_width(source) - crop.left - crop.right;
				const double h = (double)obs_source_get_height(source) - crop.top - crop.bottom;
				double sx = 1.0, sy = 1.0;
				item_draw_scale(item, sx, sy);
				if (w > 0.0 && h > 0.0) {
					bounds.x = (float)(w * sx);
					bounds.y = (float)(h * sy);
					obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_STRETCH);
				}
			}
			bounds.x *= (float)fx;
			bounds.y *= (float)fy;
			obs_sceneitem_set_bounds(item, &bounds);
		} else {
			vec2 scale;
			obs_sceneitem_get_scale(item, &scale);
			scale.x *= (float)fx;
			scale.y *= (float)fy;
			obs_sceneitem_set_scale(item, &scale);
		}
		// Its visible top-left corner stays where it was.
		const Box after = measure_item(scene, item, how);
		shift_item(scene, item, before.x - after.x, before.y - after.y);
		changed++;
	}
	return changed;
}

namespace {

// Every other visible item, measured: what can be in an item's way. Items in
// a group count on their own; a group left out takes its items with it.
std::vector<Box> others_in(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
{
	std::vector<Box> out;
	struct Gather {
		obs_source_t *scene;
		const std::vector<obs_sceneitem_t *> *leave_out;
		const Measure *how;
		std::vector<Box> *out;
	} gather{scene, &leave_out, &how, &out};
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *other, void *param) {
		auto *g = static_cast<Gather *>(param);
		if (std::find(g->leave_out->begin(), g->leave_out->end(), other) != g->leave_out->end() ||
		    !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other))
			return true;
		const Box b = measure_item(g->scene, other, *g->how);
		if (b.w > 0.0 && b.h > 0.0)
			g->out->push_back(b);
		return true;
	};
	if (obs_scene_t *s = scene_of(scene)) {
		obs_scene_enum_items(
			s,
			[](obs_scene_t *sc, obs_sceneitem_t *other, void *param) {
				auto *g = static_cast<Gather *>(param);
				const bool left_out = std::find(g->leave_out->begin(), g->leave_out->end(), other) !=
						      g->leave_out->end();
				if (obs_sceneitem_is_group(other) && !left_out && obs_sceneitem_visible(other))
					obs_sceneitem_group_enum_items(other, add, param);
				add(sc, other, param);
				return true;
			},
			&gather);
	}
	return out;
}

} // namespace

Offset slide_offset(obs_source_t *scene, obs_sceneitem_t *item, int dx, int dy, double gap, const Measure &how,
		    const std::vector<obs_sceneitem_t *> &leave_out)
{
	Offset out;
	Box me = measure_item(scene, item, how);
	if (!scene || me.w <= 0.0 || me.h <= 0.0)
		return out;
	std::vector<obs_sceneitem_t *> skip = leave_out;
	skip.push_back(item);
	const std::vector<Box> others = others_in(scene, skip, how);
	const double canvas_w = obs_source_get_width(scene), canvas_h = obs_source_get_height(scene);
	// Edges this close count as touching, not as overlapping.
	constexpr double kTouch = 0.5;
	auto overlap = [](double a0, double a1, double b0, double b1) {
		return a0 < b1 - kTouch && b0 < a1 - kTouch;
	};
	// The move along one direction (across, or up and down) from where `me` is now.
	auto along = [&](bool across, int dir) {
		const double pos = across ? me.x : me.y;
		const double size = across ? me.w : me.h;
		const double limit = across ? canvas_w : canvas_h;
		const double start = pos, end = pos + size;
		if (dir == 0) {
			// Centred between whatever is nearest on either side, or the screen's edges.
			double lo = 0.0, hi = limit;
			for (const Box &o : others) {
				const bool beside = across ? overlap(o.y, o.y + o.h, me.y, me.y + me.h)
							   : overlap(o.x, o.x + o.w, me.x, me.x + me.w);
				if (!beside)
					continue;
				const double o0 = across ? o.x : o.y, o1 = across ? o.x + o.w : o.y + o.h;
				if (o1 <= start + kTouch)
					lo = std::max(lo, o1);
				else if (o0 >= end - kTouch)
					hi = std::min(hi, o0);
			}
			return (lo + hi) / 2.0 - size / 2.0 - start;
		}
		// The places where the leading edge comes within the gap of an edge:
		// the screen's, and the near and the far side of anything in the
		// path. The item goes to the nearest one ahead of it, so the next
		// press passes through that edge to the one after.
		const double lead = dir > 0 ? end : start;
		std::vector<double> stops{dir > 0 ? limit - gap : gap};
		for (const Box &o : others) {
			const bool in_path = across ? overlap(o.y, o.y + o.h, me.y, me.y + me.h)
						    : overlap(o.x, o.x + o.w, me.x, me.x + me.w);
			if (!in_path)
				continue;
			const double o0 = across ? o.x : o.y, o1 = across ? o.x + o.w : o.y + o.h;
			stops.push_back(dir > 0 ? o0 - gap : o1 + gap); // just short of it
			stops.push_back(dir > 0 ? o1 - gap : o0 + gap); // inside it, short of its far side
		}
		bool found = false;
		double next = lead;
		for (double stop : stops) {
			const bool ahead = dir > 0 ? stop > lead + kTouch : stop < lead - kTouch;
			if (ahead && (!found || (dir > 0 ? stop < next : stop > next))) {
				next = stop;
				found = true;
			}
		}
		return found ? next - lead : 0.0;
	};
	const bool centre = dx == 0 && dy == 0;
	if (dx != 0 || centre) {
		out.x = along(true, dx);
		me.x += out.x;
	}
	if (dy != 0 || centre)
		out.y = along(false, dy);
	return out;
}

SnapLines snap_lines(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
{
	// Lines worth meeting: the screen's edges and centre, and every other
	// visible item's edges and centre.
	SnapLines lines;
	lines.xs = {0.0, obs_source_get_width(scene) / 2.0, (double)obs_source_get_width(scene)};
	lines.ys = {0.0, obs_source_get_height(scene) / 2.0, (double)obs_source_get_height(scene)};
	struct Others {
		obs_source_t *scene;
		const std::vector<obs_sceneitem_t *> *leave_out;
		const Measure *how;
		SnapLines *lines;
	} others{scene, &leave_out, &how, &lines};
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *other, void *param) {
		auto *o = static_cast<Others *>(param);
		if (std::find(o->leave_out->begin(), o->leave_out->end(), other) != o->leave_out->end() ||
		    !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other))
			return true;
		const Box b = measure_item(o->scene, other, *o->how);
		if (b.w <= 0.0 || b.h <= 0.0)
			return true;
		for (double x : {b.x, b.x + b.w / 2.0, b.x + b.w})
			o->lines->xs.push_back(x);
		for (double y : {b.y, b.y + b.h / 2.0, b.y + b.h})
			o->lines->ys.push_back(y);
		return true;
	};
	if (obs_scene_t *s = scene_of(scene)) {
		obs_scene_enum_items(
			s,
			[](obs_scene_t *sc, obs_sceneitem_t *other, void *param) {
				if (obs_sceneitem_is_group(other))
					obs_sceneitem_group_enum_items(other, add, param);
				add(sc, other, param);
				return true;
			},
			&others);
	}
	return lines;
}

Meeting meet_lines(const Box &box, const SnapLines &lines, double within)
{
	// The nearest line to any of the box's three: start, centre and end.
	auto nearest = [within](std::initializer_list<double> mine, const std::vector<double> &all, double &line,
				double &gap_out) {
		double best_gap = within + 1.0;
		for (double m : mine) {
			for (double candidate : all) {
				const double gap = std::fabs(candidate - m);
				if (gap < best_gap) {
					best_gap = gap;
					line = candidate;
					gap_out = candidate - m;
				}
			}
		}
		return best_gap <= within;
	};
	Meeting m;
	m.across = nearest({box.x, box.x + box.w / 2.0, box.x + box.w}, lines.xs, m.line_x, m.dx);
	m.down = nearest({box.y, box.y + box.h / 2.0, box.y + box.h}, lines.ys, m.line_y, m.dy);
	if (!m.across)
		m.dx = 0.0;
	if (!m.down)
		m.dy = 0.0;
	return m;
}

bool snap_item(obs_source_t *scene, obs_sceneitem_t *item, double within, const Measure &how)
{
	if (obs_sceneitem_locked(item))
		return false;
	const Box me = measure_item(scene, item, how);
	const Meeting m = meet_lines(me, snap_lines(scene, {item}, how), within);
	if (std::fabs(m.dx) < 0.01 && std::fabs(m.dy) < 0.01)
		return false;
	shift_item(scene, item, m.dx, m.dy);
	return true;
}

} // namespace stagekit
