/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Fit Item: one capture, any shape, placed by the scene. A widescreen game
// fills the box; an emulator's squarer picture floats in the middle of it
// with a margin, with nothing to switch by hand between the two.

#include "fit-item.hpp"
#include "../common/timing.hpp"

#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../dock/arrange-tools.hpp"
#include "../dock/arrange.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Saved in settings: append only.
enum class BoxKind { WholeScene = 0, Custom = 1, FreeSpace = 2 };

struct Settings {
	std::string item;
	BoxKind box = BoxKind::WholeScene;
	double x = 0, y = 0, w = 1920, h = 1080;
	double margin = 80;
	double tolerance = 8;          // free space: pictures this close in size count as equal
	bool resize = true;            // off: the item keeps its size and is only moved, centred in the space
	bool visible_pixels = true;    // other items are measured by what they draw, not their boxes
	bool no_shadows = true;        // and shadows, glows and outlines do not count, its own included
	std::vector<std::string> skip; // items that are never in the way
	bool have_anchor = false;      // the free space is the one round this point
	double anchor_x = 0, anchor_y = 0;
	double glide_s = 0.3;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

// The largest clear space for a picture of the shape given (width over
// height) that holds the point (ax, ay): inside `within` less the gap, and
// the gap away from everything in `others`. Something the point is inside (a
// background, a frame round the picture) is not in the way.
Box free_space(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape,
	       double tolerance)
{
	const double l0 = within.x + gap, t0 = within.y + gap;
	const double r0 = within.x + within.w - gap, b0 = within.y + within.h - gap;
	ax = std::clamp(ax, l0, std::max(l0, r0));
	ay = std::clamp(ay, t0, std::max(t0, b0));
	std::vector<Box> in_way;
	std::vector<double> lefts{l0}, rights{r0}, tops{t0}, bottoms{b0};
	for (const Box &o : others) {
		if (ax > o.x && ax < o.x + o.w && ay > o.y && ay < o.y + o.h)
			continue;
		const Box grown{o.x - gap, o.y - gap, o.w + 2.0 * gap, o.h + 2.0 * gap};
		if (grown.x >= r0 || grown.x + grown.w <= l0 || grown.y >= b0 || grown.y + grown.h <= t0)
			continue;
		in_way.push_back(grown);
		if (grown.x + grown.w <= ax)
			lefts.push_back(grown.x + grown.w);
		if (grown.x >= ax)
			rights.push_back(grown.x);
		if (grown.y + grown.h <= ay)
			tops.push_back(grown.y + grown.h);
		if (grown.y >= ay)
			bottoms.push_back(grown.y);
	}
	// Every clear space, with how big a picture it holds (`k`).
	struct Space {
		Box box;
		double k, area;
	};
	std::vector<Space> spaces;
	double best_k = -1.0;
	for (double l : lefts)
		for (double r : rights)
			for (double t : tops)
				for (double b : bottoms) {
					const double w = r - l, h = b - t;
					if (w < 2.0 || h < 2.0)
						continue;
					bool clear = true;
					for (const Box &o : in_way)
						if (o.x < r - 0.01 && o.x + o.w > l + 0.01 && o.y < b - 0.01 &&
						    o.y + o.h > t + 0.01) {
							clear = false;
							break;
						}
					if (!clear)
						continue;
					const double k = shape > 0.0 ? std::min(w / shape, h) : w * h;
					spaces.push_back({Box{l, t, w, h}, k, w * h});
					best_k = std::max(best_k, k);
				}
	// The biggest picture wins; among spaces whose pictures are as good as
	// the same size (a few pixels: what shows of a small item is measured
	// to about that), the roomiest. Without that, a widget read as 3 px
	// wider than the column above it cost the owner's game the whole strip
	// beside the widget: the space stopped above it to keep those 3 px.
	// How many pixels count as "the same size" is the filter's Tolerance.
	const double near = shape > 0.0 ? std::max(0.01, tolerance) : 0.01;
	Box best{};
	double best_area = -1.0;
	for (const Space &one : spaces)
		if (one.k >= best_k - near && one.area > best_area) {
			best = one.box;
			best_area = one.area;
		}
	return best;
}

class FitItem {
public:
	FitItem(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }

