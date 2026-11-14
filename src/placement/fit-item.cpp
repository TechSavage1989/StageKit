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
	bool as_arranged = true;       // measure by the dock's Arrange options
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
Box free_space(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape)
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
	Box best{};
	double best_k = -1.0, best_area = -1.0;
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
					if (k > best_k + 0.01 || (std::fabs(k - best_k) <= 0.01 && w * h > best_area)) {
						best = Box{l, t, w, h};
						best_k = k;
						best_area = w * h;
					}
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
		const long long kind = obs_data_get_int(settings, "box");
		if (kind == 2)
			s.box = BoxKind::FreeSpace;
		s.as_arranged = obs_data_get_bool(settings, "as_arranged");
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

	void attach(obs_source_t *parent)
	{
		parent_ = parent;
		// Something added, removed, shown, hidden or moved in the scene:
		// the free space is looked at again once things are quiet.
		signal_handler_t *handler = obs_source_get_signal_handler(parent);
		for (const char *name : {"item_add", "item_remove", "item_visible", "item_transform"})
			signal_handler_connect(handler, name, scene_changed, this);
	}
	void detach()
	{
		if (parent_) {
			signal_handler_t *handler = obs_source_get_signal_handler(parent_);
			for (const char *name : {"item_add", "item_remove", "item_visible", "item_transform"})
				signal_handler_disconnect(handler, name, scene_changed, this);
		}
		parent_ = nullptr;
	}
	~FitItem() { detach(); }
	static void scene_changed(void *data, calldata_t *cd)
	{
		auto *self = static_cast<FitItem *>(data);
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		if (item && obs_sceneitem_get_id(item) == self->own_id_.load())
			return; // its own moves
		self->stirred_ = true;
	}
	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

	void tick(float seconds)
	{
		if (!parent_ || !obs_source_enabled(source_))
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
		// With the Arrange options counting shadows as part of an item, the
		// shadow is fitted too: it keeps the margin, not the picture.
		const Measure how = arrange_options().how;
		if (s.as_arranged && !how.leave_out_shadows)
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
		const double picture = pw / ph;
		bool free = s.box == BoxKind::FreeSpace;
		if (free) {
			own_id_ = obs_sceneitem_get_id(item);
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
		const bool fills = !free && std::fabs(picture - box) / box < 0.01;
		const double margin = fills || free ? 0.0 : std::min({s.margin, bw / 2.0 - 1.0, bh / 2.0 - 1.0});
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

		// What is in the way. Everything is measured in one go at the start
		// and once things are quiet after a change in the scene; otherwise
		// one item every quarter of a second, in turn, which is how a widget
		// that begins to show something is noticed.
		if (stirred_.exchange(false)) {
			quiet_for_ = 0.0;
			waiting_ = true;
		} else if (waiting_) {
			quiet_for_ += seconds;
			if (quiet_for_ >= 0.3) {
				waiting_ = false;
				look_again_ = true;
			}
		}
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
		const Measure by = s.as_arranged ? how : Measure{false, false};
		auto look = [&](obs_sceneitem_t *other) {
			Seen seen;
			seen.there = measure_seen(parent_, other, by, seen.box);
			seen.kept = true;
			auto found = seen_.find(obs_sceneitem_get_id(other));
			if (found == seen_.end() || found->second.there != seen.there ||
			    std::fabs(found->second.box.x - seen.box.x) > 0.5 ||
			    std::fabs(found->second.box.y - seen.box.y) > 0.5 ||
			    std::fabs(found->second.box.w - seen.box.w) > 0.5 ||
			    std::fabs(found->second.box.h - seen.box.h) > 0.5)
				changed_ = true;
			seen_[obs_sceneitem_get_id(other)] = seen;
		};
		if (look_again_) {
			look_again_ = false;
			for (auto &entry : seen_)
				entry.second.kept = false;
			for (obs_sceneitem_t *other : others)
				look(other);
			for (auto it = seen_.begin(); it != seen_.end();)
				it = it->second.kept ? std::next(it) : seen_.erase(it);
			changed_ = true;
			turn_ = 0;
			since_look_ = 0.0;
		} else if ((since_look_ += seconds) >= 0.25 && !others.empty()) {
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
		if (changed_ || !have_goal_) {
			changed_ = false;
			if (!have_goal_ || shape_for_ >= 0.5 || shape_used_ <= 0.0)
				shape_used_ = shape;
			std::vector<Box> boxes;
			for (const auto &entry : seen_)
				if (entry.second.there)
					boxes.push_back(entry.second.box);
			const Box goal = free_space(within, s.anchor_x, s.anchor_y, boxes, s.margin, shape_used_);
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
		Box box;
		bool there = false, kept = false;
	};
	std::map<int64_t, Seen> seen_;
	std::atomic<bool> stirred_{false};
	std::atomic<int64_t> own_id_{-1};
	bool look_again_ = true, waiting_ = false, changed_ = false;
	double quiet_for_ = 0.0, since_look_ = 0.0;
	size_t turn_ = 0;
	double shape_seen_ = 0.0, shape_used_ = 0.0, shape_for_ = 0.0;
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
	for (const char *name : {"skip|alist", "glide_ms", "ease_curve", "ease_mode"})
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
	obs_property_t *as_arranged = obs_properties_add_bool(props, "as_arranged", obs_module_text("Fit.AsArranged"));
	obs_property_set_long_description(as_arranged, obs_module_text("Fit.AsArranged.Tip"));
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
	obs_data_set_default_bool(settings, "as_arranged", true);
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
	obs_register_source(&info);
}

} // namespace stagekit
