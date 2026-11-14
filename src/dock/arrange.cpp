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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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

// Draws a source with none of its filters, the way OBS itself draws what is
// under the filter applied first. That works for every kind of source: one
// that draws itself (a browser, a nested scene), one that plays video frames.
// obs_source_default_render does not: it is only right for the plain kind.
void render_bare(obs_source_t *source)
{
	struct Find {
		obs_source_t *source;
		obs_source_t *lowest = nullptr;
	} find{source};
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *f = static_cast<Find *>(param);
			if (obs_filter_get_target(filter) == f->source)
				f->lowest = filter;
		},
		&find);
	if (find.lowest)
		obs_source_skip_video_filter(find.lowest);
	else
		obs_source_video_render(source);
}

// What two measured parts have in common. Empty when they do not meet.
Drawn common(const Drawn &a, const Drawn &b)
{
	Drawn out;
	if (!a.any || !b.any)
		return out;
	const double x0 = std::max(a.x, b.x), y0 = std::max(a.y, b.y);
	const double x1 = std::min(a.x + a.w, b.x + b.w), y1 = std::min(a.y + a.h, b.y + b.h);
	if (x1 - x0 < 1.0 || y1 - y0 < 1.0)
		return out;
	return {x0, y0, x1 - x0, y1 - y0, true};
}

// Measuring draws sources, which is only safe on OBS's own drawing thread,
// between two frames. The work is handed to that thread and waited for, but
// never for long: the owner's OBS froze for good while an item was dragged
// with guides on (beta53 to beta56), the window's thread waiting here for
// the drawing thread while that waited for something only the window's
// thread could give. After kPatience the caller goes on without an answer
// and the work, which owns everything it touches, finishes by itself later.
// Called on the drawing thread already (a filter's tick), it is simply done.
constexpr int kPatienceMs = 400;
std::atomic<int> g_gave_up{0};
// OBS is closing: nothing is measured any more, nothing is handed over.
std::atomic<bool> g_closing{false};

struct Job {
	std::function<void()> work;
	std::mutex mutex;
	std::condition_variable finished;
	bool done = false;
};

// False when the answer did not come in time.
bool on_drawing_thread(std::function<void()> work)
{
	if (obs_in_task_thread(OBS_TASK_GRAPHICS)) {
		work();
		return true;
	}
	if (g_closing.load())
		return false;
	auto job = std::make_shared<Job>();
	job->work = std::move(work);
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *param) {
			auto *held = static_cast<std::shared_ptr<Job> *>(param);
			(*held)->work();
			{
				std::lock_guard lock((*held)->mutex);
				(*held)->done = true;
			}
			(*held)->finished.notify_all();
			delete held;
		},
		new std::shared_ptr<Job>(job), false);
	std::unique_lock lock(job->mutex);
	if (job->finished.wait_for(lock, std::chrono::milliseconds(kPatienceMs), [&] { return job->done; }))
		return true;
	const int count = ++g_gave_up;
	if (count <= 5)
		blog(LOG_WARNING,
		     "[stagekit] measuring: OBS's drawing thread did not answer in %d ms; "
		     "going by the items' boxes this once",
		     kPatienceMs);
	return false;
}

// Keeps a scene and some of its items alive for work that may finish after
// its caller has gone on.
struct Held {
	obs_source_t *scene = nullptr;
	std::vector<obs_sceneitem_t *> items;
	Held(obs_source_t *scene_, const std::vector<obs_sceneitem_t *> &items_)
		: scene(obs_source_get_ref(scene_)),
		  items(items_)
	{
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_addref(item);
	}
	~Held()
	{
		for (obs_sceneitem_t *item : items)
			obs_sceneitem_release(item);
		obs_source_release(scene);
	}
	Held(const Held &) = delete;
	Held &operator=(const Held &) = delete;
};

