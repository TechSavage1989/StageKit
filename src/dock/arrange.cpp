/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "arrange.hpp"

#include "../placement/fit-engine.hpp"
#include "../placement/fit-item.hpp"

#include "../common/filter-input.hpp"
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
		skip_filter_input(find.lowest);
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
		     "[stagekit] measuring: OBS's drawing thread did not answer in %d ms", kPatienceMs);
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
		struct Result {
			Box box;
			bool nothing = true;
		};
		auto held = std::make_shared<Held>(scene, std::vector<obs_sceneitem_t *>{item});
		auto result = std::make_shared<Result>();
		if (!on_drawing_thread([held, item, how, result] {
			    result->box = measure_item_now(held->scene, item, how, &result->nothing);
		    }))
			return false;
		box = result->box;
		return !result->nothing && box.w > 0.0 && box.h > 0.0;
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
			part = common(part, drawn);
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
			part = {x0, y0, x1 - x0, y1 - y0, x1 > x0 && y1 > y0};
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
	if (x1 <= x0 || y1 <= y0) {
		if (nothing)
			*nothing = true;
		return whole;
	}
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
			const std::vector<std::string> *only, const std::vector<std::string> *to_sources,
			bool even_locked, const CopyAllowed &allow)
{
	int changed = 0;
	// The copy given the model's transform; false when `allow` refused it,
	// and it was put back as it was.
	const auto copy_onto = [&allow](obs_source_t *from_scene, obs_sceneitem_t *model, const Copy &copy) {
		if (!allow) {
			copy_transform(from_scene, model, copy.scene, copy.item);
			return true;
		}
		struct Was {
			vec2 pos, scale, bounds;
			uint32_t align, bounds_align;
			obs_bounds_type bounds_type;
			float rot;
			obs_sceneitem_crop crop;
		} was;
		obs_sceneitem_t *to = copy.item;
		obs_sceneitem_get_pos(to, &was.pos);
		obs_sceneitem_get_scale(to, &was.scale);
		obs_sceneitem_get_bounds(to, &was.bounds);
		was.align = obs_sceneitem_get_alignment(to);
		was.bounds_align = obs_sceneitem_get_bounds_alignment(to);
		was.bounds_type = obs_sceneitem_get_bounds_type(to);
		was.rot = obs_sceneitem_get_rot(to);
		obs_sceneitem_get_crop(to, &was.crop);
		refresh_item(to);
		const Box before = scene_box(copy.scene, to);
		copy_transform(from_scene, model, copy.scene, to);
		refresh_item(to);
		const Box after = scene_box(copy.scene, to);
		if (allow(copy.scene, to, before, after))
			return true;
		obs_sceneitem_defer_update_begin(to);
		obs_sceneitem_set_alignment(to, was.align);
		obs_sceneitem_set_bounds_type(to, was.bounds_type);
		obs_sceneitem_set_bounds_alignment(to, was.bounds_align);
		obs_sceneitem_set_bounds(to, &was.bounds);
		obs_sceneitem_set_scale(to, &was.scale);
		obs_sceneitem_set_rot(to, was.rot);
		obs_sceneitem_set_crop(to, &was.crop);
		obs_sceneitem_set_pos(to, &was.pos);
		obs_sceneitem_defer_update_end(to);
		refresh_item(to);
		return false;
	};
	// A copy that a Fit to Space looks after is handed over to it as placed
	// on purpose, or the filter would put it straight back where it was.
	// (In the copy's own scene only: the item copied from may still be
	// gliding to its place, and is handed over when it gets there.)
	const auto placed = [](const Copy &copy) {
		fit_item_placed(copy.scene, obs_source_get_name(obs_sceneitem_get_source(copy.item)));
	};
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
				if (copy.item == model || (!even_locked && obs_sceneitem_locked(copy.item)) ||
				    !wanted_scene(copy.scene))
					continue;
				if (!copy_onto(scene, model, copy))
					continue;
				placed(copy);
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
			// An item inside a group shown in several scenes is one and
			// the same item in all of them: there is nothing to copy to,
			// and "copying" would move it inside its group.
			if (copy.item == item)
				continue;
			if (!even_locked && obs_sceneitem_locked(copy.item))
				continue;
			if (!wanted_scene(copy.scene))
				continue;
			if (!copy_onto(scene, item, copy))
				continue;
			placed(copy);
			changed++;
		}
	}
	return changed;
}