	void update(obs_data_t *settings)
	{
		Settings s;
		s.item = obs_data_get_string(settings, "item");
		s.box = obs_data_get_int(settings, "box") == 1 ? BoxKind::Custom : BoxKind::WholeScene;
		s.x = obs_data_get_double(settings, "box_x");
		s.y = obs_data_get_double(settings, "box_y");
		s.w = std::max(2.0, obs_data_get_double(settings, "box_width"));
		s.h = std::max(2.0, obs_data_get_double(settings, "box_height"));
		s.margin = std::clamp(obs_data_get_double(settings, "margin"), 0.0, 2000.0);
		s.tolerance = std::clamp(obs_data_get_double(settings, "tolerance"), 0.0, 200.0);
		s.resize = obs_data_get_bool(settings, "resize");
		have_keep_ = false; // the size to keep is read from the item afresh
		const long long kind = obs_data_get_int(settings, "box");
		if (kind == 2)
			s.box = BoxKind::FreeSpace;
		// The filter's own two measuring options. Before they existed there
		// was one tick box that followed the dock; unticked, it meant boxes.
		s.visible_pixels = obs_data_get_bool(settings, "visible_pixels");
		s.no_shadows = obs_data_get_bool(settings, "no_shadows");
		if (obs_data_has_user_value(settings, "as_arranged") && !obs_data_get_bool(settings, "as_arranged") &&
		    !obs_data_has_user_value(settings, "visible_pixels"))
			s.visible_pixels = false;
		s.skip = pick_list_read(settings, "skip", false);
		s.have_anchor = obs_data_get_bool(settings, "have_anchor");
		s.anchor_x = obs_data_get_double(settings, "anchor_x");
		s.anchor_y = obs_data_get_double(settings, "anchor_y");
		s.glide_s = std::clamp((double)obs_data_get_int(settings, "glide_ms"), 0.0, 10000.0) / 1000.0;
		s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		look_again_ = true;
		std::lock_guard lock(mutex_);
		settings_ = s;
	}

	void attach(obs_source_t *parent) { parent_ = parent; }
	void detach() { parent_ = nullptr; }
	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

	void tick(float seconds)
	{
		if (!parent_ || !obs_source_enabled(source_))
			return;
		// Not on a group: its size is whatever is in it, so there is no
		// space to fit into, and moving an item inside it moves the space.
		if (obs_source_is_group(parent_))
			return;
		Settings s;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
		}
		// Held for the length of this tick: OBS's window may remove the item
		// at any moment from its own thread.
		struct Held {
			obs_sceneitem_t *item;
			~Held()
			{
				if (item)
					obs_sceneitem_release(item);
			}
		} held{find_item(parent_, s.item)};
		obs_sceneitem_t *item = held.item;
		if (item)
			obs_sceneitem_addref(item);
		obs_source_t *shown = item ? obs_sceneitem_get_source(item) : nullptr;
		const double sw = shown ? obs_source_get_width(shown) : 0.0,
			     sh = shown ? obs_source_get_height(shown) : 0.0;
		if (sw < 2.0 || sh < 2.0)
			return; // nothing to fit yet

		// What is fitted is the picture, without the room a shadow takes
		// around it and without what the item crops away.
		double room[4];
		shadow_room(shown, room);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		// With shadows counted as part of an item (its own tick box), the
		// shadow is fitted too: it keeps the margin, not the picture.
		const Measure how{s.visible_pixels, s.no_shadows};
		// So is one asked to stay on screen ("Keep it on screen" in its
		// shadow filter): a picture that fills the box makes room for it.
		const bool shadow_in_view = shadow_kept_in_view(shown);
		const double bare_w = sw - std::max(room[0], (double)crop.left) - std::max(room[2], (double)crop.right);
		const double bare_h = sh - std::max(room[1], (double)crop.top) - std::max(room[3], (double)crop.bottom);
		if (!s.no_shadows || shadow_in_view)
			room[0] = room[1] = room[2] = room[3] = 0.0;
		const double left = std::max(room[0], (double)crop.left), top = std::max(room[1], (double)crop.top);
		const double right = std::max(room[2], (double)crop.right),
			     bottom = std::max(room[3], (double)crop.bottom);
		const double pw = sw - left - right, ph = sh - top - bottom;
		if (pw < 2.0 || ph < 2.0)
			return;