// Draws the source small and finds the rows and columns with something in
// them.
void measure_drawn(obs_source_t *source, bool bare, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h, uint32_t &min_x,
		   uint32_t &min_y, uint32_t &max_x, uint32_t &max_y)
{
	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	if (gs_texrender_begin(render, w, h)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		if (bare)
			render_bare(source);
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
			// A smoothed edge ends in one row of faint pixels, each much fainter
			// than the one inside it. To the eye the picture ends before them,
			// and counting them leaves a line of background showing along the
			// screen's edge. Where the outermost row is all like that, the
			// picture ends one pixel further in.
			const auto at = [&](uint32_t x, uint32_t y) {
				return (int)data[(size_t)y * linesize + x * 4 + 3];
			};
			const auto fringe = [&](bool column, uint32_t line, int inward) {
				const uint32_t from = column ? min_y : min_x, to = column ? max_y : max_x;
				for (uint32_t i = from; i <= to; i++) {
					const int here = column ? at(line, i) : at(i, line);
					const int inside = column ? at((uint32_t)((int)line + inward), i)
								  : at(i, (uint32_t)((int)line + inward));
					if (here > 12 && here * 2 >= inside)
						return false;
				}
				return true;
			};
			if (max_x >= min_x + 3 && max_y >= min_y + 3) {
				const bool left = fringe(true, min_x, 1), right = fringe(true, max_x, -1);
				const bool top = fringe(false, min_y, 1), bottom = fringe(false, max_y, -1);
				min_x += left ? 1 : 0;
				max_x -= right ? 1 : 0;
				min_y += top ? 1 : 0;
				max_y -= bottom ? 1 : 0;
			}
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(render);
}

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

	uint32_t min_x = w, min_y = h, max_x = 0, max_y = 0;
	obs_enter_graphics();
	measure_drawn(source, bare, sw, sh, w, h, min_x, min_y, max_x, max_y);
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

namespace {
Box measure_item_now(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how, bool *nothing = nullptr);
}

void measuring_stop()
{
	g_closing = true;
}

bool measuring_stopped()
{
	return g_closing.load();
}

int measuring_gave_up()
{
	return g_gave_up.load();
}

Box measure_item(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how)
{
	if (!scene || !item)
		return {};
	if (obs_in_task_thread(OBS_TASK_GRAPHICS))
		return measure_item_now(scene, item, how);
	auto held = std::make_shared<Held>(scene, std::vector<obs_sceneitem_t *>{item});
	auto out = std::make_shared<Box>();
	if (on_drawing_thread([held, out, item, how] { *out = measure_item_now(held->scene, item, how); }))
		return *out;
	// No answer in time: the item's box.
	return scene_box(scene, item);
}

bool measure_seen(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how, Box &box)
{
	if (!scene || !item)
		return false;
	if (!obs_in_task_thread(OBS_TASK_GRAPHICS)) {
		box = measure_item(scene, item, how);
		return box.w > 0.0 && box.h > 0.0;
	}
	bool nothing = false;
	box = measure_item_now(scene, item, how, &nothing);
	return !nothing && box.w > 0.0 && box.h > 0.0;
}

namespace {

// `nothing` is set when the item is measured by what shows and nothing does:
// a widget with nothing to display, a capture with no window. Its box is
// still returned, for a tool asked to move that very item.
Box measure_item_now(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how, bool *nothing)
{
	if (nothing)
		*nothing = false;
	refresh_item(item);
	const Box whole = scene_box(scene, item);
	if (!how.visible_pixels)
		return whole;
	obs_source_t *source = obs_sceneitem_get_source(item);
	Drawn part;
	bool measured = false;
	ShadowFree free;
	const bool shadowed = how.leave_out_shadows && shadow_free(source, free);
	// Everything the source draws, filters and all.
	const Drawn drawn = drawn_part(source);
	if (how.leave_out_shadows) {
		// The source's own pixels: drawn with none of its filters, so no
		// shadow, glow, outline or border any filter adds counts. That
		// picture sits in the whole source where the shadows' room puts it.
		// It only can when the filters change the source's size by nothing
		// but that room (an Auto Frame, say, gives it another size).
		const double bw = obs_source_get_base_width(source), bh = obs_source_get_base_height(source);
		const double fw = obs_source_get_width(source), fh = obs_source_get_height(source);
		const bool only_room = std::fabs(fw - (bw + free.left + free.right)) <= 2.0 &&
				       std::fabs(fh - (bh + free.top + free.bottom)) <= 2.0;
		// A source that plays video frames (a camera, a video) is not drawn
		// on its own here: OBS draws those in step with its frames. Up to
		// beta52 they were left out; beta53 to beta55 drew them, and the
		// owner had OBS crash during slides and drags in exactly those
		// versions. Not proven to be the cause, so they are left out again.
		const bool async = (obs_source_get_output_flags(source) & OBS_SOURCE_ASYNC) != 0;
		if (bw > 0.0 && bh > 0.0 && only_room && !async) {
			part = drawn_part(source, true);
			measured = part.any;
		}
		// Otherwise: the picture as it is before its first shadow, glow or
		// outline, put where the shadow puts it.
		if (!measured && shadowed && !(free.bare && async)) {
			part = drawn_part(free.draw, free.bare);
			measured = part.any;
		}
		if (measured) {
			part.x += free.left;
			part.y += free.top;
			// What a filter takes away (a mask, a key, cut corners) is not
			// there to be seen either: only what is also drawn in the end.
			const Drawn both = common(part, drawn);
			if (both.any)
				part = both;
		}
	}
	if (shadowed)
		obs_source_release(free.draw);
	if (!measured) {
		// Nothing to go by without the filters: everything drawn, less the
		// room the shadows take.
		part = drawn;
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
	if (!part.any) {
		if (nothing)
			*nothing = true;
		return whole;
	}
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

} // namespace

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

namespace {

// One copy of a source: the scene it is in (never a group) and its item there.
struct Copy {
	obs_source_t *scene;
	obs_sceneitem_t *item;
};

// Every item showing `source`, in every scene but `skip`, groups included.
std::vector<Copy> copies_of(obs_source_t *source, obs_source_t *skip)
{
	std::vector<Copy> out;
	struct Find {
		obs_source_t *source, *skip, *scene;
		std::vector<Copy> *out;
	} find{source, skip, nullptr, &out};
	static const auto match = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *f = static_cast<Find *>(param);
		if (obs_sceneitem_get_source(item) == f->source)
			f->out->push_back({f->scene, item});
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			auto *f = static_cast<Find *>(param);
			// A group is listed as a scene too; its items are reached through
			// the scene that holds it.
			if (obs_source_is_group(scene) || scene == f->skip)
				return true;
			f->scene = scene;
			obs_scene_enum_items(
				obs_scene_from_source(scene),
				[](obs_scene_t *s, obs_sceneitem_t *item, void *p) {
					match(s, item, p);
					if (obs_sceneitem_is_group(item))
						obs_sceneitem_group_enum_items(item, match, p);
					return true;
				},
				f);
			return true;
		},
		&find);
	return out;
}

// Gives `to` the transform of `from`, worked out on the screen, so a copy in
// a group lands where a copy outside one would.
void copy_transform(obs_source_t *from_scene, obs_sceneitem_t *from, obs_source_t *to_scene, obs_sceneitem_t *to)
{
	const Space fs = item_space(from_scene, from), ts = item_space(to_scene, to);
	const double kx = fs.sx / ts.sx, ky = fs.sy / ts.sy;
	vec2 pos, scale, bounds;
	obs_sceneitem_get_pos(from, &pos);
	obs_sceneitem_get_scale(from, &scale);
	obs_sceneitem_get_bounds(from, &bounds);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(from, &crop);

	obs_sceneitem_defer_update_begin(to);
	obs_sceneitem_set_alignment(to, obs_sceneitem_get_alignment(from));
	obs_sceneitem_set_bounds_type(to, obs_sceneitem_get_bounds_type(from));
	obs_sceneitem_set_bounds_alignment(to, obs_sceneitem_get_bounds_alignment(from));
	vec2 b;
	vec2_set(&b, (float)(bounds.x * kx), (float)(bounds.y * ky));
	obs_sceneitem_set_bounds(to, &b);
	vec2 s;
	vec2_set(&s, (float)(scale.x * kx), (float)(scale.y * ky));
	obs_sceneitem_set_scale(to, &s);
	obs_sceneitem_set_rot(to, obs_sceneitem_get_rot(from));
	obs_sceneitem_set_crop(to, &crop);
	vec2 p;
	vec2_set(&p, (float)((fs.ox + pos.x * fs.sx - ts.ox) / ts.sx),
		 (float)((fs.oy + pos.y * fs.sy - ts.oy) / ts.sy));
	obs_sceneitem_set_pos(to, &p);
	obs_sceneitem_defer_update_end(to);
}

} // namespace

int copy_to_every_scene(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items,
			const std::vector<std::string> *only, const std::vector<std::string> *to_sources)
{
	int changed = 0;
	const auto wanted_scene = [only](obs_source_t *in) {
		if (!only)
			return true;
		const char *name = obs_source_get_name(in);
		return name && std::find(only->begin(), only->end(), name) != only->end();
	};
	if (to_sources && !to_sources->empty()) {
		// To other sources: one item is the model, and every item showing a
		// listed source takes its transform, here and in the other scenes.
		if (items.empty())
			return 0;
		obs_sceneitem_t *model = items.back();
		for (const std::string &name : *to_sources) {
			obs_source_t *source = obs_get_source_by_name(name.c_str());
			if (!source)
				continue;
			for (const Copy &copy : copies_of(source, nullptr)) {
				if (copy.item == model || obs_sceneitem_locked(copy.item) || !wanted_scene(copy.scene))
					continue;
				copy_transform(scene, model, copy.scene, copy.item);
				changed++;
			}
			obs_source_release(source);
		}
		return changed;
	}
	std::vector<obs_source_t *> done; // one item per source: the first selected
	for (obs_sceneitem_t *item : items) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (!source || std::find(done.begin(), done.end(), source) != done.end())
			continue;
		done.push_back(source);
		for (const Copy &copy : copies_of(source, scene)) {
			if (obs_sceneitem_locked(copy.item))
				continue;
			if (!wanted_scene(copy.scene))
				continue;
			copy_transform(scene, item, copy.scene, copy.item);
			changed++;
		}
	}
	return changed;
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
std::vector<Box> others_in_now(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
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
		// With "Use visible pixels", an item that draws nothing is not there:
		// it is not in the way of a slide or a resize.
		bool nothing = false;
		const Box b = measure_item_now(g->scene, other, *g->how, &nothing);
		if (nothing)
			return true;
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

// Every item is measured in one visit to the drawing thread.
std::vector<Box> others_in(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
{
	if (!scene)
		return {};
	if (obs_in_task_thread(OBS_TASK_GRAPHICS))
		return others_in_now(scene, leave_out, how);
	// The items to leave out are only compared with, never looked into, so
	// they are not held: one of them may already be gone.
	auto held = std::make_shared<Held>(scene, std::vector<obs_sceneitem_t *>{});
	auto out = std::make_shared<std::vector<Box>>();
	if (on_drawing_thread([held, out, leave_out, how] { *out = others_in_now(held->scene, leave_out, how); }))
		return *out;
	// No answer in time: nothing counts as in the way this once.
	return {};
}

} // namespace

Offset slide_offset(obs_source_t *scene, obs_sceneitem_t *item, int dx, int dy, double gap, const Measure &how,
		    const std::vector<obs_sceneitem_t *> &leave_out, const Box *from)
{
	Offset out;
	Box me = from ? *from : measure_item(scene, item, how);
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
		// The screen's edge is as far as it goes: nothing past it counts.
		const double screen = dir > 0 ? limit - gap : gap;
		std::vector<double> stops{screen};
		for (const Box &o : others) {
			const bool in_path = across ? overlap(o.y, o.y + o.h, me.y, me.y + me.h)
						    : overlap(o.x, o.x + o.w, me.x, me.x + me.w);
			if (!in_path)
				continue;
			const double o0 = across ? o.x : o.y, o1 = across ? o.x + o.w : o.y + o.h;
			stops.push_back(dir > 0 ? o0 - gap : o1 + gap); // just short of it
			// Inside it, short of its far side: only for something wider than
			// the gap, or that stop would come before its near side.
			if (dir > 0 ? o1 - gap > o0 + kTouch : o0 + gap < o1 - kTouch)
				stops.push_back(dir > 0 ? o1 - gap : o0 + gap);
		}
		bool found = false;
		double next = lead;
		for (double stop : stops) {
			const bool on_screen = dir > 0 ? stop <= screen + kTouch : stop >= screen - kTouch;
			const bool ahead = on_screen && (dir > 0 ? stop > lead + kTouch : stop < lead - kTouch);
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

namespace {

SnapLines snap_lines_now(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
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
		// Nor does it give a line to snap to or a guide to show.
		bool nothing = false;
		const Box b = measure_item_now(o->scene, other, *o->how, &nothing);
		if (nothing)
			return true;
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

} // namespace

SnapLines snap_lines(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how)
{
	if (!scene)
		return {};
	if (obs_in_task_thread(OBS_TASK_GRAPHICS))
		return snap_lines_now(scene, leave_out, how);
	auto held = std::make_shared<Held>(scene, std::vector<obs_sceneitem_t *>{});
	auto lines = std::make_shared<SnapLines>();
	if (on_drawing_thread([held, lines, leave_out, how] { *lines = snap_lines_now(held->scene, leave_out, how); }))
		return *lines;
	// No answer in time: no lines to meet this once.
	return {};
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

bool limit_resize(obs_source_t *scene, obs_sceneitem_t *item, const Box &before, double gap, const Measure &how)
{
	if (!scene || !item || obs_sceneitem_locked(item) || std::fabs(obs_sceneitem_get_rot(item)) > 0.01f)
		return false;
	refresh_item(item);
	Box now = scene_box(scene, item);
	// Only a change of size is a resize; an item that was moved is left to
	// the other tools.
	if (before.w <= 0.0 || before.h <= 0.0 || now.w <= 0.0 || now.h <= 0.0 ||
	    (std::fabs(now.w - before.w) < 0.5 && std::fabs(now.h - before.h) < 0.5))
		return false;
	const double canvas_w = obs_source_get_width(scene), canvas_h = obs_source_get_height(scene);
	const std::vector<Box> others = others_in(scene, {item}, how);
	// Both directions grew by the same amount: it was scaled, and is pulled
	// back the same way so its shape is kept.
	const bool even = std::fabs(now.w / before.w - now.h / before.h) < 0.01;
	constexpr double kTouch = 0.5;
	bool changed = false;
	// Twice: what is visible in an item is not always the same share of its
	// box at another size, so the second pass takes up what the first left.
	for (int pass = 0; pass < 2; pass++) {
		const Box seen = measure_item(scene, item, how);
		// How far what counts as the item sits inside its box, each side.
		const double in_l = seen.x - now.x, in_t = seen.y - now.y;
		const double in_r = (now.x + now.w) - (seen.x + seen.w), in_b = (now.y + now.h) - (seen.y + seen.h);
		// Which sides were pulled outward.
		const bool out_l = now.x<before.x - kTouch, out_r = now.x + now.w> before.x + before.w + kTouch;
		const bool out_t = now.y<before.y - kTouch, out_b = now.y + now.h> before.y + before.h + kTouch;
		// The furthest each side may go: the edge distance short of the
		// screen's edge and of anything that was ahead of it and is level
		// with the item.
		double max_r = canvas_w - gap, min_l = gap, max_b = canvas_h - gap, min_t = gap;
		const double was_l = before.x + in_l, was_r = before.x + before.w - in_r;
		const double was_t = before.y + in_t, was_b = before.y + before.h - in_b;
		for (const Box &o : others) {
			const bool level = o.y < seen.y + seen.h - kTouch && seen.y < o.y + o.h - kTouch;
			const bool in_line = o.x < seen.x + seen.w - kTouch && seen.x < o.x + o.w - kTouch;
			if (level && o.x >= was_r - kTouch)
				max_r = std::min(max_r, o.x - gap);
			if (level && o.x + o.w <= was_l + kTouch)
				min_l = std::max(min_l, o.x + o.w + gap);
			if (in_line && o.y >= was_b - kTouch)
				max_b = std::min(max_b, o.y - gap);
			if (in_line && o.y + o.h <= was_t + kTouch)
				min_t = std::max(min_t, o.y + o.h + gap);
		}
		// How much too far each pulled side went.
		const double over_r = out_r ? std::max(0.0, seen.x + seen.w - max_r) : 0.0;
		const double over_l = out_l ? std::max(0.0, min_l - seen.x) : 0.0;
		const double over_b = out_b ? std::max(0.0, seen.y + seen.h - max_b) : 0.0;
		const double over_t = out_t ? std::max(0.0, min_t - seen.y) : 0.0;
		if (over_r < kTouch && over_l < kTouch && over_b < kTouch && over_t < kTouch)
			break;
		Box target = now;
		target.x += over_l;
		target.w -= over_l + over_r;
		target.y += over_t;
		target.h -= over_t + over_b;
		if (even) {
			// The smaller of the two sizes wins, and the side or corner that
			// was not pulled stays where it is.
			const double f = std::min(target.w / now.w, target.h / now.h);
			const double w = now.w * f, h = now.h * f;
			target.x = out_l && !out_r  ? now.x + now.w - w
				   : out_l == out_r ? now.x + (now.w - w) / 2.0
						    : now.x;
			target.y = out_t && !out_b  ? now.y + now.h - h
				   : out_t == out_b ? now.y + (now.h - h) / 2.0
						    : now.y;
			target.w = w;
			target.h = h;
		}
		if (target.w < 1.0 || target.h < 1.0)
			break;
		const double fx = target.w / now.w, fy = target.h / now.h;
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 bounds;
			obs_sceneitem_get_bounds(item, &bounds);
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
		refresh_item(item);
		const Box resized = scene_box(scene, item);
		shift_item(scene, item, target.x - resized.x, target.y - resized.y);
		refresh_item(item);
		now = scene_box(scene, item);
		changed = true;
	}
	return changed;
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

int set_items_locked(bool lock, const std::vector<std::string> *sources, const std::vector<std::string> *scenes)
{
	struct Work {
		bool lock;
		const std::vector<std::string> *sources, *scenes;
		int changed = 0;
	} work{lock, sources, scenes};
	static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *w = static_cast<Work *>(param);
		const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
		const bool wanted = !w->sources || (name && std::find(w->sources->begin(), w->sources->end(), name) !=
								    w->sources->end());
		if (wanted && obs_sceneitem_locked(item) != w->lock) {
			obs_sceneitem_set_locked(item, w->lock);
			w->changed++;
		}
		// A group's own items belong to the scene that holds the group.
		if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					auto *w2 = static_cast<Work *>(p);
					const char *inner_name = obs_source_get_name(obs_sceneitem_get_source(inner));
					const bool inner_wanted =
						!w2->sources ||
						(inner_name && std::find(w2->sources->begin(), w2->sources->end(),
									 inner_name) != w2->sources->end());
					if (inner_wanted && obs_sceneitem_locked(inner) != w2->lock) {
						obs_sceneitem_set_locked(inner, w2->lock);
						w2->changed++;
					}
					return true;
				},
				param);
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			auto *w = static_cast<Work *>(param);
			if (obs_source_is_group(scene))
				return true; // reached through the scene that holds it
			const char *name = obs_source_get_name(scene);
			if (w->scenes &&
			    (!name || std::find(w->scenes->begin(), w->scenes->end(), name) == w->scenes->end()))
				return true;
			obs_scene_enum_items(obs_scene_from_source(scene), each, param);
			return true;
		},
		&work);
	return work.changed;
}

namespace {

// Settings that belong to the source a filter is on, not to how the filter
// looks or behaves: copied to another source they would point it at the
// wrong item or undo what it had placed or learned there.
const char *const kOwnSettings[] = {
	"item",           "items",           "linked",           "skip",
	"by_hand",        "elsewhere",       "sources",          "scenes",
	"saved_x",        "saved_y",         "saved_scale_x",    "saved_scale_y",
	"saved_count",    "spot_x",          "spot_y",           "anchor_x",
	"anchor_y",       "have_anchor",     "adjusted_left",    "adjusted_top",
	"adjusted_right", "adjusted_bottom", "adjusted_for",     "placed",
	"learned",        "last_width",      "last_height",      "rule_program",
	"rule_browse",    "force_ratio",     "force_until_gone",
};

// Copies one setting, whatever it holds, by way of its JSON.
void keep_setting(obs_data_t *into, obs_data_t *from, const char *name)
{
	obs_data_erase(into, name);
	if (!obs_data_has_user_value(from, name))
		return;
	obs_data_t *one = obs_data_create();
	obs_data_item_t *item = obs_data_item_byname(from, name);
	if (item) {
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			obs_data_set_string(one, name, obs_data_item_get_string(item));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(one, name, obs_data_item_get_bool(item));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				obs_data_set_int(one, name, obs_data_item_get_int(item));
			else
				obs_data_set_double(one, name, obs_data_item_get_double(item));
			break;
		case OBS_DATA_OBJECT: {
			obs_data_t *object = obs_data_item_get_obj(item);
			obs_data_set_obj(one, name, object);
			obs_data_release(object);
			break;
		}
		case OBS_DATA_ARRAY: {
			obs_data_array_t *array = obs_data_item_get_array(item);
			obs_data_set_array(one, name, array);
			obs_data_array_release(array);
			break;
		}
		default:
			break;
		}
		obs_data_item_release(&item);
	}
	obs_data_apply(into, one);
	obs_data_release(one);
}

} // namespace

int copy_filter_settings(obs_source_t *from, const char *filter_name, const std::vector<std::string> *to,
			 bool add_missing, int &added)
{
	added = 0;
	obs_source_t *filter = from && filter_name ? obs_source_get_filter_by_name(from, filter_name) : nullptr;
	if (!filter)
		return 0;
	struct Work {
		obs_source_t *from, *filter;
		std::string kind, name;
		obs_data_t *settings;
		const std::vector<std::string> *to;
		bool add_missing;
		int changed = 0, added = 0;
		std::vector<obs_source_t *> same_name, same_kind;
	} work{from, filter, obs_source_get_id(filter), filter_name, obs_source_get_settings(filter), to, add_missing};
	const auto each = [](void *param, obs_source_t *target) {
		auto *w = static_cast<Work *>(param);
		const char *name = obs_source_get_name(target);
		if (target == w->from || !name || !*name)
			return true;
		if (w->to && std::find(w->to->begin(), w->to->end(), name) == w->to->end())
			return true;
		w->same_name.clear();
		w->same_kind.clear();
		obs_source_enum_filters(
			target,
			[](obs_source_t *, obs_source_t *one, void *p) {
				auto *w2 = static_cast<Work *>(p);
				if (w2->kind != obs_source_get_id(one))
					return;
				w2->same_kind.push_back(one);
				const char *one_name = obs_source_get_name(one);
				if (one_name && w2->name == one_name)
					w2->same_name.push_back(one);
			},
			w);
		const std::vector<obs_source_t *> &chosen = w->same_name.empty() ? w->same_kind : w->same_name;
		for (obs_source_t *one : chosen) {
			// Everything as on the filter copied from, except what this
			// one keeps for its own source.
			obs_data_t *merged = obs_data_create();
			obs_data_apply(merged, w->settings);
			obs_data_t *own = obs_source_get_settings(one);
			for (const char *keep : kOwnSettings)
				keep_setting(merged, own, keep);
			obs_data_release(own);
			obs_source_reset_settings(one, merged);
			obs_data_release(merged);
			w->changed++;
		}
		// A copy for a source that has none, where the filter can go: a
		// picture filter on something with a picture, a sound filter on sound.
		const uint32_t needs = obs_source_get_output_flags(w->filter) & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO);
		// A scene's filter is only added to scenes, a source's only to sources.
		const bool alike = (obs_source_get_type(target) == OBS_SOURCE_TYPE_SCENE) ==
				   (obs_source_get_type(w->from) == OBS_SOURCE_TYPE_SCENE);
		if (chosen.empty() && w->add_missing && alike &&
		    (obs_source_get_output_flags(target) & needs) == needs) {
			obs_data_t *fresh = obs_data_create();
			obs_data_apply(fresh, w->settings);
			for (const char *keep : kOwnSettings)
				obs_data_erase(fresh, keep);
			obs_source_t *made = obs_source_create(w->kind.c_str(), w->name.c_str(), fresh, nullptr);
			obs_data_release(fresh);
			if (made) {
				obs_source_filter_add(target, made);
				obs_source_set_enabled(made, obs_source_enabled(w->filter));
				obs_source_release(made);
				w->added++;
			}
		}
		return true;
	};
	obs_enum_sources(each, &work);
	obs_enum_scenes(each, &work); // scenes and groups carry filters too
	obs_data_release(work.settings);
	obs_source_release(filter);
	added = work.added;
	return work.changed;
}

std::vector<std::string> sources_in_scenes(const std::vector<std::string> &scenes)
{
	std::vector<std::string> names;
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *list = static_cast<std::vector<std::string> *>(param);
		const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
		if (name && *name && std::find(list->begin(), list->end(), name) == list->end())
			list->push_back(name);
		if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					auto *inner_list = static_cast<std::vector<std::string> *>(p);
					const char *inner_name = obs_source_get_name(obs_sceneitem_get_source(inner));
					if (inner_name && *inner_name &&
					    std::find(inner_list->begin(), inner_list->end(), inner_name) ==
						    inner_list->end())
						inner_list->push_back(inner_name);
					return true;
				},
				param);
		return true;
	};
	for (const std::string &scene_name : scenes) {
		obs_source_t *scene = obs_get_source_by_name(scene_name.c_str());
		if (obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr) {
			if (std::find(names.begin(), names.end(), scene_name) == names.end())
				names.push_back(scene_name);
			obs_scene_enum_items(s, add, &names);
		}
		obs_source_release(scene);
	}
	return names;
}

} // namespace stagekit