int share_place(obs_source_t *scene, obs_sceneitem_t *item, const std::vector<std::string> &scenes, const double *home)
{
	obs_source_t *source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!scene || !source)
		return 0;
	int changed = 0;
	for (const Copy &copy : copies_of(source, scene)) {
		// (The same item, where a group holding it is shown in several scenes.)
		if (copy.item == item || obs_source_showing(copy.scene))
			continue;
		const char *in = obs_source_get_name(copy.scene);
		if (!in || std::find(scenes.begin(), scenes.end(), in) == scenes.end())
			continue;
		copy_transform(scene, item, copy.scene, copy.item);
		fit_item_shared(copy.scene, obs_source_get_name(source), home);
		changed++;
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
		    !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other) || is_morph_layer(other))
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
		    !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other) || is_morph_layer(other))
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
	"rule_browse",    "force_ratio",     "force_until_gone", "orig_item",
	"orig_k",         "share_scenes",    "keep_item",        "keep_k",
	"orig_w",         "orig_h",          "keep_w",           "keep_h",
	"mode_now",       "mode_before",     "managed",          "managed_by",
	"hid_items",      "hidden_record",   "home_item",        "home_x",
	"home_y",         "home_w",          "home_h",           "home_space_x",
	"home_space_y",   "home_space_w",    "home_space_h",     "home_cut_l",
	"home_cut_t",     "home_cut_r",      "home_cut_b",
};

// Whether a setting is one of those. A setting kept per source or per mode
// ("fit|Cam|anchor_x") goes by the last part of its name.
bool own_setting(const char *name)
{
	if (!name)
		return false;
	const char *bar = std::strrchr(name, '|');
	const char *last = bar ? bar + 1 : name;
	for (const char *keep : kOwnSettings)
		if (std::strcmp(last, keep) == 0)
			return true;
	return false;
}

// The names of the settings in `data` that are a source's own.
std::vector<std::string> own_names(obs_data_t *data)
{
	std::vector<std::string> names;
	for (obs_data_item_t *item = obs_data_first(data); item; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		if (own_setting(name))
			names.emplace_back(name);
	}
	return names;
}

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
			std::vector<std::string> keep = own_names(merged);
			for (std::string &name : own_names(own))
				keep.push_back(std::move(name));
			for (const std::string &name : keep)
				keep_setting(merged, own, name.c_str());
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
			for (const std::string &name : own_names(fresh))
				obs_data_erase(fresh, name.c_str());
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

namespace {

// Every item of a scene, groups' items after their group, bottom-most first.
std::vector<obs_sceneitem_t *> stacking_order(obs_source_t *scene)
{
	std::vector<obs_sceneitem_t *> out;
	obs_scene_t *s = scene_of(scene);
	if (!s)
		return out;
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
		return true;
	};
	obs_scene_enum_items(
		s,
		[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
			add(sc, item, param);
			if (obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(item, add, param);
			return true;
		},
		&out);
	return out;
}

// Scales an item by `k` about nothing in particular: its scale, or its
// bounding box when it has one. A box that only limits the size (OBS's
// "maximum size only") draws its source at the item's own scale while that
// fits in it, so both are scaled: the picture grows by `k` either way.
void scale_item_by(obs_sceneitem_t *item, double k)
{
	const obs_bounds_type type = obs_sceneitem_get_bounds_type(item);
	if (type != OBS_BOUNDS_NONE) {
		vec2 bounds;
		obs_sceneitem_get_bounds(item, &bounds);
		bounds.x *= (float)k;
		bounds.y *= (float)k;
		obs_sceneitem_set_bounds(item, &bounds);
	}
	if (type == OBS_BOUNDS_NONE || type == OBS_BOUNDS_MAX_ONLY) {
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		scale.x *= (float)k;
		scale.y *= (float)k;
		obs_sceneitem_set_scale(item, &scale);
	}
}

// (What a one-press fit says it is, to the claims of the scene's fits: no
// fit, so every claim counts.)
const char kFitTool = 0;

} // namespace