		double bx = 0.0, by = 0.0, bw = obs_source_get_width(parent_), bh = obs_source_get_height(parent_);
		if (s.box == BoxKind::Custom) {
			bx = s.x;
			by = s.y;
			bw = s.w;
			bh = s.h;
		}
		if (bw < 2.0 || bh < 2.0)
			return;
		// "Resize it to fit" unticked: the picture keeps the size it has in
		// the scene and is only moved, to the middle of its space. That size
		// is read from the item once, and again after it is resized by hand.
		if (!s.resize) {
			vec2 b_now;
			obs_sceneitem_get_bounds(item, &b_now);
			const bool boxed = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
			if (have_keep_ && boxed && have_set_ &&
			    (std::fabs(b_now.x - set_bw_) > 0.5 || std::fabs(b_now.y - set_bh_) > 0.5))
				have_keep_ = false;
			if (!have_keep_) {
				double kx = 1.0, ky = 1.0;
				if (boxed) {
					const double cw = sw - crop.left - crop.right, ch = sh - crop.top - crop.bottom;
					if (cw >= 1.0 && ch >= 1.0)
						kx = ky = std::min(b_now.x / cw, b_now.y / ch);
				} else {
					vec2 scale;
					obs_sceneitem_get_scale(item, &scale);
					kx = std::fabs(scale.x);
					ky = std::fabs(scale.y);
				}
				const double kept = std::min(kx, ky);
				if (kept > 0.0) {
					keep_w_ = pw * kept;
					keep_h_ = ph * kept;
					have_keep_ = true;
				}
			}
		}
		const double picture = pw / ph;
		// The screen (or the box) itself, before a free space is taken in it.
		const Box outer{bx, by, bw, bh};
		bool free = s.box == BoxKind::FreeSpace;
		if (free) {
			Box space;
			if (!free_box(item, s, how, Box{bx, by, bw, bh}, picture, seconds, space))
				return; // being moved by hand, or no room at all
			bx = space.x;
			by = space.y;
			bw = space.w;
			bh = space.h;
		} else {
			have_cur_ = false;
		}
		// The same shape as the box fills it; any other shape keeps clear of
		// its edges by the margin. The free space already has the margin
		// taken off all round.
		const double box = bw / bh;
		// (The shape that counts is the picture's, also when its shadow is
		// fitted with it.)
		const double bare = bare_w >= 2.0 && bare_h >= 2.0 ? bare_w / bare_h : picture;
		const bool fills = !free && std::fabs(bare - box) / box < 0.01;
		double margin = fills || free ? 0.0 : std::min({s.margin, bw / 2.0 - 1.0, bh / 2.0 - 1.0});
		if (!s.resize && have_keep_) {
			// Its own size, in the middle of the room it has: the gaps are
			// balanced, left with right and top with bottom.
			bx += margin + (bw - 2.0 * margin - keep_w_) / 2.0;
			by += margin + (bh - 2.0 * margin - keep_h_) / 2.0;
			// A space smaller than the item cannot hold it, and its
			// middle may then lie off the screen: the item stays the gap
			// inside the screen's edges whatever else gives way. (The
			// owner's chat box was pushed off the canvas by a game that
			// grew beside it; kept on the canvas, the game makes room.)
			const double gap = std::min({s.margin, outer.w / 2.0 - 1.0, outer.h / 2.0 - 1.0});
			auto inside = [](double at, double size, double from, double room) {
				return room >= size ? std::clamp(at, from, from + room - size)
						    : from + (room - size) / 2.0;
			};
			bx = inside(bx, keep_w_, outer.x + gap, outer.w - 2.0 * gap);
			by = inside(by, keep_h_, outer.y + gap, outer.h - 2.0 * gap);
			bw = keep_w_;
			bh = keep_h_;
			margin = 0.0;
		}
		const double aw = bw - 2.0 * margin, ah = bh - 2.0 * margin;
		const double k = std::min(aw / pw, ah / ph);
		// The fit itself is left to OBS's bounding box, which OBS works out
		// afresh for every frame it draws. The box is the space the picture
		// may take, grown by whatever the item draws round the picture (a
		// shadow's room) at this scale, so that OBS's own "scale to inner
		// bounds" lands the picture exactly in that space, centred. While the
		// picture only changes size, as a window does while it is dragged
		// larger, the box stays as it is and OBS keeps the picture fitted on
		// every frame: nothing here has to catch up a frame later.
		const double room_l = left - crop.left, room_t = top - crop.top;
		const double room_r = right - crop.right, room_b = bottom - crop.bottom;
		const double want_w = aw + (room_l + room_r) * k, want_h = ah + (room_t + room_b) * k;
		const double x = bx + margin - room_l * k;
		const double y = by + margin - room_t * k;

