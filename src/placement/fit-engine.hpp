/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "../common/easing.hpp"
#include "../common/scene-util.hpp"
#include "../dock/arrange.hpp"

#include <obs.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit::fit {

// Saved in settings: append only.
enum class BoxKind { WholeScene = 0, Custom = 1, FreeSpace = 2 };

struct FitSettings {
	std::string item;
	BoxKind box = BoxKind::WholeScene;
	double x = 0, y = 0, w = 1920, h = 1080;
	double margin = 80;
	double tolerance = 8;           // free space: pictures this close in size count as equal
	bool resize = true;             // off: the item keeps its size and is only moved, centred in the space
	int toward_x = 0, toward_y = 0; // the side of its space it is kept towards: -1, 0 (the middle) or 1
	// Its place here is given to its copies in these scenes while they are
	// not on screen. (A tick box for every scene came first, in beta105; the
	// owner has the same source in scenes where it belongs elsewhere.)
	std::vector<std::string> share_scenes;
	std::string orig_item;         // the item whose size was noted before the filter first touched it
	double orig_k = 0;             // how large it was drawn then, as a scale of its source
	bool visible_pixels = true;    // other items are measured by what they draw, not their boxes
	bool no_shadows = true;        // and shadows, glows and outlines do not count, its own included
	std::vector<std::string> skip; // items that are never in the way
	// ... unless a rule says the picture leaves too much of its space empty:
	bool skip_rule = false; // the switch for both rules
	bool skip_pct = false;  // by the share of its space it would fill
	double skip_fill = 0.0; // (that share)
	bool skip_px = false;   // by the empty room it would leave at each edge of its space
	double px_top = 0.0, px_bottom = 0.0, px_left = 0.0, px_right = 0.0;
	bool have_anchor = false; // the free space is the one round this point
	double anchor_x = 0, anchor_y = 0;
	double glide_s = 0.3;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

// What a fit will show of its item once it has got there, for the other
// fits of the scene to go by.
struct Claim {
	Box box;
	bool resizes = true;
};
void claim_set(obs_source_t *scene, int64_t item, const void *owner, const Claim &claim);
void claim_clear(const void *owner);
bool claim_of(obs_source_t *scene, int64_t item, const void *asking, Claim &out);

struct Glance {
	bool any = false;                   // something is drawn
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0; // and where, in cells of the small copy
};
bool comes_and_goes(obs_source_t *source);
bool glance_differs(const Glance &a, const Glance &b);
bool glance_of(obs_source_t *scene, int64_t item, Glance &out);
void glance_step(const void *asking, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items);
void glance_forget(obs_source_t *scene);
void glance_free();

Box free_space(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape,
	       double tolerance);

// One fitted item: the whole of a Fit to Space filter but its window.
class FitEngine {
public:
	FitEngine(obs_data_t *settings, obs_source_t *source);
	~FitEngine();
	void placed(obs_source_t *scene, const char *item_name);
	// Whether this filter, looking after the item of that name, lists the
	// scene of that name to share its place with.
	bool shares_with(const std::string &item_name, const char *scene_name)
	{
		std::lock_guard lock(mutex_);
		return settings_.item == item_name &&
		       std::find(settings_.share_scenes.begin(), settings_.share_scenes.end(), scene_name) !=
			       settings_.share_scenes.end();
	}
	const char *scene_name() const { return parent_ ? obs_source_get_name(parent_) : nullptr; }
	void shared(obs_source_t *scene, const char *item_name, const double *home);
	void update(obs_data_t *settings);
	void attach(obs_source_t *parent);
	void detach();
	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }
	bool holds_room(obs_sceneitem_t *item, const FitSettings &s);
	void tick(float seconds);

private:
	bool free_box(obs_sceneitem_t *item, FitSettings &s, const Measure &how, const Box &within, double shape,
		      double kept_w, double kept_h, float seconds, Box &space);

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	FitSettings settings_;
	// The free space: used on the tick only, but for the two flags.
	struct Seen {
		Box box;     // what shows in it, on the canvas, when it was looked at
		Box item_at; // and where its box was then
		bool there = false;
	};
	std::vector<Box> boxes_;  // what is in the way now
	std::vector<bool> gives_; // and which of them another filter resizes to make room
	std::vector<int64_t> ids_;
	std::vector<bool> said_;
	std::vector<bool> passed_; // which are items never in the way, counted only when the picture leaves room
	bool avoiding_ = false;    // they count just now
	std::array<double, 8> used_rule_{-1.0};
	static constexpr double kArriving = 0.25;
	bool was_showing_ = false;
	double shown_for_ = 0.0;
	std::map<int64_t, Seen> seen_;
	bool look_again_ = true, changed_ = false, unsettled_ = false;
	double calm_for_ = 0.0;
	double since_look_ = 0.0;
	size_t turn_ = 0;
	std::map<int64_t, Glance> looked_; // what a glance said of each item when it was last looked at
	double shape_seen_ = 0.0, shape_used_ = 0.0, shape_for_ = 0.0;
	double used_margin_ = -1.0, used_tolerance_ = -1.0;
	double set_bw_ = 0.0, set_bh_ = 0.0; // the bounding box last given to the item
	double keep_k_ = 0.0;                // "Resize it to fit" off: how large the picture is kept, as a scale
	// What shows of the fitted item itself: the part of its picture cut off
	// on the left, top, right and bottom, when it was last looked at.
	double own_cut_[4] = {0.0, 0.0, 0.0, 0.0};
	bool own_seen_ = false, was_placed_ = false;
	double own_sw_ = 0.0, own_sh_ = 0.0, own_for_ = 0.0;
	bool have_keep_ = false;
	// The kept size as it is saved in the settings, and whether the settings
	// have been read yet (the first time is OBS loading the filter).
	bool first_update_ = true;
	std::string saved_keep_item_;
	double saved_keep_k_ = 0.0;
	bool back_to_original_ = false, resized_by_hand_ = false;
	bool placed_by_tool_ = false;
	bool placed_ = false; // under mutex_: the item was just placed on purpose by a tool
	// Under mutex_: the item was just given its place by another scene's filter.
	bool shared_in_ = false, shared_home_ = false;
	double shared_home_x_ = 0.0, shared_home_y_ = 0.0;
	double steady_sw_ = 0.0, steady_sh_ = 0.0,
	       steady_for_ =
		       0.0; // the item's source has kept its size this long // just unticked: keep the size from before the filter
	Box goal_, from_, cur_;
	bool have_goal_ = false, have_cur_ = false;
	double glide_ = 1.0;
	// A glide that goes with a window switcher's change of window: its time
	// and easing, and which change was last answered.
	bool synced_ = false;
	double sync_s_ = 0.0;
	EaseCurve sync_curve_ = EaseCurve::Cubic;
	EaseMode sync_mode_ = EaseMode::InOut;
	uint64_t switch_id_ = 0;
	bool have_set_ = false, moving_ = false;
	// "Same place in other scenes": the place last given to the other scenes.
	bool have_shared_ = false;
	std::atomic<bool> share_again_{false};
	double shared_[4] = {0.0, 0.0, 0.0, 0.0};
	uint32_t shared_toward_ = 0;
	// A hidden item is left alone; shown again, it first says where it is.
	static constexpr double kComingBack = 0.1;
	bool hidden_ = false, coming_back_ = false, have_claim_ = false;
	bool box_asked_ = false; // its bounding box was asked for, for want of a room it was given
	bool new_item_ = false;  // the filter was given another item (set under mutex_)
	double back_for_ = 0.0;
	Claim last_claim_;
	double set_x_ = 0.0, set_y_ = 0.0, seen_x_ = 0.0, seen_y_ = 0.0, still_for_ = 0.0;
};

// Every engine there is: told of an item placed by a tool, or given its place
// by another scene's fit; and the filters that own them (each held).
void engines_placed(obs_source_t *scene, const char *item_name);
void engines_shared(obs_source_t *scene, const char *item_name, const double *home);
std::vector<obs_source_t *> engine_owners();

} // namespace stagekit::fit