namespace {

// One coordinate of a moved item's part, from before (`p0`, `len`) to after
// (`q0`, `qlen`): inside the part it scales with it; outside, with
// `keeps_outside`, it keeps its distance from the part (a shadow sized in
// screen pixels), else it scales too.
double moved_coord(double v, double p0, double len, double q0, double qlen, bool keeps_outside)
{
	if (len < 1e-9)
		return q0 + (v - p0);
	if (keeps_outside) {
		if (v <= p0)
			return q0 - (p0 - v);
		if (v >= p0 + len)
			return q0 + qlen + (v - p0 - len);
	}
	return q0 + (v - p0) * qlen / len;
}

// What another fit sees of a moved item (`o`), where the item goes.
Box moved_box(const Box &o, const MovedBox &m)
{
	const double x0 = moved_coord(o.x, m.from.x, m.from.w, m.to.x, m.to.w, m.keeps_outside);
	const double x1 = moved_coord(o.x + o.w, m.from.x, m.from.w, m.to.x, m.to.w, m.keeps_outside);
	const double y0 = moved_coord(o.y, m.from.y, m.from.h, m.to.y, m.to.h, m.keeps_outside);
	const double y1 = moved_coord(o.y + o.h, m.from.y, m.from.h, m.to.y, m.to.h, m.keeps_outside);
	return Box{x0, y0, x1 - x0, y1 - y0};
}

// What was seen of an item when its box was `at`, now that its box is
// `now`: the same part of its box (as FitScan follows an item by its box).
Box follow_box(const Box &o, const Box &at, const Box &now)
{
	if (at.w < 1.0 || at.h < 1.0 || now.w < 1.0 || now.h < 1.0)
		return o;
	const double kx = now.w / at.w, ky = now.h / at.h;
	return Box{now.x + (o.x - at.x) * kx, now.y + (o.y - at.y) * ky, o.w * kx, o.h * ky};
}

bool same_space(const Box &a, const Box &b)
{
	const bool had = a.w >= 2.0 && a.h >= 2.0, has = b.w >= 2.0 && b.h >= 2.0;
	return had == has && (!had || (std::fabs(a.x - b.x) <= 0.5 && std::fabs(a.y - b.y) <= 0.5 &&
				       std::fabs(a.w - b.w) <= 0.5 && std::fabs(a.h - b.h) <= 0.5));
}

// The home of moved item `i` of a fit's inputs once it is moved (`m`): with
// its own fit known, that fit's home where the item is put (it takes its
// home there, FitEngine::placed, also when its home did not match the item
// before, and then goes elsewhere when it does not keep the gap there), so
// the others look for their own home spaces with it there; else its home
// moved with it.
void home_after(fit::SpaceInputs &in, size_t i, const MovedBox &m)
{
	if (i >= in.has_homes.size() || i >= in.homes.size())
		return;
	if (m.has_claim) {
		in.has_homes[i] = m.has_home;
		if (m.has_home)
			in.homes[i] = m.home;
	} else if (in.has_homes[i]) {
		in.homes[i] = moved_box(in.homes[i], m);
	}
}

// A fit's inputs with the moved items where they are now (not `after`) or
// where they would be (`after`): each where it is now, not where it was when
// the fit last looked (out of view, something else may have moved it since).
// `touched`: one of them is in its way. `unknown`: one of them was not
// visible then, so how the fit goes by it is not known.
fit::SpaceInputs with_moved(const fit::SpaceInputs &in, const std::vector<MovedBox> &moved, bool after, bool *touched,
			    bool *unknown)
{
	fit::SpaceInputs out = in;
	for (const MovedBox &m : moved) {
		bool found = false;
		for (size_t i = 0; i < out.ids.size() && i < out.boxes.size(); i++) {
			if (out.ids[i] != m.id)
				continue;
			found = true;
			Box &o = out.boxes[i];
			const bool said = i < out.said.size() && out.said[i];
			if (said) {
				// Where its own fit takes it: where that will take it.
				if (after) {
					o = m.has_claim ? m.claim : moved_box(o, m);
					home_after(out, i, m);
				}
				continue;
			}
			const Box here = i < out.at.size() ? follow_box(o, out.at[i], m.now) : o;
			if (!after) {
				o = here;
			} else if (m.has_claim) {
				o = m.claim;
				if (i < out.said.size())
					out.said[i] = true;
				home_after(out, i, m);
			} else {
				o = moved_box(here, m);
			}
		}
		if (found) {
			if (touched)
				*touched = true;
		} else if (unknown && std::find(out.visible.begin(), out.visible.end(), m.id) == out.visible.end()) {
			*unknown = true;
		}
	}
	return out;
}

// A fit's inputs once a home it has just taken has settled (FitEngine::motion):
// the space found round it then is its home's space, and from then on it is
// at home only while that space is found again and it keeps the gap there.
// What a fit will do for good is worked out from these.
fit::SpaceInputs settled(fit::SpaceInputs in)
{
	if (in.has_home && in.home_settling) {
		fit::SpaceOut out;
		fit::space_from(in, &out);
		in.home_settling = false;
		in.home_space = out.home_space;
		in.have_home_space = out.have_home_space;
	}
	return in;
}

// Protect the image a neighbour displays and the growth and lean it publishes to other fits.
bool same_outcome(const fit::SpaceInputs &a, const fit::SpaceInputs &b)
{
	fit::SpaceOut x, y;
	fit::space_from(a, &x);
	fit::space_from(b, &y);
	bool lean_before = false, lean_after = false;
	const Box before = fit::claim_from(a, &x, &lean_before);
	const Box after = fit::claim_from(b, &y, &lean_after);
	return same_space(before, after) && x.grows_x == y.grows_x && x.grows_y == y.grows_y &&
	       lean_before == lean_after;
}

// An item's own fit once it is handed the item where it is put (`m`: the
// item's box, or a part of it, before and after): it looks for its space
// round there and keeps the size it is given (`kx`, `ky` times the size it
// kept), and, with a home, takes its home there too (a home is where an item
// placed on purpose is put, FitEngine::placed), which then settles. (Also
// one whose home does not match the item now, a window of another shape say
// (`takes_home`): the home taken where it is put matches it.)
// `own_claim`: where it shows the item now.
fit::SpaceInputs handed_over(fit::SpaceInputs in, const MovedBox &m, const Box &own_claim, double kx, double ky)
{
	const Box box_to = moved_box(m.now, m);
	in.ax = box_to.x + box_to.w / 2.0;
	in.ay = box_to.y + box_to.h / 2.0;
	in.kept_w *= kx;
	in.kept_h *= ky;
	if (in.has_home || in.takes_home) {
		in.has_home = true;
		in.home = moved_box(own_claim, m);
		in.home_settling = true;
	}
	return settled(std::move(in));
}

// Whether every fit that has worked its space out (but the moved items' own)
// does the same with the moved items where they would be: the same space,
// and the same goal (so one at its home stays there).
FitCheck spaces_kept(const std::vector<fit::HeldSpace> &spaces, const std::vector<MovedBox> &moved, std::string *who,
		     const std::vector<int64_t> &empty = {})
{
	for (const fit::HeldSpace &held : spaces) {
		if (std::find(empty.begin(), empty.end(), held.item) != empty.end())
			continue;
		if (std::any_of(moved.begin(), moved.end(), [&](const MovedBox &m) { return m.id == held.item; }))
			continue; // a moved item's own fit: asked about by the caller
		if (!held.worked) {
			if (!held.shown)
				continue;
			if (who)
				*who = held.name;
			return FitCheck::Unknown;
		}
		bool touched = false, unknown = false;
		const fit::SpaceInputs before = settled(with_moved(held.in, moved, false, &touched, &unknown));
		if (unknown) {
			if (who)
				*who = held.name;
			return FitCheck::Unknown;
		}
		if (!touched)
			continue; // the moved items are not in its way at all
		// Worked out both ways by the fit's own working out, so the two
		// are alike in everything but the moved items.
		const fit::SpaceInputs after = settled(with_moved(held.in, moved, true, nullptr, nullptr));
		if (!same_outcome(before, after)) {
			if (who)
				*who = held.name;
			return FitCheck::Changes;
		}
	}
	return FitCheck::Kept;
}

const fit::HeldSpace *own_fit_of(const std::vector<fit::HeldSpace> &held, obs_sceneitem_t *item)
{
	const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
	const int64_t id = obs_sceneitem_get_id(item);
	for (const fit::HeldSpace &h : held)
		if (h.item == id && name && h.name == name)
			return &h;
	return nullptr;
}

// The fit that looks after the group an item is in, if one does.
const fit::HeldSpace *group_fit_of(const std::vector<fit::HeldSpace> &held, obs_source_t *scene, obs_sceneitem_t *item)
{
	obs_scene_t *s = scene_of(scene);
	obs_sceneitem_t *group = s ? obs_sceneitem_get_group(s, item) : nullptr;
	return group ? own_fit_of(held, group) : nullptr;
}

} // namespace