		vec2 pos, bounds;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_bounds(item, &bounds);
		const bool placed = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER &&
				    obs_sceneitem_get_alignment(item) == (OBS_ALIGN_LEFT | OBS_ALIGN_TOP) &&
				    obs_sceneitem_get_bounds_alignment(item) == OBS_ALIGN_CENTER &&
				    std::fabs(pos.x - x) < 0.05 && std::fabs(pos.y - y) < 0.05 &&
				    std::fabs(bounds.x - want_w) < 0.05 && std::fabs(bounds.y - want_h) < 0.05 &&
				    obs_sceneitem_get_rot(item) == 0.0f;
		set_x_ = x;
		set_y_ = y;
		set_bw_ = want_w;
		set_bh_ = want_h;
		have_set_ = true;
		if (placed)
			return;
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_rot(item, 0.0f);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
		obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
		vec2_set(&bounds, (float)want_w, (float)want_h);
		obs_sceneitem_set_bounds(item, &bounds);
		vec2_set(&pos, (float)x, (float)y);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_defer_update_end(item);
	}

private:
	// The free space round the item's place, gliding to it when it changes.
	// False while there is nothing to do this tick.
	bool free_box(obs_sceneitem_t *item, Settings &s, const Measure &how, const Box &within, double shape,
		      float seconds, Box &space)
	{
		if (measuring_stopped())
			return false;
		// Only for a scene that is being shown somewhere. Every scene's
		// filters run all the time, and looking at the items of scenes nobody
		// sees would cost the picture on screen its smoothness for nothing.
		if (!obs_source_showing(parent_)) {
			if (!have_cur_)
				return false;
			space = cur_;
			return true;
		}
		// Moved by something else (dragged in the preview, sent somewhere by
		// a key): left alone while it moves, then its new place is where the
		// free space is looked for.
		vec2 pos;
		obs_sceneitem_get_pos(item, &pos);
		if (have_set_ && !moving_ && (std::fabs(pos.x - set_x_) > 0.5 || std::fabs(pos.y - set_y_) > 0.5)) {
			moving_ = true;
			still_for_ = 0.0;
		}
		if (moving_) {
			if (std::fabs(pos.x - seen_x_) > 0.05 || std::fabs(pos.y - seen_y_) > 0.05)
				still_for_ = 0.0;
			else
				still_for_ += seconds;
			seen_x_ = pos.x;
			seen_y_ = pos.y;
			if (still_for_ < 0.4)
				return false;
			moving_ = false;
			s.have_anchor = false; // taken afresh below
			have_cur_ = false;     // and no glide from where it was let go
		}
		if (!s.have_anchor) {
			const Box at = item_box(item);
			s.anchor_x = at.x + at.w / 2.0;
			s.anchor_y = at.y + at.h / 2.0;
			s.have_anchor = true;
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_bool(settings, "have_anchor", true);
			obs_data_set_double(settings, "anchor_x", s.anchor_x);
			obs_data_set_double(settings, "anchor_y", s.anchor_y);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			settings_.have_anchor = true;
			settings_.anchor_x = s.anchor_x;
			settings_.anchor_y = s.anchor_y;
			look_again_ = true;
		}

		// What is in the way. Drawing an item to see what shows in it is
		// costly (it stalls the picture for a moment), so it is done as
		// seldom as can be: once for each item when it is first met, and
		// after that one item a second, in turn, while nothing is moving,
		// which is how a widget that begins to show something is noticed.
		// An item that only moves or changes size is followed by its box,
		// which costs nothing: what shows in it keeps its place in the box.
		std::vector<obs_sceneitem_t *> others;
		struct Gather {
			std::vector<obs_sceneitem_t *> *out;
			obs_sceneitem_t *self;
			const std::vector<std::string> *skip;
		} gather{&others, item, &s.skip};
		static const auto add = [](obs_scene_t *, obs_sceneitem_t *other, void *param) {
			auto *g = static_cast<Gather *>(param);
			if (other == g->self || !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other))
				return true;
			const char *name = obs_source_get_name(obs_sceneitem_get_source(other));
			if (name && std::find(g->skip->begin(), g->skip->end(), name) != g->skip->end())
				return true;
			g->out->push_back(other);
			return true;
		};
		obs_scene_enum_items(
			obs_scene_from_source(parent_),
			[](obs_scene_t *sc, obs_sceneitem_t *other, void *param) {
				auto *g = static_cast<Gather *>(param);
				if (other == g->self)
					return true;
				if (obs_sceneitem_is_group(other) && obs_sceneitem_visible(other))
					obs_sceneitem_group_enum_items(other, add, param);
				add(sc, other, param);
				return true;
			},
			&gather);
		const Measure by = how;
		auto look = [&](obs_sceneitem_t *other) {
			Seen seen;
			seen.there = measure_seen(parent_, other, by, seen.box);
			seen.item_at = scene_box(parent_, other);
			seen_[obs_sceneitem_get_id(other)] = seen;
		};
		if (look_again_) {
			look_again_ = false;
			seen_.clear();
			turn_ = 0;
			since_look_ = 0.0;
		}
		// Where each is now: what was seen in it, carried along with its box.
		std::vector<Box> boxes;
		for (obs_sceneitem_t *other : others) {
			auto found = seen_.find(obs_sceneitem_get_id(other));
			if (found == seen_.end()) {
				look(other);
				found = seen_.find(obs_sceneitem_get_id(other));
			}
			const Seen &seen = found->second;
			if (!seen.there)
				continue;
			const Box now = scene_box(parent_, other);
			if (seen.item_at.w < 1.0 || seen.item_at.h < 1.0 || now.w < 1.0 || now.h < 1.0) {
				boxes.push_back(seen.box);
				continue;
			}
			const double kx = now.w / seen.item_at.w, ky = now.h / seen.item_at.h;
			boxes.push_back(Box{now.x + (seen.box.x - seen.item_at.x) * kx,
					    now.y + (seen.box.y - seen.item_at.y) * ky, seen.box.w * kx,
					    seen.box.h * ky});
		}
		auto same = [](const std::vector<Box> &a, const std::vector<Box> &b) {
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); i++)
				if (std::fabs(a[i].x - b[i].x) > 0.25 || std::fabs(a[i].y - b[i].y) > 0.25 ||
				    std::fabs(a[i].w - b[i].w) > 0.25 || std::fabs(a[i].h - b[i].h) > 0.25)
					return false;
			return true;
		};
		if (!same(boxes, boxes_)) {
			// Something is moving. The fitted item waits for it to come to
			// rest and then takes its new space in one glide: chasing a
			// moving item would resize the picture (and redo its shadow)
			// on every frame of the move. No looking meanwhile either.
			boxes_ = boxes;
			unsettled_ = true;
			calm_for_ = 0.0;
			since_look_ = 0.0;
		} else if (unsettled_) {
			if ((calm_for_ += seconds) >= 0.15) {
				unsettled_ = false;
				changed_ = true;
			}
		} else if ((since_look_ += seconds) >= 1.0 && !others.empty() && glide_ >= 1.0) {
			since_look_ = 0.0;
			turn_ = (turn_ + 1) % others.size();
			look(others[turn_]);
		}

		// The shape counts once it has stopped changing.
		if (std::fabs(shape - shape_seen_) > 0.01 * shape) {
			shape_seen_ = shape;
			shape_for_ = 0.0;
		} else if (shape_for_ < 0.5 && (shape_for_ += seconds) >= 0.5 &&
			   std::fabs(shape - shape_used_) > 0.01 * shape) {
			changed_ = true;
		}
		// The gap or the tolerance set anew: the space is worked out again.
		if (s.margin != used_margin_ || s.tolerance != used_tolerance_) {
			used_margin_ = s.margin;
			used_tolerance_ = s.tolerance;
			changed_ = true;
		}
		if (changed_ || !have_goal_) {
			changed_ = false;
			if (!have_goal_ || shape_for_ >= 0.5 || shape_used_ <= 0.0)
				shape_used_ = shape;
			const Box goal =
				free_space(within, s.anchor_x, s.anchor_y, boxes_, s.margin, shape_used_, s.tolerance);
			if (goal.w < 2.0 || goal.h < 2.0)
				return false;
			if (!have_goal_ || std::fabs(goal.x - goal_.x) > 0.05 || std::fabs(goal.y - goal_.y) > 0.05 ||
			    std::fabs(goal.w - goal_.w) > 0.05 || std::fabs(goal.h - goal_.h) > 0.05) {
				from_ = have_cur_ ? cur_ : goal;
				goal_ = goal;
				glide_ = have_cur_ && s.glide_s > 0.0 ? 0.0 : 1.0;
				have_goal_ = true;
			}
		}
		if (glide_ < 1.0) {
			glide_ = s.glide_s > 0.0 ? std::min(1.0, glide_ + seconds / s.glide_s) : 1.0;
			const double t = ease(s.curve, s.mode, glide_);
			cur_ = Box{from_.x + (goal_.x - from_.x) * t, from_.y + (goal_.y - from_.y) * t,
				   std::max(2.0, from_.w + (goal_.w - from_.w) * t),
				   std::max(2.0, from_.h + (goal_.h - from_.h) * t)};
		} else {
			cur_ = goal_;
		}
		have_cur_ = true;
		space = cur_;
		return true;
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	Settings settings_;
	// The free space: used on the tick only, but for the two flags.
	struct Seen {
		Box box;     // what shows in it, on the canvas, when it was looked at
		Box item_at; // and where its box was then
		bool there = false;
	};
	std::vector<Box> boxes_; // what is in the way now
	std::map<int64_t, Seen> seen_;
	bool look_again_ = true, changed_ = false, unsettled_ = false;
	double calm_for_ = 0.0;
	double since_look_ = 0.0;
	size_t turn_ = 0;
	double shape_seen_ = 0.0, shape_used_ = 0.0, shape_for_ = 0.0;
	double used_margin_ = -1.0, used_tolerance_ = -1.0;
	double set_bw_ = 0.0, set_bh_ = 0.0; // the bounding box last given to the item
	double keep_w_ = 0.0, keep_h_ = 0.0; // "Resize it to fit" off: the picture's size, kept
	bool have_keep_ = false;
	Box goal_, from_, cur_;
	bool have_goal_ = false, have_cur_ = false;
	double glide_ = 1.0;
	bool have_set_ = false, moving_ = false;
	double set_x_ = 0.0, set_y_ = 0.0, seen_x_ = 0.0, seen_y_ = 0.0, still_for_ = 0.0;
};