FitCheck copy_keeps_fits(obs_source_t *scene, obs_sceneitem_t *copy, const Box &from, const Box &to, std::string *who)
{
	if (!scene || !copy)
		return FitCheck::Kept;
	const std::vector<fit::HeldSpace> held = fit::held_spaces(scene);
	// In a group a fit looks after: the group changes with it, and that fit
	// fits it again.
	if (const fit::HeldSpace *group = group_fit_of(held, scene, copy)) {
		if (who)
			*who = group->name;
		return FitCheck::Changes;
	}
	const fit::HeldSpace *own = own_fit_of(held, copy);
	MovedBox m{obs_sceneitem_get_id(copy), from, to, false, from, false, Box{}};
	for (const fit::HeldSpace &h : held) {
		if (&h == own || h.worked || !h.shown)
			continue;
		// A fit there that has not worked its space out yet (its scene not
		// shown since OBS started): what it would do is not known.
		if (who)
			*who = h.name;
		return FitCheck::Unknown;
	}
	if (own) {
		if (!own->worked) {
			if (who)
				*who = own->name;
			return FitCheck::Unknown;
		}
		// Handed over, its own fit looks for its space round where it is put
		// and keeps the size it is given (or fills that space), or stays
		// there, at its home: the others then go by where it takes it.
		const bool scaled = !own->resizes && from.w >= 1.0 && from.h >= 1.0;
		const fit::SpaceInputs in = handed_over(own->in, m, fit::claim_from(own->in),
							scaled ? to.w / from.w : 1.0, scaled ? to.h / from.h : 1.0);
		fit::SpaceOut out;
		const Box goal = fit::space_from(in, &out);
		if (goal.w < 2.0 || goal.h < 2.0) {
			if (who)
				*who = own->name;
			return FitCheck::Changes;
		}
		m.has_claim = true;
		m.claim = fit::claim_from(in, &out);
		// (Its home where it is put, whether or not it stays at home.)
		m.has_home = in.has_home;
		m.home = in.home;
	}
	return spaces_kept(held, {m}, who);
}