bool box_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const long long kind = obs_data_get_int(settings, "box");
	for (const char *name : {"box_x", "box_y", "box_width", "box_height"})
		obs_property_set_visible(obs_properties_get(props, name), kind == 1);
	for (const char *name : {"skip|alist", "glide_ms", "ease_curve", "ease_mode", "visible_pixels", "tolerance"})
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, kind == 2);
	obs_property_set_description(obs_properties_get(props, "margin"),
				     obs_module_text(kind == 2 ? "Fit.Gap" : "Fit.Margin"));
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<FitItem *>(data);
	obs_properties_t *props = obs_properties_create();
	// On a group it has nothing to go by (a group is only as large as what
	// is in it) and does nothing; said here, where it will be looked for.
	if (self && self->parent() && obs_source_is_group(self->parent())) {
		obs_property_t *wrong =
			obs_properties_add_text(props, "on_group", obs_module_text("Fit.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(wrong, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(wrong, true);
	}
	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Fit.Item"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(item, obs_module_text("Fit.Item.None"), "");
	add_scene_items_to_list(self ? self->parent() : nullptr, item);

	obs_property_t *box = obs_properties_add_list(props, "box", obs_module_text("Fit.Box"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Scene"), (int)BoxKind::WholeScene);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Custom"), (int)BoxKind::Custom);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Free"), (int)BoxKind::FreeSpace);
	obs_property_set_long_description(box, obs_module_text("Fit.Box.Tip"));
	auto number = [&](const char *name, const char *text) {
		obs_property_t *p =
			obs_properties_add_float(props, name, obs_module_text(text), -16384.0, 16384.0, 1.0);
		obs_property_float_set_suffix(p, " px");
	};
	number("box_x", "Fit.BoxX");
	number("box_y", "Fit.BoxY");
	number("box_width", "Fit.BoxWidth");
	number("box_height", "Fit.BoxHeight");

	obs_property_t *margin =
		obs_properties_add_float_slider(props, "margin", obs_module_text("Fit.Margin"), 0.0, 400.0, 1.0);
	obs_property_float_set_suffix(margin, " px");
	obs_property_set_long_description(margin, obs_module_text("Fit.Margin.Tip"));
	obs_property_t *resize = obs_properties_add_bool(props, "resize", obs_module_text("Fit.Resize"));
	obs_property_set_long_description(resize, obs_module_text("Fit.Resize.Tip"));
	obs_property_t *tolerance =
		obs_properties_add_float_slider(props, "tolerance", obs_module_text("Fit.Tolerance"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(tolerance, " px");
	obs_property_set_long_description(tolerance, obs_module_text("Fit.Tolerance.Tip"));
	obs_property_t *visible_pixels =
		obs_properties_add_bool(props, "visible_pixels", obs_module_text("Measure.Visible"));
	obs_property_set_long_description(visible_pixels, obs_module_text("Fit.Visible.Tip"));
	obs_property_t *no_shadows = obs_properties_add_bool(props, "no_shadows", obs_module_text("Measure.NoShadows"));
	obs_property_set_long_description(no_shadows, obs_module_text("Fit.NoShadows.Tip"));
	obs_property_t *glide = obs_properties_add_int(props, "glide_ms", obs_module_text("Fit.Glide"), 0, 10000, 50);
	obs_property_int_set_suffix(glide, " ms");
	obs_property_set_long_description(glide, obs_module_text("Fit.Glide.Tip"));
	add_easing_properties(props, "");

	obs_source_t *parent = self ? self->parent() : nullptr;
	PickList skip;
	skip.setting = "skip";
	skip.title = "Fit.Skip";
	skip.add = "List.AddItem";
	skip.tip = "Fit.Skip.Tip";
	skip.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, skip);
	// Last, so that everything it shows and hides is there.
	obs_property_set_modified_callback(box, box_modified);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "box", (int)BoxKind::WholeScene);
	obs_data_set_default_double(settings, "box_x", 0.0);
	obs_data_set_default_double(settings, "box_y", 0.0);
	obs_data_set_default_double(settings, "box_width", have ? ovi.base_width : 1920);
	obs_data_set_default_double(settings, "box_height", have ? ovi.base_height : 1080);
	obs_data_set_default_double(settings, "margin", 80.0);
	obs_data_set_default_double(settings, "tolerance", 8.0);
	obs_data_set_default_bool(settings, "resize", true);
	obs_data_set_default_bool(settings, "visible_pixels", true);
	obs_data_set_default_bool(settings, "no_shadows", true);
	obs_data_set_default_int(settings, "glide_ms", 300);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

void register_fit_item()
{
	obs_source_info info = {};
	info.id = kFitItemId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Fit.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new FitItem(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<FitItem *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<FitItem *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<FitItem *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<FitItem *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<FitItem *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<FitItem *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