int fit_to_space(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, double dock_gap, const Measure &dock_how,
		 std::vector<obs_sceneitem_t *> *fitted, FitToSpaceNote *note)
{
	if (!scene || items.empty())
		return 0;
	const double canvas_w = obs_source_get_width(scene), canvas_h = obs_source_get_height(scene);
	if (canvas_w < 2.0 || canvas_h < 2.0)
		return 0;

	// Top-most first, so the item in front has the first say.
	const std::vector<obs_sceneitem_t *> stack = stacking_order(scene);
	std::vector<obs_sceneitem_t *> order = items;
	auto height_of = [&](obs_sceneitem_t *item) {
		const auto found = std::find(stack.begin(), stack.end(), item);
		return found == stack.end() ? (long)stack.size() : (long)(found - stack.begin());
	};
	std::stable_sort(order.begin(), order.end(),
			 [&](obs_sceneitem_t *a, obs_sceneitem_t *b) { return height_of(a) > height_of(b); });
	// An entry may use a different measurement from the preceding selection.
	std::map<int, std::shared_ptr<fit::FitScan>> scans;
	// The spaces the scene's fits hold, asked once for the press.
	const std::vector<fit::HeldSpace> held = fit::held_spaces(scene);
	// The items changed so far in this press, for the check, and where what
	// shows of each now is (another fit's claim on one is from before).
	std::vector<MovedBox> moved;
	std::vector<std::pair<int64_t, Box>> placed_at;
	const Box canvas{0.0, 0.0, canvas_w, canvas_h};
	int changed = 0;
	auto add_name = [](std::vector<std::string> &list, const std::string &name) {
		if (!name.empty() && std::find(list.begin(), list.end(), name) == list.end())
			list.push_back(name);
	};
	auto gave_way = [&](const std::string &who) {
		if (note)
			add_name(note->gave_way_to, who);
	};
	for (obs_sceneitem_t *item : order) {
		if (!item || obs_sceneitem_locked(item) || !obs_sceneitem_visible(item))
			continue;
		const int64_t id = obs_sceneitem_get_id(item);
		const char *item_name = obs_source_get_name(obs_sceneitem_get_source(item));
		fit::FitSettings entry;
		std::string resizing_filter;
		const bool has_entry = fit::fit_settings_for(scene, item_name, entry, resizing_filter);
		const Measure how = has_entry ? Measure{entry.visible_pixels, entry.no_shadows} : dock_how;
		const double gap = has_entry ? entry.margin : std::max(0.0, dock_gap);
		const double tolerance = has_entry ? entry.tolerance : 8.0;
		// In a group a fit looks after: changing it changes the group, and
		// that fit would fit the group again. Left as it is.
		if (const fit::HeldSpace *group = group_fit_of(held, scene, item)) {
			if (note)
				note->refused++;
			gave_way(group->name);
			continue;
		}
		obs_scene_t *in_scene = scene_of(scene);
		obs_sceneitem_t *group_item = in_scene ? obs_sceneitem_get_group(in_scene, item) : nullptr;
		const int64_t group_id = group_item ? obs_sceneitem_get_id(group_item) : 0;
		// A fit of its own that resizes it decides its size: left to it. One
		// that keeps its size is worked out below.
		const fit::HeldSpace *own = own_fit_of(held, item);
		if (!resizing_filter.empty()) {
			if (note)
				add_name(note->own_fit, resizing_filter);
			continue;
		}
		if (own && (!own->worked || own->in.kept_w <= 0.0 || own->in.kept_h <= 0.0)) {
			if (note)
				add_name(note->unknown, own->name);
			continue;
		}
		struct Measurements {
			Box now, seen, picture;
			bool any = false;
			std::vector<fit::Obstacle> obstacles;
		};
		const int key = (how.visible_pixels ? 1 : 0) | (how.leave_out_shadows ? 2 : 0);
		auto &scan = scans[key];
		if (!scan)
			scan = std::make_shared<fit::FitScan>();
		auto result = std::make_shared<Measurements>();
		auto hold = std::make_shared<Held>(scene, std::vector<obs_sceneitem_t *>{item});
		if (!on_drawing_thread([hold, scan, item, how, result] {
			    obs_source_t *s = hold->scene;
			    result->any = measure_seen(s, item, Measure{true, how.leave_out_shadows}, result->seen);
			    if (!result->any)
				    return;
			    result->now = scene_box(s, item);
			    if (!how.visible_pixels)
				    result->seen = result->now;
			    if (how.visible_pixels && !how.leave_out_shadows) {
				    ShadowFree shadow;
				    if (shadow_free(obs_sceneitem_get_source(item), shadow)) {
					    obs_source_release(shadow.draw);
					    if (shadow.fixed_on_screen)
						    measure_seen(s, item, Measure{true, true}, result->picture);
				    }
			    }
			    scan->gather(s, item, &kFitTool);
			    scan->obstacles(s, item, {}, false, &kFitTool, how, result->obstacles);
		    })) {
			if (note)
				note->busy = true;
			break;
		}
		const Box now = result->now, seen = result->seen;
		if (!result->any || now.w < 1.0 || now.h < 1.0 || seen.w < 1.0 || seen.h < 1.0)
			continue;
		const double sx0 = std::max(seen.x, 0.0), sy0 = std::max(seen.y, 0.0);
		const double sx1 = std::min(seen.x + seen.w, canvas_w), sy1 = std::min(seen.y + seen.h, canvas_h);

		const auto held_by = [&](int64_t other) -> const fit::HeldSpace * {
			for (const fit::HeldSpace &h : held)
				if (h.item == other && &h != own)
					return &h;
			return nullptr;
		};
		std::vector<Box> boxes;
		std::vector<bool> firm, of_fit;
		std::vector<std::string> fitted_by; // whose spaces are in the way
		for (const fit::Obstacle &o : result->obstacles) {
			if (group_item && o.id == group_id)
				continue; // its own group: what it lies in
			// Changed earlier in this press: where it now is.
			const auto was = std::find_if(placed_at.begin(), placed_at.end(),
						      [&](const auto &p) { return p.first == o.id; });
			if (was != placed_at.end()) {
				boxes.push_back(was->second);
				firm.push_back(true);
				of_fit.push_back(false);
				continue;
			}
			const fit::HeldSpace *h = held_by(o.id);

			if (o.box.w < 1.0 || o.box.h < 1.0)
				continue;
			const bool under = o.box.x <= sx0 + 0.5 && o.box.y <= sy0 + 0.5 &&
					   o.box.x + o.box.w >= sx1 - 0.5 && o.box.y + o.box.h >= sy1 - 0.5;
			if (under)
				continue;
			boxes.push_back(o.box);
			firm.push_back(true);
			of_fit.push_back(h != nullptr);
			if (h)
				add_name(fitted_by, h->name);
		}
		// A briefly hidden fit can return on the next frame. Only its last
		// image is protected. Mode-held and long-hidden fits are filtered out.
		for (const fit::HeldSpace &h : held) {
			if (h.shown || &h == own || !h.worked)
				continue;
			const Box claim = fit::claim_from(h.in);
			if (claim.w < 1.0 || claim.h < 1.0)
				continue;
			boxes.push_back(claim);
			firm.push_back(true);
			of_fit.push_back(true);
			add_name(fitted_by, h.name);
		}
		const double cx = seen.x + seen.w / 2.0, cy = seen.y + seen.h / 2.0;
		const Box space = fit::free_space(canvas, cx, cy, boxes, gap, seen.w / seen.h, tolerance, &firm);
		// No room for it: when there would be but for the spaces of fitted
		// sources, it is left as it is for them, and the status line says so.
		const auto left_for_fits = [&] {
			if (fitted_by.empty())
				return;
			std::vector<Box> rest;
			std::vector<bool> rest_firm;
			for (size_t i = 0; i < boxes.size(); i++)
				if (!of_fit[i]) {
					rest.push_back(boxes[i]);
					rest_firm.push_back(firm[i]);
				}
			const Box without =
				fit::free_space(canvas, cx, cy, rest, gap, seen.w / seen.h, tolerance, &rest_firm);
			if (without.w < 2.0 || without.h < 2.0)
				return;
			if (note)
				note->refused++;
			for (const std::string &name : fitted_by)
				gave_way(name);
		};
		if (space.w < 2.0 || space.h < 2.0) {
			left_for_fits();
			continue;
		}
		// A shadow sized in screen pixels keeps its size whatever the item's:
		// when it counts, only the picture grows into what is left. Its
		// filter keeps the picture where it is when its room changes, a
		// tick later, so the place is set by the picture's: `rl`, `rt` is
		// where the picture starts in what is measured, on the screen.
		double room_w = 0.0, room_h = 0.0, rl = 0.0, rt = 0.0;
		bool fixed_room = false;
		// (With such a shadow, whether it counts here or not, the image in
		// the item's box, for the check: what lies round it keeps its size.)
		bool screen_room = false;
		Box image = now;
		ShadowFree free;
		if (shadow_free(obs_sceneitem_get_source(item), free)) {
			obs_source_release(free.draw);
			obs_source_t *source = obs_sceneitem_get_source(item);
			const double src_w = obs_source_get_width(source), src_h = obs_source_get_height(source);
			if (free.fixed_on_screen && src_w >= 1.0 && src_h >= 1.0) {
				const double fl = free.left * now.w / src_w, fr = free.right * now.w / src_w;
				const double ft = free.top * now.h / src_h, fb = free.bottom * now.h / src_h;
				if (fl + fr < now.w - 1.0 && ft + fb < now.h - 1.0) {
					screen_room = true;
					image = Box{now.x + fl, now.y + ft, now.w - fl - fr, now.h - ft - fb};
				}
			}
			if (free.fixed_on_screen && how.visible_pixels && !how.leave_out_shadows) {
				const Box picture = result->picture;
				if (picture.w >= 1.0 && picture.h >= 1.0 && picture.w < seen.w && picture.h < seen.h) {
					room_w = seen.w - picture.w;
					room_h = seen.h - picture.h;
					rl = picture.x - seen.x;
					rt = picture.y - seen.y;
					fixed_room = true;
				}
			} else if (free.fixed_on_screen && !how.visible_pixels && src_w >= 1.0 && src_h >= 1.0) {
				// By boxes, the box is measured with the room in it
				// whatever "Ignore shadows" says.
				room_w = (free.left + free.right) * now.w / src_w;
				room_h = (free.top + free.bottom) * now.h / src_h;
				rl = free.left * now.w / src_w;
				rt = free.top * now.h / src_h;
				fixed_room = room_w > 0.0 && room_h > 0.0 && room_w < seen.w && room_h < seen.h;
				if (!fixed_room)
					room_w = room_h = rl = rt = 0.0;
			}
		}
		// The full result: as large as the space lets it be, centred in it.
		const double full_k =
			std::min((space.w - room_w) / (seen.w - room_w), (space.h - room_h) / (seen.h - room_h));
		if (!(full_k > 0.0) || (has_entry && !entry.resize && full_k < 1.0 - 1e-4)) {
			left_for_fits();
			continue;
		}
		const double full_w = (seen.w - room_w) * full_k + room_w, full_h = (seen.h - room_h) * full_k + room_h;
		const Box full{space.x + (space.w - full_w) * (entry.toward_x + 1) / 2.0,
			       space.y + (space.h - full_h) * (entry.toward_y + 1) / 2.0, full_w, full_h};
		// A smaller result: `k` the scale (no larger than the space lets it
		// be), at (x, y) or, without a place, where `ax`, `ay` put it in the
		// space (-1, 0 or 1 a side: 0 the middle), kept in the space.
		struct Step {
			Box at;
			double k;
		};
		const auto step = [&](double k, int ax, int ay, const double *at) {
			k = std::min(k, full_k);
			Box b{0.0, 0.0, (seen.w - room_w) * k + room_w, (seen.h - room_h) * k + room_h};
			b.x = at ? at[0] : space.x + (space.w - b.w) * (ax + 1) / 2.0;
			b.y = at ? at[1] : space.y + (space.h - b.h) * (ay + 1) / 2.0;
			b.x = std::clamp(b.x, space.x, std::max(space.x, space.x + space.w - b.w));
			b.y = std::clamp(b.y, space.y, std::max(space.y, space.y + space.h - b.h));
			return Step{b, k};
		};
		const auto nothing_to_do = [&](const Step &st) {
			return std::fabs(st.k - 1.0) < 1e-4 && std::fabs(st.at.x - seen.x) < 0.25 &&
			       std::fabs(st.at.y - seen.y) < 0.25;
		};
		// The item put as a try says, for the check: the part of it that
		// scales (with a shadow in screen pixels, the image), before and
		// after.
		const auto moved_for = [&](const Step &st) {
			MovedBox m{id, seen, st.at, false, now, false, Box{}};
			if (screen_room) {
				// (`rl`, `rt` place the image's measured part in what is
				// measured, when the room counts; else they are 0.)
				const double vx = seen.x + rl, vy = seen.y + rt;
				m.keeps_outside = true;
				m.from = image;
				m.to = Box{st.at.x + rl - (vx - image.x) * st.k, st.at.y + rt - (vy - image.y) * st.k,
					   image.w * st.k, image.h * st.k};
			}
			return m;
		};
		// With a fit of its own that keeps its size: handed over, that fit
		// keeps the new size and looks for its space round where the item
		// is put, and puts it there (its claim). A try is taken where that
		// fit will put it, when that fit then finds the same space round it,
		// and it stays inside the space found here.
		const Box own_claim = own ? fit::claim_from(own->in) : Box{};
		struct Outcome {
			Step st;
			MovedBox m;
		};
		const auto outcome = [&](const Step &st, Outcome &out) {
			const MovedBox m = moved_for(st);
			if (!own) {
				out = Outcome{st, m};
				return true;
			}
			// (With a home, that fit takes its home where the item is put, and
			// keeps it there while the gap is kept.)
			fit::SpaceInputs in = handed_over(with_moved(own->in, moved, true, nullptr, nullptr), m,
							  own_claim, st.k, st.k);
			fit::SpaceOut found;
			const Box goal = fit::space_from(in, &found);
			if (goal.w < 2.0 || goal.h < 2.0 || own_claim.w < 1.0 || own_claim.h < 1.0)
				return false;
			const Box claim = fit::claim_from(in, &found);
			// (Put where it claims, its home, when it has one, is there.)
			const MovedBox by{id, own_claim, claim, screen_room, now, true, claim, in.has_home, claim};
			const Step put{moved_box(seen, by), st.k};
			const Box box_put = moved_box(now, by);
			in.ax = box_put.x + box_put.w / 2.0;
			in.ay = box_put.y + box_put.h / 2.0;
			if (!same_space(fit::space_from(in), goal))
				return false;
			if (put.at.x < space.x - 0.5 || put.at.y < space.y - 0.5 ||
			    put.at.x + put.at.w > space.x + space.w + 0.5 ||
			    put.at.y + put.at.h > space.y + space.h + 0.5)
				return false;
			out = Outcome{put, by};
			return true;
		};
		// The largest that changes no other fit's space: the full result
		// when it can be, else a twentieth of the way to its size now less
		// at a time (to half the full size when that is no larger), each
		// size centred, then part of the way from where it is, then against
		// each side and corner of the space. Measured once for the press;
		// each try only works the fits' spaces out again.
		{
			Outcome first;
			if (outcome(step(full_k, entry.toward_x, entry.toward_y, nullptr), first) && nothing_to_do(first.st))
				continue; // already fitted
		}
		const double k_low = full_k > 1.0 ? 1.0 : full_k * 0.5;
		std::string who, not_known;
		bool found = false, shrunk = false;
		Outcome chosen{};
		static const int kSides[8][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
		for (int n = 20; n >= 1 && !found && not_known.empty(); n--) {
			const double k = k_low + (full_k - k_low) * n / 20.0;
			std::vector<Step> tries{step(k, entry.toward_x, entry.toward_y, nullptr)};
			if (!has_entry && n < 20) {
				const double t = std::fabs(full_k - 1.0) > 1e-6 ? (k - 1.0) / (full_k - 1.0) : 1.0;
				const double at[2] = {seen.x + (full.x - seen.x) * t, seen.y + (full.y - seen.y) * t};
				tries.push_back(step(k, 0, 0, at));
			}
			if (!has_entry)
				for (const auto &side : kSides)
					tries.push_back(step(k, side[0], side[1], nullptr));
			for (const Step &st : tries) {
				Outcome oc;
				if (!outcome(st, oc) || nothing_to_do(oc.st))
					continue;
				std::vector<MovedBox> with = moved;
				with.push_back(oc.m);
				std::string blocked;
				const FitCheck check = spaces_kept(held, with, &blocked, scan->empty_ids());
				if (check == FitCheck::Kept) {
					chosen = oc;
					found = true;
					shrunk = n < 20;
					break;
				}
				if (check == FitCheck::Unknown) {
					not_known = blocked; // the same for every try
					break;
				}
				if (who.empty())
					who = blocked;
			}
		}
		if (!found) {
			// (Already as large as it can be: nothing to say.)
			if (!not_known.empty()) {
				if (note) {
					note->refused++;
					add_name(note->unknown, not_known);
				}
			} else if (!who.empty()) {
				if (note)
					note->refused++;
				gave_way(who);
			} else if (own && note) {
				gave_way(own->name);
			}
			continue;
		}
		if (shrunk) {
			if (note)
				note->shrunk++;
			gave_way(who);
		}
		const double k = chosen.st.k, to_x = chosen.st.at.x, to_y = chosen.st.at.y;
		obs_sceneitem_defer_update_begin(item);
		scale_item_by(item, k);
		obs_sceneitem_defer_update_end(item);
		refresh_item(item);
		double at_x = 0.0, at_y = 0.0;
		const Box resized = scene_box(scene, item);
		if (fixed_room && how.visible_pixels) {
			at_x = resized.x + (result->picture.x - now.x) * resized.w / now.w - rl;
			at_y = resized.y + (result->picture.y - now.y) * resized.h / now.h - rt;
		} else if (fixed_room) {
			// The picture's place in the box grows with the box (the
			// room in source pixels is still the old one).
			at_x = resized.x + rl * resized.w / now.w - rl;
			at_y = resized.y + rt * resized.h / now.h - rt;
		} else {
			// What shows is the same part of the box at any size.
			const double rx = resized.w / now.w, ry = resized.h / now.h;
			at_x = resized.x + (seen.x - now.x) * rx;
			at_y = resized.y + (seen.y - now.y) * ry;
		}
		shift_item(scene, item, to_x - at_x, to_y - at_y);
		refresh_item(item);
		moved.push_back(chosen.m);
		placed_at.emplace_back(id, chosen.st.at);
		// An item a Fit to Space looks after is told it was placed on
		// purpose by the caller, once it is there (it may glide).
		if (fitted)
			fitted->push_back(item);
		changed++;
	}
	return changed;
}

} // namespace stagekit
