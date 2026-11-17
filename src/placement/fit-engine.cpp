/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The engine behind Fit to Space (and Scene Fit): one item fitted in a box
// of its scene, the free space round it worked out from what is in the way,
// the claims two fits go by, and the glances that notice a widget change.

#include "fit-engine.hpp"
#include "../common/stand-in.hpp"

#include "../common/timing.hpp"

#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../dock/arrange-tools.hpp"
#include "../dock/arrange.hpp"
#include "../shadow/shadow-backdrop.hpp"
#include "../capture/app-capture.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit::fit {

// Where each Fit to Space filter (in "the free space around it") is taking
// its item: what will show of it, on the canvas, once it has got there, and
// whether the filter resizes it. Two filters in one scene go by each other's
// claims, not by where the other's item happens to be on its way: without
// that they took turns, each gliding a little after the other had, and the
// owner's game and chat box shifted "a number of times" before they settled.
std::mutex g_claims_mutex;
std::map<std::pair<obs_source_t *, int64_t>, std::pair<const void *, Claim>> g_claims;

void claim_set(obs_source_t *scene, int64_t item, const void *owner, const Claim &claim)
{
	std::lock_guard lock(g_claims_mutex);
	for (auto it = g_claims.begin(); it != g_claims.end();)
		it = it->second.first == owner && it->first != std::make_pair(scene, item) ? g_claims.erase(it)
											   : std::next(it);
	g_claims[{scene, item}] = {owner, claim};
}

void claim_clear(const void *owner)
{
	std::lock_guard lock(g_claims_mutex);
	for (auto it = g_claims.begin(); it != g_claims.end();)
		it = it->second.first == owner ? g_claims.erase(it) : std::next(it);
}

// Another filter's claim on an item, if there is one. Asked only about items
// that show: a hidden item's claim is kept on record for when it is shown.
bool claim_of(obs_source_t *scene, int64_t item, const void *asking, Claim &out)
{
	std::lock_guard lock(g_claims_mutex);
	const auto found = g_claims.find({scene, item});
	if (found == g_claims.end() || found->second.first == asking)
		return false;
	out = found->second.second;
	return true;
}

// ---------------------------------------------------------------- glances
// A cheap watch on what the items of a scene draw. Looking at an item
// properly (drawing it, reading it back at once and finding its edges) stalls
// the picture for a few milliseconds each time, and it used to be done once a
// second by every filter, whether anything had changed or not: the owner's log
// showed single ticks of 8 to 45 ms, and a widget that began to draw could go
// unnoticed for as many seconds as the scene has items. A glance instead
// draws one item very small, hands the copy to the graphics card and reads it
// two frames later, when that costs nothing. It only says roughly what part of
// the item is drawn; a filter looks properly only when that has changed.
constexpr uint32_t kGlanceSize = 128;
// A glance draws its item once more, filters and all, so it is not free on
// the graphics card: at fifteen a second on every item the owner saw the
// frame time go up (beta107). Only an item that can come and go is glanced at
// often: a browser widget, text, a group or a nested scene, and anything that
// has once been seen to change. The rest (a game capture, a camera, a
// background) are glanced at seldom, one of them at a time.
constexpr uint64_t kGlanceEvery = 4;                  // frames from one glance to the next, at the least
constexpr uint64_t kLivelyEvery = 30;                 // an item that comes and goes: every half second
constexpr uint64_t kOthersEvery = 600;                // any other item: every ten seconds
constexpr uint64_t kOthersApart = 120;                // and no two of those within two seconds
using GlanceKey = std::pair<obs_source_t *, int64_t>; // scene, item
std::mutex g_glance_mutex;
struct GlanceState {
	std::map<GlanceKey, Glance> seen;
	std::map<GlanceKey, uint64_t> when;   // the frame each was last glanced at
	std::map<GlanceKey, uint64_t> lively; // and the frame it last changed in, if it ever has
	uint64_t last_other = 0;              // the frame an item that never changes was last glanced at
	gs_texrender_t *render = nullptr;
	gs_stagesurf_t *stage = nullptr;
	bool out = false; // a copy is with the graphics card
	GlanceKey of{nullptr, 0};
	uint64_t begun = 0, frame = 0, frame_time = 0;
	const void *last = nullptr; // whose list the last glance came from
	bool others_asked = false;  // and whether another has asked since
} g_glance;

// Whether a source is of a kind that shows something at one time and nothing
// at another without changing size.
bool comes_and_goes(obs_source_t *source)
{
	if (obs_scene_from_source(source) || obs_group_from_source(source))
		return true;
	const char *id = obs_source_get_unversioned_id(source);
	for (const char *kind : {"browser_source", "text_gdiplus", "text_ft2_source", "text_pango_source"})
		if (id && std::strcmp(id, kind) == 0)
			return true;
	return false;
}

bool glance_differs(const Glance &a, const Glance &b)
{
	if (a.any != b.any)
		return true;
	// By two cells or more: an edge that lies on the line between two cells
	// flickers by one.
	return a.any && (std::abs(a.x0 - b.x0) >= 2 || std::abs(a.y0 - b.y0) >= 2 || std::abs(a.x1 - b.x1) >= 2 ||
			 std::abs(a.y1 - b.y1) >= 2);
}

// What was last seen of an item at a glance; false while it has had none.
bool glance_of(obs_source_t *scene, int64_t item, Glance &out)
{
	std::lock_guard lock(g_glance_mutex);
	const auto found = g_glance.seen.find({scene, item});
	if (found == g_glance.seen.end())
		return false;
	out = found->second;
	return true;
}

// Called every tick by each filter that is at rest in a scene on screen, with
// the items it wants watched. Reads the glance that is out, or begins one on
// whichever item of the list is due and has waited longest.
void glance_step(const void *asking, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items)
{
	if (measuring_stopped() || items.empty())
		return;
	std::lock_guard lock(g_glance_mutex);
	GlanceState &g = g_glance;
	const uint64_t now = obs_get_video_frame_time();
	if (now != g.frame_time) {
		g.frame_time = now;
		g.frame++;
	}
	if (g.out) {
		if (g.frame - g.begun < 2)
			return;
		g.out = false;
		Glance seen;
		obs_enter_graphics();
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		const bool read = gs_stagesurface_map(g.stage, &data, &linesize);
		if (read) {
			int x0 = (int)kGlanceSize, y0 = (int)kGlanceSize, x1 = -1, y1 = -1;
			for (uint32_t y = 0; y < kGlanceSize; y++) {
				const uint8_t *row = data + (size_t)y * linesize;
				for (uint32_t x = 0; x < kGlanceSize; x++) {
					if (row[x * 4 + 3] <= 12)
						continue;
					x0 = std::min(x0, (int)x);
					x1 = std::max(x1, (int)x);
					y0 = std::min(y0, (int)y);
					y1 = std::max(y1, (int)y);
				}
			}
			gs_stagesurface_unmap(g.stage);
			if (x1 >= x0 && y1 >= y0)
				seen = Glance{true, x0, y0, x1, y1};
		}
		obs_leave_graphics();
		if (read) {
			const auto was = g.seen.find(g.of);
			if (was != g.seen.end() && glance_differs(seen, was->second))
				g.lively[g.of] = g.frame;
			g.seen[g.of] = seen;
		}
		return;
	}
	if (asking != g.last)
		g.others_asked = true;
	if (g.frame - g.begun < kGlanceEvery)
		return;
	// Each filter in turn: the one whose list the last glance came from
	// stands back while another is asking.
	if (asking == g.last && g.others_asked && g.frame - g.begun < 3 * kGlanceEvery)
		return;
	// Which: the longest-waiting of the items that come and go and are due;
	// failing one, the longest-waiting of the others that is due, if none of
	// those has been glanced at lately.
	obs_sceneitem_t *pick = nullptr;
	uint64_t pick_when = 0;
	bool pick_lively = false;
	for (int pass = 0; pass < 2 && !pick; pass++) {
		if (pass == 1 && g.last_other && g.frame - g.last_other < kOthersApart)
			break;
		for (obs_sceneitem_t *item : items) {
			obs_source_t *shows = obs_sceneitem_get_source(item);
			if (!shows)
				continue;
			const GlanceKey key{scene, obs_sceneitem_get_id(item)};
			const bool lively = g.lively.count(key) || comes_and_goes(shows);
			if (lively != (pass == 0))
				continue;
			const auto at = g.when.find(key);
			const uint64_t when = at == g.when.end() ? 0 : at->second;
			if (when && g.frame - when < (lively ? kLivelyEvery : kOthersEvery))
				continue; // not due yet
			if (!pick || when < pick_when) {
				pick = item;
				pick_when = when;
				pick_lively = lively;
			}
		}
	}
	if (pick && !pick_lively)
		g.last_other = g.frame;
	obs_source_t *source = pick ? obs_sceneitem_get_source(pick) : nullptr;
	if (!source)
		return;
	const GlanceKey key{scene, obs_sceneitem_get_id(pick)};
	g.when[key] = g.frame;
	g.begun = g.frame;
	g.last = asking;
	g.others_asked = false;
	const uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
	if (!sw || !sh) {
		g.seen[key] = Glance{};
		return;
	}
	obs_enter_graphics();
	if (!g.render)
		g.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!g.stage)
		g.stage = gs_stagesurface_create(kGlanceSize, kGlanceSize, GS_RGBA);
	gs_texrender_reset(g.render);
	if (g.render && g.stage && gs_texrender_begin(g.render, kGlanceSize, kGlanceSize)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(g.render);
		gs_stage_texture(g.stage, gs_texrender_get_texture(g.render));
		g.out = true;
		g.of = key;
	}
	obs_leave_graphics();
}

// What was seen of a scene's items is let go when a filter joins or leaves
// the scene. A scene that is deleted leaves its entries behind, and OBS may
// make the next scene in the very same place, with items numbered from one
// again: the new scene's items would be taken for the old one's, and a widget
// in it go unnoticed.
void glance_forget(obs_source_t *scene)
{
	if (!scene)
		return;
	std::lock_guard lock(g_glance_mutex);
	const auto drop = [scene](auto &table) {
		for (auto it = table.begin(); it != table.end();)
			it = it->first.first == scene ? table.erase(it) : std::next(it);
	};
	drop(g_glance.seen);
	drop(g_glance.when);
	drop(g_glance.lively);
}

// When the last filter has gone: nothing is kept, on the graphics card or off it.
void glance_free()
{
	std::lock_guard lock(g_glance_mutex);
	obs_enter_graphics();
	gs_texrender_destroy(g_glance.render);
	gs_stagesurface_destroy(g_glance.stage);
	obs_leave_graphics();
	g_glance = GlanceState{};
}

namespace {
std::mutex g_filters_mutex;
std::vector<FitEngine *> g_filters; // every engine there is, for fit_item_placed
// Adds the scenes whose own filter for `item` lists `asking`'s scene to share with.
void share_partners(FitEngine *asking, const std::string &item, std::vector<std::string> &with);
} // namespace

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

namespace {
// Off screen, with nothing to do, an engine's tick is skipped but four times a
// second (at 60 frames a second), each engine on a frame of its own.
constexpr uint32_t kIdleEvery = 15;
std::atomic<uint64_t> g_full_ticks{0};
} // namespace

uint64_t engine_full_ticks()
{
	return g_full_ticks.load();
}

std::string fit_list_prefix(const std::string &entry)
{
	static const char *const kHex = "0123456789abcdef";
	std::string out = "fit_";
	for (unsigned char c : entry) {
		out += kHex[c >> 4];
		out += kHex[c & 15];
	}
	return out + "_";
}

FitEngine::FitEngine(obs_data_t *settings, obs_source_t *owner, std::string prefix, std::string entry)
	: source_(owner),
	  prefix_(std::move(prefix)),
	  entry_(std::move(entry)),
	  list_prefix_(entry_.empty() ? std::string() : fit_list_prefix(entry_))
{
	idle_phase_ = (double)((reinterpret_cast<uintptr_t>(this) >> 4) % kIdleEvery);
	update(settings);
	std::lock_guard lock(g_filters_mutex);
	g_filters.push_back(this);
}

FitEngine::~FitEngine()
{
	claim_clear(this);
	bool last;
	{
		std::lock_guard lock(g_filters_mutex);
		g_filters.erase(std::remove(g_filters.begin(), g_filters.end(), this), g_filters.end());
		last = g_filters.empty();
	}
	if (last)
		glance_free();
}

void FitEngine::store_bool(const char *key, bool v)
{
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_data_set_bool(settings, (prefix_ + key).c_str(), v);
	obs_data_release(settings);
}

void FitEngine::store_double(const char *key, double v)
{
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_data_set_double(settings, (prefix_ + key).c_str(), v);
	obs_data_release(settings);
}

void FitEngine::store_string(const char *key, const char *v)
{
	obs_data_t *settings = obs_source_get_settings(source_);
	obs_data_set_string(settings, (prefix_ + key).c_str(), v);
	obs_data_release(settings);
}

// See fit_item_placed.
void FitEngine::placed(obs_source_t *scene, const char *item_name)
{
	std::lock_guard lock(mutex_);
	if (settings_.item == item_name && (!scene || scene == parent_ || obs_source_is_group(scene)))
		placed_ = true;
}

// See fit_item_shared.
void FitEngine::shared(obs_source_t *scene, const char *item_name, const double *home)
{
	std::lock_guard lock(mutex_);
	if (settings_.item != item_name || scene != parent_)
		return;
	shared_in_ = true;
	shared_home_ = home != nullptr;
	if (home) {
		shared_home_x_ = home[0];
		shared_home_y_ = home[1];
	}
}

void FitEngine::update(obs_data_t *settings)
{
	auto k = [this](const char *name) {
		return prefix_ + name;
	};
	auto num = [&](const char *name) {
		return obs_data_get_double(settings, k(name).c_str());
	};
	FitSettings s;
	s.item = entry_.empty() ? obs_data_get_string(settings, k("item").c_str()) : entry_;
	s.box = obs_data_get_int(settings, k("box").c_str()) == 1 ? BoxKind::Custom : BoxKind::WholeScene;
	s.x = num("box_x");
	s.y = num("box_y");
	s.w = std::max(2.0, num("box_width"));
	s.h = std::max(2.0, num("box_height"));
	s.margin = std::clamp(num("margin"), 0.0, 2000.0);
	s.tolerance = std::clamp(num("tolerance"), 0.0, 200.0);
	// (A switch of its own since beta122; a share set before that
	// counts as switched on, and is written down as such.) For an entry
	// of a Scene Fit the switch is the Scene Fit's own, for all entries.
	if (entry_.empty() && !obs_data_has_user_value(settings, "skip_fill_on") &&
	    obs_data_has_user_value(settings, "skip_fill") && obs_data_get_double(settings, "skip_fill") > 0.0)
		obs_data_set_bool(settings, "skip_fill_on", true);
	s.skip_rule = obs_data_get_bool(settings, entry_.empty() ? k("skip_fill_on").c_str() : "skip_fill_on");
	s.skip_pct = obs_data_get_bool(settings, k("skip_use_pct").c_str());
	s.skip_fill = std::clamp(num("skip_fill") / 100.0, 0.0, 1.0);
	s.skip_px = obs_data_get_bool(settings, k("skip_use_px").c_str());
	s.px_top = std::max(0.0, num("skip_px_top"));
	s.px_bottom = std::max(0.0, num("skip_px_bottom"));
	s.px_left = std::max(0.0, num("skip_px_left"));
	s.px_right = std::max(0.0, num("skip_px_right"));
	s.resize = obs_data_get_bool(settings, k("resize").c_str());
	// 0 the middle, then top, bottom, left, right, and the four corners.
	static const int kTowardX[] = {0, 0, 0, -1, 1, -1, 1, -1, 1};
	static const int kTowardY[] = {0, -1, 1, 0, 0, -1, -1, 1, 1};
	const long long toward = std::clamp<long long>(obs_data_get_int(settings, k("toward").c_str()), 0, 8);
	s.toward_x = kTowardX[toward];
	s.toward_y = kTowardY[toward];
	s.share_scenes = pick_list_read(settings, (list_prefix_ + "share_scenes").c_str(), false);
	s.orig_item = obs_data_get_string(settings, k("orig_item").c_str());
	s.orig_k = num("orig_k");
	own_seen_ = false; // what shows of the item is looked at again
	{
		std::lock_guard lock(mutex_);
		dirty_ = true;
		// The size kept is only read afresh when the filter is given
		// another item or told to stop resizing: any other change of
		// settings (and OBS sends them for many reasons) leaves it.
		if (s.item != settings_.item || s.resize != settings_.resize)
			have_keep_ = false;
		// Another item: the room the last one held is not its room.
		if (s.item != settings_.item)
			new_item_ = true;
		// Another list of scenes to share with: they get the place now.
		if (s.share_scenes != settings_.share_scenes)
			share_again_ = true;
		// Unticked just now: the size to keep is the one the item had
		// before this filter resized it, not the fitted one.
		if (!s.resize && settings_.resize)
			back_to_original_ = true;
		// When OBS starts, the size kept is the one saved, not one
		// read off the item: just after loading, a group or a source
		// with a shadow is not yet the size it will be, and a size
		// worked out from it then came out a few pixels large. The
		// owner: "if i restart, the chat box is slightly larger".
		if (first_update_) {
			first_update_ = false;
			back_to_original_ = false;
			saved_keep_item_ = obs_data_get_string(settings, k("keep_item").c_str());
			saved_keep_k_ = num("keep_k");
			if (!s.resize && !s.item.empty() && saved_keep_item_ == s.item && saved_keep_k_ > 0.0) {
				keep_k_ = saved_keep_k_;
				have_keep_ = true;
			}
		}
	}
	const long long kind = obs_data_get_int(settings, k("box").c_str());
	if (kind == 2)
		s.box = BoxKind::FreeSpace;
	// The filter's own two measuring options. Before they existed there
	// was one tick box that followed the dock; unticked, it meant boxes.
	s.visible_pixels = obs_data_get_bool(settings, k("visible_pixels").c_str());
	s.no_shadows = obs_data_get_bool(settings, k("no_shadows").c_str());
	if (prefix_.empty() && obs_data_has_user_value(settings, "as_arranged") &&
	    !obs_data_get_bool(settings, "as_arranged") && !obs_data_has_user_value(settings, "visible_pixels"))
		s.visible_pixels = false;
	s.skip = pick_list_read(settings, (list_prefix_ + "skip").c_str(), false);
	s.have_anchor = obs_data_get_bool(settings, k("have_anchor").c_str());
	s.anchor_x = num("anchor_x");
	s.anchor_y = num("anchor_y");
	s.glide_s = std::clamp((double)obs_data_get_int(settings, k("glide_ms").c_str()), 0.0, 10000.0) / 1000.0;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, k("ease_curve").c_str()));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, k("ease_mode").c_str()));
	look_again_ = true;
	std::lock_guard lock(mutex_);
	settings_ = s;
}

void FitEngine::attach(obs_source_t *parent, bool forget)
{
	parent_ = parent;
	if (forget)
		glance_forget(parent);
	own_looked_have_ = false;
}

void FitEngine::detach(bool forget)
{
	if (forget)
		glance_forget(parent_);
	parent_ = nullptr;
}

// The room an item holds in the free space while this filter is not
// fitting it (it is hidden, or shows but has no picture yet): the room it
// was last given or, for one never fitted here, its bounding box as it
// was saved.
bool FitEngine::holds_room(obs_sceneitem_t *item, const FitSettings &s)
{
	if (s.box != BoxKind::FreeSpace)
		return false;
	if (!have_claim_ && !box_asked_ && obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		box_asked_ = true;
		refresh_item(item);
		const Box at = scene_box(parent_, item);
		if (at.w >= 2.0 && at.h >= 2.0) {
			last_claim_ = Claim{at, s.resize};
			have_claim_ = true;
		}
	}
	return have_claim_;
}

// Its scene not on screen, and nothing asked of it: nobody sees the item,
// so the tick is skipped, all but four times a second, when it is done in
// full with the time skipped added (a hidden item's room stays on record,
// a kept size is still read). The tick the scene comes into view is a full
// one, and puts the item right without a glide. Anything asked of it (a
// tool's placing, another scene's sharing, new settings) is done at once.
bool FitEngine::idle(float seconds)
{
	if (!parent_ || obs_source_showing(parent_))
		return false;
	{
		std::lock_guard lock(mutex_);
		if (dirty_ || placed_ || shared_in_ || new_item_)
			return false;
	}
	idle_s_ += seconds;
	idle_phase_ += 1.0;
	if (idle_phase_ >= (double)kIdleEvery) {
		idle_phase_ = 0.0;
		return false;
	}
	return true;
}

bool FitEngine::prepare(obs_sceneitem_t *item, float tick_seconds, Prep &p)
{
	g_full_ticks++;
	full_ticks_++;
	// (With the time of the ticks skipped while off screen.)
	const float seconds = tick_seconds + (float)std::exchange(idle_s_, 0.0);
	FitSettings &s = p.s;
	bool placed_now = false, shared_now = false, new_home = false;
	{
		std::lock_guard lock(mutex_);
		dirty_ = false;
		// Given its place by another scene's filter: it stays there
		// until this scene comes into view, and the size it was given
		// is the one kept. The point its space is looked for around
		// is the other filter's, not where the item happens to be.
		if (shared_in_) {
			shared_in_ = false;
			shared_now = true;
			if (shared_home_) {
				new_home = true;
				settings_.have_anchor = true;
				settings_.anchor_x = shared_home_x_;
				settings_.anchor_y = shared_home_y_;
			}
		}
		// Placed on purpose by a tool: its space is looked for where
		// it is now, starting afresh.
		if (placed_) {
			placed_ = false;
			placed_now = true;
			settings_.have_anchor = false;
		}
		if (new_item_) {
			new_item_ = false;
			have_claim_ = box_asked_ = false;
		}
		s = settings_;
	}
	if (placed_now || shared_now) {
		// The room it held is not where it is now.
		have_claim_ = box_asked_ = false;
		have_keep_ = false;
		have_cur_ = have_goal_ = have_set_ = false;
		moving_ = false;
		own_seen_ = false;
		placed_by_tool_ = true;
	}
	if (new_home) {
		store_bool("have_anchor", true);
		store_double("anchor_x", s.anchor_x);
		store_double("anchor_y", s.anchor_y);
	}
	p.item = item;
	// A hidden item is left exactly as it is and holds no space: the
	// others take its room, and it is found where it was when it is
	// shown again. (Fitted while hidden, the owner's camera was put in
	// the middle of the room the chat box had taken meanwhile, came
	// back on top of it, and neither moved again.)
	// The room it will say is its own when it is shown is kept on
	// record meanwhile. Nothing goes by it while the item is hidden
	// (only items that show are asked about), and it is there from the
	// moment the item is shown, not from this filter's next tick: until
	// then a filter ticked before this one took the item for something
	// firm, and with the captures of a layout scene changed by a key
	// the camera and the chat box beside them jumped 27 px for a frame.
	if (item && !obs_sceneitem_visible(item)) {
		if (!hidden_)
			box_asked_ = false;
		if (holds_room(item, s))
			claim_set(parent_, obs_sceneitem_get_id(item), this, last_claim_);
		else
			claim_clear(this);
		hidden_ = true;
		return false;
	}
	// Shown again: for a moment it only says where it is, so that
	// whatever took its room goes by that and makes way first. Looking
	// for its space at once, it would find its place inside the other
	// and take that for something it sits on.
	if (hidden_) {
		hidden_ = false;
		box_asked_ = false;
		back_for_ = 0.0;
		coming_back_ = have_claim_;
	}
	if (coming_back_) {
		if ((back_for_ += seconds) < kComingBack && item) {
			claim_set(parent_, obs_sceneitem_get_id(item), this, last_claim_);
			return false;
		}
		coming_back_ = false;
	}
	obs_source_t *shown = item ? obs_sceneitem_get_source(item) : nullptr;
	const double sw = shown ? obs_source_get_width(shown) : 0.0, sh = shown ? obs_source_get_height(shown) : 0.0;
	if (sw < 2.0 || sh < 2.0) {
		// Nothing to fit yet. An item that is showing but has no
		// picture so far (a display capture before its first frame)
		// still holds its room in the free space. Holding none, it was
		// not there for the other filters, and with five captures
		// stacked in one place and a display switched to, the camera
		// beside it glided to the middle of the screen and stayed
		// there, and the display was fitted smaller in what was left.
		if (item && holds_room(item, s))
			claim_set(parent_, obs_sceneitem_get_id(item), this, last_claim_);
		return false;
	}

	// What is fitted is the picture, without the room a shadow takes
	// around it and without what the item crops away. (The room is read
	// again only when a shadow's room has changed, or another source is
	// shown.)
	const uint64_t rev = shadow_room_revision();
	if (shown != room_src_ || rev != room_rev_) {
		room_src_ = shown;
		room_rev_ = rev;
		shadow_room(shown, room_c_);
		in_view_c_ = shadow_kept_in_view(shown);
	}
	double room[4] = {room_c_[0], room_c_[1], room_c_[2], room_c_[3]};
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	// With shadows counted as part of an item (its own tick box), the
	// shadow is fitted too: it keeps the margin, not the picture.
	const Measure how{s.visible_pixels, s.no_shadows};
	// So is one asked to stay on screen ("Keep it on screen" in its
	// shadow filter): a picture that fills the box makes room for it.
	const bool shadow_in_view = in_view_c_;
	const double bare_w = sw - std::max(room[0], (double)crop.left) - std::max(room[2], (double)crop.right);
	const double bare_h = sh - std::max(room[1], (double)crop.top) - std::max(room[3], (double)crop.bottom);
	if (!s.no_shadows || shadow_in_view)
		room[0] = room[1] = room[2] = room[3] = 0.0;
	double left = std::max(room[0], (double)crop.left), top = std::max(room[1], (double)crop.top);
	double right = std::max(room[2], (double)crop.right), bottom = std::max(room[3], (double)crop.bottom);
	double pw = sw - left - right, ph = sh - top - bottom;
	if (pw < 2.0 || ph < 2.0)
		return false;
	// "Use visible pixels" is about the fitted item too: what is fitted
	// and kept clear is the part of it that shows, not its box. A group
	// or a widget is often larger than what it draws; the owner: "only
	// the visible pixels in the group, or any other item should be
	// counted. otherwise, what's the point of that setting?" Looking is
	// costly, so it is done when the item is first met, when its source
	// changes size, and every few seconds while it sits where it was put.
	if (s.visible_pixels) {
		own_for_ += seconds;
		// Its source changed size (a filter was added, a window
		// resized): what was seen no longer holds, and the look waits
		// until the size has stayed put for half a second, so that it
		// is not taken while things are still settling.
		if (own_sw_ != sw || own_sh_ != sh) {
			own_sw_ = sw;
			own_sh_ = sh;
			own_cut_[0] = own_cut_[1] = own_cut_[2] = own_cut_[3] = 0.0;
			own_seen_ = false;
			own_for_ = 0.0;
		}
		// (While it sits where it was put, a glance says when it
		// draws another part of itself; outside the free space, where
		// nothing else is watched, the glances are asked for here.)
		const int64_t own_id = obs_sceneitem_get_id(item);
		bool own_changed = false;
		if (was_placed_ && own_seen_ && obs_source_showing(parent_)) {
			if (s.box != BoxKind::FreeSpace)
				glance_step(this, parent_, {item});
			Glance glance;
			if (glance_of(parent_, own_id, glance))
				own_changed = !own_looked_have_ || glance_differs(glance, own_looked_);
		}
		const bool due = (!own_seen_ && own_for_ >= 0.5) || (own_changed && own_for_ >= 0.5);
		if (due && obs_source_showing(parent_) && !measuring_stopped()) {
			own_for_ = 0.0;
			own_seen_ = true;
			own_looked_have_ = glance_of(parent_, own_id, own_looked_);
			// (Measured first: that also brings the item's box up to date.)
			Box shows;
			const bool any = measure_seen(parent_, item, how, shows);
			const Box whole = scene_box(parent_, item);
			const double cw = sw - crop.left - crop.right, ch = sh - crop.top - crop.bottom;
			// Only an item wholly on the screen is believed: what hangs
			// over the edge may not be measured at all.
			const double scene_w = obs_source_get_width(parent_), scene_h = obs_source_get_height(parent_);
			const bool on_screen = whole.x >= -0.5 && whole.y >= -0.5 &&
					       whole.x + whole.w <= scene_w + 0.5 && whole.y + whole.h <= scene_h + 0.5;
			if (!on_screen)
				own_seen_ = false; // looked at again once it is
			if (any && on_screen && whole.w >= 1.0 && whole.h >= 1.0 && cw >= 1.0 && ch >= 1.0) {
				// Where the picture itself is drawn: inside a bounding
				// box it is scaled to fit and centred; without one the
				// box is the picture.
				const bool boxed = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
				const double kk = boxed ? std::min(whole.w / cw, whole.h / ch) : whole.w / cw;
				const double ky = boxed ? kk : whole.h / ch;
				// (Against the side it is kept towards, when it is: taken
				// for centred, a squarer window kept towards the left was
				// seen as cut off on its right, and was fitted to a
				// space of another size than the one it had.)
				const uint32_t lies = boxed ? obs_sceneitem_get_bounds_alignment(item) : 0;
				const double fx = lies & OBS_ALIGN_LEFT ? 0.0 : lies & OBS_ALIGN_RIGHT ? 1.0 : 0.5;
				const double fy = lies & OBS_ALIGN_TOP ? 0.0 : lies & OBS_ALIGN_BOTTOM ? 1.0 : 0.5;
				const double dx = whole.x + (whole.w - cw * kk) * fx;
				const double dy = whole.y + (whole.h - ch * ky) * fy;
				const double px = dx + (left - crop.left) * kk, py = dy + (top - crop.top) * ky;
				const double pwn = pw * kk, phn = ph * ky;
				if (pwn >= 1.0 && phn >= 1.0) {
					// Under a fiftieth of the picture is the measuring (it is
					// done on a small copy), not empty room.
					auto cut = [](double part) {
						return part < 0.02 ? 0.0 : std::min(part, 0.9);
					};
					const double l = cut((shows.x - px) / pwn);
					const double t = cut((shows.y - py) / phn);
					const double r = cut((px + pwn - shows.x - shows.w) / pwn);
					const double b = cut((py + phn - shows.y - shows.h) / phn);
					// A change of a pixel or so is the measuring, not the item.
					const double least = 1.5 / std::max(pwn, phn);
					if (l + r < 0.95 && t + b < 0.95 &&
					    (std::fabs(l - own_cut_[0]) > least || std::fabs(t - own_cut_[1]) > least ||
					     std::fabs(r - own_cut_[2]) > least ||
					     std::fabs(b - own_cut_[3]) > least)) {
						own_cut_[0] = l;
						own_cut_[1] = t;
						own_cut_[2] = r;
						own_cut_[3] = b;
					}
				}
			}
		}
		left += own_cut_[0] * pw;
		right += own_cut_[2] * pw;
		top += own_cut_[1] * ph;
		bottom += own_cut_[3] * ph;
		pw = sw - left - right;
		ph = sh - top - bottom;
	}
	if (pw < 2.0 || ph < 2.0)
		return false;

	double bx = 0.0, by = 0.0, bw = obs_source_get_width(parent_), bh = obs_source_get_height(parent_);
	if (s.box == BoxKind::Custom) {
		bx = s.x;
		by = s.y;
		bw = s.w;
		bh = s.h;
	}
	if (bw < 2.0 || bh < 2.0)
		return false;
	// "Resize it to fit" unticked: the picture keeps the size it has in
	// the scene and is only moved, to the middle of its space. That size
	// is read from the item once, and again after it is resized by hand.
	// How large the picture is drawn in the scene right now.
	auto drawn_now = [&](double &k_now) {
		double kx = 1.0, ky = 1.0;
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 b;
			obs_sceneitem_get_bounds(item, &b);
			const double cw = sw - crop.left - crop.right, ch = sh - crop.top - crop.bottom;
			if (cw >= 1.0 && ch >= 1.0)
				kx = ky = std::min(b.x / cw, b.y / ch);
		} else {
			vec2 scale;
			obs_sceneitem_get_scale(item, &scale);
			kx = std::fabs(scale.x);
			ky = std::fabs(scale.y);
		}
		k_now = std::min(kx, ky);
		return k_now > 0.0;
	};
	// Before the filter does anything to an item, the size it has is
	// noted, so that "Resize it to fit" can be unticked later and the
	// item gets its own size back. (A new filter starts ticked: the
	// owner's chat box was fitted, and so shrunk, before the box could
	// be unticked, and then kept the shrunken size.)
	if (s.orig_item != s.item) {
		double k0 = 0.0;
		if (drawn_now(k0)) {
			s.orig_item = s.item;
			s.orig_k = k0;
			store_string("orig_item", s.orig_item.c_str());
			store_double("orig_k", k0);
			std::lock_guard lock(mutex_);
			settings_.orig_item = s.orig_item;
			settings_.orig_k = k0;
		}
	}
	if (!s.resize) {
		vec2 b_now;
		obs_sceneitem_get_bounds(item, &b_now);
		const bool boxed = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
		// Resized by hand in the preview: that is the size to keep
		// from now on. Only a person does that to an item that is
		// showing and whose source is not changing size. A shadow
		// filter also changes the bounding box, when its room does
		// (an item hidden or shown changes it): taken for a resize
		// by hand, that made the owner's camera a little smaller
		// each time it was hidden and shown again.
		if (sw != steady_sw_ || sh != steady_sh_ || !obs_sceneitem_visible(item)) {
			steady_sw_ = sw;
			steady_sh_ = sh;
			steady_for_ = 0.0;
		} else {
			steady_for_ += seconds;
		}
		// (And only to an item that is selected: the handles
		// that resize one are on the selected item.)
		if (have_keep_ && boxed && have_set_ && steady_for_ >= 1.0 && obs_sceneitem_selected(item) &&
		    (std::fabs(b_now.x - set_bw_) > 2.0 || std::fabs(b_now.y - set_bh_) > 2.0)) {
			have_keep_ = false;
			resized_by_hand_ = true;
		}
		if (!have_keep_ && back_to_original_ && s.orig_item == s.item && s.orig_k > 0.0) {
			keep_k_ = s.orig_k;
			have_keep_ = true;
		}
		// (Read only from an item whose source has kept its size for
		// half a second: one that is still loading says a wrong size.)
		if (!have_keep_ && !resized_by_hand_ && steady_for_ < 0.5)
			return false;
		if (!have_keep_) {
			const double was = keep_k_;
			have_keep_ = drawn_now(keep_k_);
			// For the log, so a size that changes by itself can be
			// traced: when the kept size was read, and why.
			if (have_keep_ && was > 0.0 && std::fabs(keep_k_ - was) > 0.001 * was)
				obs_log(LOG_INFO,
					"%s '%s': the size kept for '%s' went from %.4f to %.4f of its source (%s)",
					prefix_.empty() ? "Fit to Space" : "Scene Fit", obs_source_get_name(source_),
					s.item.c_str(), was, keep_k_,
					resized_by_hand_  ? "resized in the preview"
					: placed_by_tool_ ? "placed by a copy tool"
							  : "settings changed");
			resized_by_hand_ = false;
		}
		back_to_original_ = false;
		// Saved with the scene collection, for the next time OBS starts.
		if (have_keep_ && (saved_keep_item_ != s.item || std::fabs(saved_keep_k_ - keep_k_) > 1e-9 * keep_k_)) {
			saved_keep_item_ = s.item;
			saved_keep_k_ = keep_k_;
			store_string("keep_item", s.item.c_str());
			store_double("keep_k", keep_k_);
		}
	}
	p.sw = sw;
	p.sh = sh;
	p.crop = crop;
	p.how = how;
	p.bare_w = bare_w;
	p.bare_h = bare_h;
	p.left = left;
	p.top = top;
	p.right = right;
	p.bottom = bottom;
	p.pw = pw;
	p.ph = ph;
	p.bx = bx;
	p.by = by;
	p.bw = bw;
	p.bh = bh;
	// The size kept is the size of what shows of it.
	p.keep_w = pw * keep_k_;
	p.keep_h = ph * keep_k_;
	p.picture = pw / ph;
	p.free = s.box == BoxKind::FreeSpace;
	return true;
}

// The free space round the item's place: whether there is anything to do
// this tick (`Hold`: off screen, the place it has is kept).
FitEngine::Motion FitEngine::motion(Prep &p, float seconds)
{
	if (measuring_stopped())
		return Motion::Stop;
	FitSettings &s = p.s;
	obs_sceneitem_t *item = p.item;
	// Only for a scene that is being shown somewhere. Every scene's
	// filters run all the time, and looking at the items of scenes nobody
	// sees would cost the picture on screen its smoothness for nothing.
	if (!obs_source_showing(parent_)) {
		was_showing_ = false;
		return have_cur_ ? Motion::Hold : Motion::Stop;
	}
	// A scene that has only just come into view is put right at once
	// and without a glide: nobody has seen it as it was.
	if (!was_showing_) {
		was_showing_ = true;
		shown_for_ = 0.0;
	} else {
		shown_for_ += seconds;
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
			return Motion::Stop;
		moving_ = false;
		s.have_anchor = false; // taken afresh below
		have_cur_ = false;     // and no glide from where it was let go
	}
	if (!s.have_anchor) {
		const Box at = item_box(item);
		s.anchor_x = at.x + at.w / 2.0;
		s.anchor_y = at.y + at.h / 2.0;
		s.have_anchor = true;
		store_bool("have_anchor", true);
		store_double("anchor_x", s.anchor_x);
		store_double("anchor_y", s.anchor_y);
		std::lock_guard lock(mutex_);
		settings_.have_anchor = true;
		settings_.anchor_x = s.anchor_x;
		settings_.anchor_y = s.anchor_y;
		look_again_ = true;
	}
	return Motion::Solve;
}

// What is in the way, now against before.
void FitEngine::observe(const std::vector<Obstacle> &in_way, bool arriving, float seconds, bool &at_rest)
{
	at_rest = false;
	std::vector<Box> boxes;
	std::vector<bool> gives;
	std::vector<int64_t> ids; // which item each is, and whether its
	std::vector<bool> said;   // filter said where it is going
	std::vector<bool> passed; // one of the items never in the way
	for (const Obstacle &o : in_way) {
		boxes.push_back(o.box);
		gives.push_back(o.gives);
		ids.push_back(o.id);
		said.push_back(o.said);
		passed.push_back(o.passed);
	}
	auto differs = [](const Box &a, const Box &b) {
		return std::fabs(a.x - b.x) > 0.25 || std::fabs(a.y - b.y) > 0.25 || std::fabs(a.w - b.w) > 0.25 ||
		       std::fabs(a.h - b.h) > 0.25;
	};
	// What changed since the last look: `moved`, an item that is on its
	// way somewhere (gliding, being dragged, a window growing), or
	// `stepped`, a change made in one go: an item shown or hidden, or
	// another filter saying where its item is going.
	bool moved = false, stepped = ids != ids_ || said != said_ || passed != passed_;
	if (!stepped)
		for (size_t i = 0; i < boxes.size(); i++)
			if (differs(boxes[i], boxes_[i]))
				(said[i] ? stepped : moved) = true;
	if (moved && arriving) {
		moved = false;
		stepped = true;
	}
	// Which of them give way can change while nothing moves (another
	// filter has just said where its item is going).
	if (gives != gives_ && !moved && !stepped) {
		gives_ = gives;
		changed_ = true;
	}
	if (moved) {
		// Something is moving. The fitted item waits for it to come to
		// rest and then takes its new space in one glide: chasing a
		// moving item would resize the picture (and redo its shadow)
		// on every frame of the move. No looking meanwhile either.
		boxes_ = boxes;
		gives_ = gives;
		ids_ = ids;
		said_ = said;
		passed_ = passed;
		unsettled_ = true;
		calm_for_ = 0.0;
	} else if (stepped) {
		// Nothing to wait for: the new space is taken at once, in the
		// one glide. (Waiting here as for a moving item, two filters
		// answered each other a moment apart, and the owner's items
		// "adjust multiple times before settling".)
		boxes_ = boxes;
		gives_ = gives;
		ids_ = ids;
		said_ = said;
		passed_ = passed;
		if (!unsettled_)
			changed_ = true;
	} else if (unsettled_) {
		if ((calm_for_ += seconds) >= 0.15) {
			unsettled_ = false;
			changed_ = true;
		}
	} else if (glide_ >= 1.0) {
		// At rest: the items are glanced at, which costs next to
		// nothing, and one is looked at properly only when a glance
		// shows it drawing another part of itself than at the last
		// look (a widget that begins to show something, or stops).
		at_rest = true;
	}
}

bool FitEngine::solve(Prep &p, float seconds, bool first_pass, bool &moved)
{
	moved = false;
	FitSettings &s = p.s;
	const double shape = p.picture;
	const Box within{p.bx, p.by, p.bw, p.bh};
	const bool kept = !s.resize && have_keep_;
	const double kept_w = kept ? p.keep_w : 0.0, kept_h = kept ? p.keep_h : 0.0;
	// An Automatic Window Switcher on the fitted item is changing to
	// another window: the shape it is on its way to is taken now, and
	// the space for it is glided to over the switcher's own time and
	// easing, so the two move as one. (Left to notice the shape by
	// itself, the fit waited for it to stop changing and only then set
	// off: the owner saw the window switch "but then it's like the auto
	// fit has to catch up".)
	if (first_pass) {
		with_switch_ = false;
		AppCaptureChange change;
		if (app_capture_change(obs_sceneitem_get_source(p.item), change) && change.id != switch_id_ &&
		    change.now_w > 0.0 && change.now_h > 0.0 && change.to_w > 0.0 && change.to_h > 0.0) {
			switch_id_ = change.id;
			// (By how much the picture's shape will change: what comes
			// after the switcher, a shadow say, is carried along.)
			const double target = shape * (change.to_w / change.to_h) / (change.now_w / change.now_h);
			if (target > 0.0) {
				shape_used_ = target;
				shape_seen_ = shape;
				shape_for_ = 0.0;
				changed_ = true;
				with_switch_ = true;
				sync_s_ = change.total_s * std::clamp(1.0 - change.done, 0.0, 1.0);
				sync_curve_ = static_cast<EaseCurve>(change.curve);
				sync_mode_ = static_cast<EaseMode>(change.mode);
			}
		}
		// The shape counts once it has stopped changing.
		if (std::fabs(shape - shape_seen_) > 0.01 * shape) {
			shape_seen_ = shape;
			shape_for_ = 0.0;
		} else if (shape_for_ < 0.5 && (shape_for_ += seconds) >= 0.5 &&
			   std::fabs(shape - shape_used_) > 0.01 * shape) {
			changed_ = true;
		}
	}
	// The gap or the tolerance set anew: the space is worked out again.
	const std::array<double, 8> rule{s.skip_rule ? 1.0 : 0.0,
					 s.skip_pct ? 1.0 : 0.0,
					 s.skip_fill,
					 s.skip_px ? 1.0 : 0.0,
					 s.px_top,
					 s.px_bottom,
					 s.px_left,
					 s.px_right};
	if (s.margin != used_margin_ || s.tolerance != used_tolerance_ || rule != used_rule_) {
		used_rule_ = rule;
		used_margin_ = s.margin;
		used_tolerance_ = s.tolerance;
		changed_ = true;
	}
	if (changed_ || !have_goal_) {
		changed_ = false;
		if (!with_switch_ && (!have_goal_ || shape_for_ >= 0.5 || shape_used_ <= 0.0))
			shape_used_ = shape;
		// First without the items never in the way. Should the picture
		// then leave too much of its space empty (a narrow window on a
		// wide screen), they count after all: the picture goes beside
		// the camera rather than under it. Both spaces come from the
		// picture's shape alone, not from where it is now, so the
		// choice does not flip once the picture has moved.
		std::vector<Box> firm;
		for (size_t i = 0; i < boxes_.size(); i++)
			if (i >= passed_.size() || !passed_[i])
				firm.push_back(boxes_[i]);
		Box goal = free_space(within, s.anchor_x, s.anchor_y, firm, s.margin, shape_used_, s.tolerance);
		avoiding_ = false;
		if (s.skip_rule && firm.size() != boxes_.size() && goal.w >= 2.0 && goal.h >= 2.0) {
			double fw = kept_w, fh = kept_h;
			if (fw <= 0.0 || fh <= 0.0) {
				fw = std::min(goal.w, goal.h * shape_used_);
				fh = fw / shape_used_;
			}
			// Either rule that is ticked may say the picture fills its
			// space: by the share it takes up, or by the empty room at
			// each edge, the picture lying where it is kept towards.
			// With neither ticked nothing says so.
			const double fill = std::min(1.0, fw / goal.w) * std::min(1.0, fh / goal.h);
			const double spare_w = std::max(0.0, goal.w - fw), spare_h = std::max(0.0, goal.h - fh);
			const double left = spare_w * (s.toward_x + 1) / 2.0, top = spare_h * (s.toward_y + 1) / 2.0;
			const bool fills = (s.skip_pct && fill >= s.skip_fill) ||
					   (s.skip_px && left <= s.px_left + 0.05 &&
					    spare_w - left <= s.px_right + 0.05 && top <= s.px_top + 0.05 &&
					    spare_h - top <= s.px_bottom + 0.05);
			if (!fills) {
				const Box beside = free_space(within, s.anchor_x, s.anchor_y, boxes_, s.margin,
							      shape_used_, s.tolerance);
				if (beside.w >= 2.0 && beside.h >= 2.0) {
					goal = beside;
					avoiding_ = true;
				}
			}
		}
		if (goal.w < 2.0 || goal.h < 2.0)
			return false;
		if (!have_goal_ || std::fabs(goal.x - goal_.x) > 0.05 || std::fabs(goal.y - goal_.y) > 0.05 ||
		    std::fabs(goal.w - goal_.w) > 0.05 || std::fabs(goal.h - goal_.h) > 0.05) {
			from_ = have_cur_ ? cur_ : goal;
			goal_ = goal;
			// (With a window switcher's change: over its time, not this
			// filter's own.)
			synced_ = with_switch_;
			glide_ = have_cur_ && (synced_ ? sync_s_ : s.glide_s) > 0.0 && !arriving() ? 0.0 : 1.0;
			have_goal_ = true;
			moved = true;
		}
	}
	return true;
}

Box FitEngine::advance(const Prep &p, float seconds)
{
	const FitSettings &s = p.s;
	if (glide_ < 1.0) {
		const double over = synced_ ? sync_s_ : s.glide_s;
		glide_ = over > 0.0 ? std::min(1.0, glide_ + seconds / over) : 1.0;
		const double t = synced_ ? ease(sync_curve_, sync_mode_, glide_) : ease(s.curve, s.mode, glide_);
		cur_ = Box{from_.x + (goal_.x - from_.x) * t, from_.y + (goal_.y - from_.y) * t,
			   std::max(2.0, from_.w + (goal_.w - from_.w) * t),
			   std::max(2.0, from_.h + (goal_.h - from_.h) * t)};
	} else {
		cur_ = goal_;
	}
	have_cur_ = true;
	return cur_;
}

// Which sides of the free space end at something another filter resizes
// to make room (a game fitted beside it). Centring between such a thing and
// a wall never comes to rest: it makes room, the middle moves, it makes room
// again. So against one of those the item goes to the other side at once,
// and stays.
void FitEngine::soft_sides(bool &soft_l, bool &soft_r, bool &soft_t, bool &soft_b) const
{
	soft_l = soft_r = soft_t = soft_b = false;
	if (!have_goal_)
		return;
	for (size_t i = 0; i < boxes_.size() && i < gives_.size(); i++) {
		if (!gives_[i])
			continue;
		const Box &o = boxes_[i];
		const bool beside = o.y < goal_.y + goal_.h && o.y + o.h > goal_.y;
		const bool above_below = o.x < goal_.x + goal_.w && o.x + o.w > goal_.x;
		// By which side of the space's middle it lies on, not by
		// whether it touches the space: it may have grown into
		// the space since that was worked out, and judged by
		// touching, the two filters chased each other for good.
		const double mid_x = goal_.x + goal_.w / 2.0, mid_y = goal_.y + goal_.h / 2.0;
		soft_l = soft_l || (beside && o.x + o.w <= mid_x);
		soft_r = soft_r || (beside && o.x >= mid_x);
		soft_t = soft_t || (above_below && !beside && o.y + o.h <= mid_y);
		soft_b = soft_b || (above_below && !beside && o.y >= mid_y);
	}
}

// Where a kept-size picture goes in a space.
Box FitEngine::kept_in(const Prep &p, const Box &room, double kw, double kh) const
{
	bool soft_l = false, soft_r = false, soft_t = false, soft_b = false;
	if (p.free)
		soft_sides(soft_l, soft_r, soft_t, soft_b);
	// (A side it is to be kept towards counts unless that side is
	// the one that gives way: the two would close in for good.)
	auto along = [](double from, double size, double own, bool soft_near, bool soft_far, int toward) {
		if (soft_near && !soft_far)
			return from + size - own; // against the far, firm side
		if (soft_far && !soft_near)
			return from;
		if (toward != 0)
			return toward < 0 ? from : from + size - own;
		return from + (size - own) / 2.0;
	};
	return Box{along(room.x, room.w, kw, soft_l, soft_r, p.s.toward_x),
		   along(room.y, room.h, kh, soft_t, soft_b, p.s.toward_y), kw, kh};
}

// What will show of it once it is where it is going, for any other Fit
// to Space in this scene to go by.
void FitEngine::publish_claim(const Prep &p)
{
	const FitSettings &s = p.s;
	const Box outer{p.bx, p.by, p.bw, p.bh};
	if (p.free && have_goal_) {
		Claim claim;
		claim.resizes = s.resize;
		if (!s.resize && have_keep_) {
			claim.box = kept_in(p, goal_, p.keep_w, p.keep_h);
			const double gap = std::min({s.margin, outer.w / 2.0 - 1.0, outer.h / 2.0 - 1.0});
			if (outer.w - 2.0 * gap >= p.keep_w)
				claim.box.x =
					std::clamp(claim.box.x, outer.x + gap, outer.x + outer.w - gap - p.keep_w);
			if (outer.h - 2.0 * gap >= p.keep_h)
				claim.box.y =
					std::clamp(claim.box.y, outer.y + gap, outer.y + outer.h - gap - p.keep_h);
		} else {
			const double kg = std::min(goal_.w / p.pw, goal_.h / p.ph);
			claim.box = Box{goal_.x + (goal_.w - p.pw * kg) * (s.toward_x + 1) / 2.0,
					goal_.y + (goal_.h - p.ph * kg) * (s.toward_y + 1) / 2.0, p.pw * kg, p.ph * kg};
		}
		claim_set(parent_, obs_sceneitem_get_id(p.item), this, claim);
		last_claim_ = claim;
		have_claim_ = true;
	} else {
		claim_clear(this);
		have_claim_ = false;
	}
}

void FitEngine::apply(const Prep &p, Box space)
{
	const FitSettings &s = p.s;
	obs_sceneitem_t *item = p.item;
	const obs_sceneitem_crop &crop = p.crop;
	double bx = p.bx, by = p.by, bw = p.bw, bh = p.bh;
	// The screen (or the box) itself, before a free space is taken in it.
	const Box outer{bx, by, bw, bh};
	const bool free = p.free;
	if (free) {
		bx = space.x;
		by = space.y;
		bw = space.w;
		bh = space.h;
	}
	// The same shape as the box fills it; any other shape keeps clear of
	// its edges by the margin. The free space already has the margin
	// taken off all round.
	const double box = bw / bh;
	// (The shape that counts is the picture's, also when its shadow is
	// fitted with it.)
	// (Or, when only part of the item shows, the shape of that part.)
	const bool cut_down = s.visible_pixels && own_cut_[0] + own_cut_[1] + own_cut_[2] + own_cut_[3] > 0.0;
	const double bare = !cut_down && p.bare_w >= 2.0 && p.bare_h >= 2.0 ? p.bare_w / p.bare_h : p.picture;
	const bool fills = !free && std::fabs(bare - box) / box < 0.01;
	double margin = fills || free ? 0.0 : std::min({s.margin, bw / 2.0 - 1.0, bh / 2.0 - 1.0});
	if (!s.resize && have_keep_) {
		// Its own size, in the middle of the room it has: the gaps are
		// balanced, left with right and top with bottom.
		const Box put = kept_in(p, Box{bx + margin, by + margin, bw - 2.0 * margin, bh - 2.0 * margin},
					p.keep_w, p.keep_h);
		bx = put.x;
		by = put.y;
		// A space smaller than the item cannot hold it, and its
		// middle may then lie off the screen: the item stays the gap
		// inside the screen's edges whatever else gives way. (The
		// owner's chat box was pushed off the canvas by a game that
		// grew beside it; kept on the canvas, the game makes room.)
		const double gap = std::min({s.margin, outer.w / 2.0 - 1.0, outer.h / 2.0 - 1.0});
		auto inside = [](double at, double size, double from, double room) {
			return room >= size ? std::clamp(at, from, from + room - size) : from + (room - size) / 2.0;
		};
		bx = inside(bx, p.keep_w, outer.x + gap, outer.w - 2.0 * gap);
		by = inside(by, p.keep_h, outer.y + gap, outer.h - 2.0 * gap);
		bw = p.keep_w;
		bh = p.keep_h;
		margin = 0.0;
	}
	const double aw = bw - 2.0 * margin, ah = bh - 2.0 * margin;
	const double k = std::min(aw / p.pw, ah / p.ph);
	// The fit itself is left to OBS's bounding box, which OBS works out
	// afresh for every frame it draws. The box is the space the picture
	// may take, grown by whatever the item draws round the picture (a
	// shadow's room) at this scale, so that OBS's own "scale to inner
	// bounds" lands the picture exactly in that space, centred. While the
	// picture only changes size, as a window does while it is dragged
	// larger, the box stays as it is and OBS keeps the picture fitted on
	// every frame: nothing here has to catch up a frame later.
	const double room_l = p.left - crop.left, room_t = p.top - crop.top;
	const double room_r = p.right - crop.right, room_b = p.bottom - crop.bottom;
	const double want_w = aw + (room_l + room_r) * k, want_h = ah + (room_t + room_b) * k;
	const double x = bx + margin - room_l * k;
	const double y = by + margin - room_t * k;

	// A picture of another shape than its space floats in the middle
	// of it, or lies against the side it is kept towards.
	const uint32_t toward = (s.toward_x < 0   ? OBS_ALIGN_LEFT
				 : s.toward_x > 0 ? OBS_ALIGN_RIGHT
						  : OBS_ALIGN_CENTER) |
				(s.toward_y < 0   ? OBS_ALIGN_TOP
				 : s.toward_y > 0 ? OBS_ALIGN_BOTTOM
						  : OBS_ALIGN_CENTER);
	vec2 pos, bounds;
	obs_sceneitem_get_pos(item, &pos);
	obs_sceneitem_get_bounds(item, &bounds);
	const bool placed = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER &&
			    obs_sceneitem_get_alignment(item) == (OBS_ALIGN_LEFT | OBS_ALIGN_TOP) &&
			    obs_sceneitem_get_bounds_alignment(item) == toward && std::fabs(pos.x - x) < 0.05 &&
			    std::fabs(pos.y - y) < 0.05 && std::fabs(bounds.x - want_w) < 0.05 &&
			    std::fabs(bounds.y - want_h) < 0.05 && obs_sceneitem_get_rot(item) == 0.0f;
	set_x_ = x;
	set_y_ = y;
	set_bw_ = want_w;
	set_bh_ = want_h;
	have_set_ = true;
	was_placed_ = placed;
	if (placed) {
		// At rest where it belongs. "Same place in other scenes": the
		// same source in the scenes that are not on screen is put
		// exactly there too, each time this place changes, so that
		// it is already there when one of them is switched to.
		if (share_again_.exchange(false))
			have_shared_ = false;
		if (glide_ >= 1.0 && obs_source_showing(parent_) &&
		    !(have_shared_ && std::fabs(shared_[0] - x) < 0.05 && std::fabs(shared_[1] - y) < 0.05 &&
		      std::fabs(shared_[2] - want_w) < 0.05 && std::fabs(shared_[3] - want_h) < 0.05 &&
		      shared_toward_ == toward)) {
			have_shared_ = true;
			shared_[0] = x;
			shared_[1] = y;
			shared_[2] = want_w;
			shared_[3] = want_h;
			shared_toward_ = toward;
			// The scenes listed here, and any whose own filter for
			// this item lists this scene: a link holds both ways.
			std::vector<std::string> with = s.share_scenes;
			share_partners(this, s.item, with);
			if (!with.empty()) {
				const double home[2] = {s.anchor_x, s.anchor_y};
				share_place(parent_, item, with, free && s.have_anchor ? home : nullptr);
			}
		}
		return;
	}
	obs_sceneitem_defer_update_begin(item);
	obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
	obs_sceneitem_set_rot(item, 0.0f);
	obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
	obs_sceneitem_set_bounds_alignment(item, toward);
	vec2_set(&bounds, (float)want_w, (float)want_h);
	obs_sceneitem_set_bounds(item, &bounds);
	vec2_set(&pos, (float)x, (float)y);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_defer_update_end(item);
}

void FitEngine::tick(float seconds, FitScan &scan)
{
	if (!parent_ || !obs_source_enabled(source_)) {
		claim_clear(this);
		return;
	}
	// Not on a group: its size is whatever is in it, so there is no
	// space to fit into, and moving an item inside it moves the space.
	if (obs_source_is_group(parent_))
		return;
	if (idle(seconds))
		return;
	// Held for the length of this tick: OBS's window may remove the item
	// at any moment from its own thread.
	struct Held {
		obs_sceneitem_t *item;
		~Held()
		{
			if (item)
				obs_sceneitem_release(item);
		}
	} held{find_item(parent_, item_name())};
	if (held.item)
		obs_sceneitem_addref(held.item);
	Prep p;
	if (!prepare(held.item, seconds, p))
		return;
	Box space;
	if (p.free) {
		const Motion m = motion(p, seconds);
		if (m == Motion::Stop)
			return; // being moved by hand, or no room at all
		if (m == Motion::Solve) {
			// What is in the way. Drawing an item to see what shows in
			// it is costly (it stalls the picture for a moment), so it
			// is done as seldom as can be: once for each item when it
			// is first met, and after that only when a glance shows it
			// has changed. An item that only moves or changes size is
			// followed by its box, which costs nothing: what shows in
			// it keeps its place in the box.
			if (look_again())
				scan.forget();
			scan.gather(parent_, p.item, this);
			std::vector<Obstacle> in_way;
			scan.obstacles(parent_, p.item, p.s.skip, p.s.skip_rule, this, p.how, in_way);
			bool at_rest = false;
			observe(in_way, arriving(), seconds, at_rest);
			if (at_rest)
				scan.at_rest(parent_, {p.item}, this, p.how);
			bool moved = false;
			if (!solve(p, seconds, true, moved))
				return;
			if (moved)
				glides_++;
			space = advance(p, seconds);
		} else {
			space = cur_;
		}
	} else {
		have_cur_ = false;
	}
	publish_claim(p);
	apply(p, space);
}

// ---------------------------------------------------------------- the scan

void FitScan::gather(obs_source_t *scene, obs_sceneitem_t *self, const void *asking)
{
	gathered_.clear();
	used_.clear();
	struct Gather {
		std::vector<Gathered> *out;
		obs_sceneitem_t *self;
		obs_source_t *scene;
		const void *asking;
		obs_sceneitem_t *top;
	} gather{&gathered_, self, scene, asking, nullptr};
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *other, void *param) {
		auto *g = static_cast<Gather *>(param);
		if (other == g->self || !obs_sceneitem_visible(other) || obs_sceneitem_is_group(other) ||
		    is_morph_layer(other))
			return true;
		g->out->push_back(Gathered{other, g->top ? g->top : other, false});
		return true;
	};
	obs_scene_enum_items(
		obs_scene_from_source(scene),
		[](obs_scene_t *sc, obs_sceneitem_t *other, void *param) {
			auto *g = static_cast<Gather *>(param);
			if (other == g->self)
				return true;
			if (obs_sceneitem_is_group(other) && obs_sceneitem_visible(other)) {
				// A group another fit is placing counts as one thing,
				// where that fit is taking it.
				Claim claim;
				if (claim_of(g->scene, obs_sceneitem_get_id(other), g->asking, claim)) {
					g->out->push_back(Gathered{other, other, true});
				} else {
					g->top = other;
					obs_sceneitem_group_enum_items(other, add, param);
					g->top = nullptr;
				}
			}
			add(sc, other, param);
			return true;
		},
		&gather);
}

void FitScan::look(obs_source_t *scene, obs_sceneitem_t *other, const Measure &how)
{
	Seen seen;
	seen.there = measure_seen(scene, other, how, seen.box);
	seen.item_at = scene_box(scene, other);
	seen_[obs_sceneitem_get_id(other)] = seen;
	// What a glance last said of it goes with this look: another
	// look is due when a glance says otherwise.
	Glance glance;
	if (glance_of(scene, obs_sceneitem_get_id(other), glance))
		looked_[obs_sceneitem_get_id(other)] = glance;
	else
		looked_.erase(obs_sceneitem_get_id(other));
}

void FitScan::obstacles(obs_source_t *scene, obs_sceneitem_t *self, const std::vector<std::string> &skip,
			bool keep_skipped, const void *asking, const Measure &how, std::vector<Obstacle> &out,
			const std::vector<int64_t> *twins_of)
{
	out.clear();
	if (look_again_) {
		look_again_ = false;
		seen_.clear();
	}
	for (const Gathered &g : gathered_) {
		if (g.item == self || g.top == self)
			continue;
		bool passed = false;
		if (!g.claimed) {
			const char *name = obs_source_get_name(obs_sceneitem_get_source(g.item));
			if (name && std::find(skip.begin(), skip.end(), name) != skip.end()) {
				if (!keep_skipped)
					continue;
				passed = true;
			}
			// (A stand-in for one of the host's own items holds that
			// item's room, which the item's claim already says.)
			if (twins_of) {
				const int64_t of = stand_in_for_item(g.item);
				if (of && std::find(twins_of->begin(), twins_of->end(), of) != twins_of->end())
					continue;
			}
		}
		if (std::find(used_.begin(), used_.end(), g.item) == used_.end())
			used_.push_back(g.item);
		const int64_t id = obs_sceneitem_get_id(g.item);
		Claim claim;
		if (claim_of(scene, id, asking, claim)) {
			out.push_back(Obstacle{claim.box, id, true, claim.resizes, passed});
			continue;
		}
		auto found = seen_.find(id);
		if (found == seen_.end()) {
			look(scene, g.item, how);
			found = seen_.find(id);
		}
		const Seen &seen = found->second;
		if (!seen.there)
			continue;
		const Box now = scene_box(scene, g.item);
		if (seen.item_at.w < 1.0 || seen.item_at.h < 1.0 || now.w < 1.0 || now.h < 1.0) {
			out.push_back(Obstacle{seen.box, id, false, false, passed});
			continue;
		}
		const double kx = now.w / seen.item_at.w, ky = now.h / seen.item_at.h;
		out.push_back(
			Obstacle{Box{now.x + (seen.box.x - seen.item_at.x) * kx,
				     now.y + (seen.box.y - seen.item_at.y) * ky, seen.box.w * kx, seen.box.h * ky},
				 id, false, false, passed});
	}
}

void FitScan::at_rest(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &extra, const void *asking,
		      const Measure &how)
{
	std::vector<obs_sceneitem_t *> watched = used_;
	for (obs_sceneitem_t *item : extra)
		if (std::find(watched.begin(), watched.end(), item) == watched.end())
			watched.push_back(item);
	glance_step(asking, scene, watched);
	for (obs_sceneitem_t *other : used_) {
		const int64_t id = obs_sceneitem_get_id(other);
		Claim claim;
		Glance glance;
		if (claim_of(scene, id, asking, claim) || !glance_of(scene, id, glance))
			continue;
		// (Its first glance since it was looked at is no measure
		// of that look: the item may have changed in between,
		// seconds apart as they can be. It is looked at once
		// more, and the two go together from then on.)
		const auto was = looked_.find(id);
		if (was == looked_.end() || glance_differs(glance, was->second)) {
			look(scene, other, how);
			break; // one look a tick is enough
		}
	}
}

namespace {
void share_partners(FitEngine *asking, const std::string &item, std::vector<std::string> &with)
{
	const char *here = asking->scene_name();
	if (!here)
		return;
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *filter : g_filters) {
		const char *there = filter == asking ? nullptr : filter->scene_name();
		if (there && filter->shares_with(item, here) &&
		    std::find(with.begin(), with.end(), there) == with.end())
			with.push_back(there);
	}
}
} // namespace

void engines_placed(obs_source_t *scene, const char *item_name)
{
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *filter : g_filters)
		filter->placed(scene, item_name);
}

void engines_shared(obs_source_t *scene, const char *item_name, const double *home)
{
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *filter : g_filters)
		filter->shared(scene, item_name, home);
}

void engine_counts(obs_source_t *owner, const char *item, uint64_t &ticks, uint64_t &glides)
{
	ticks = glides = 0;
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *each : g_filters) {
		if (each->source() != owner || (item && *item && each->item_name() != item))
			continue;
		ticks += each->full_ticks();
		glides += each->glides();
	}
}

std::vector<obs_source_t *> engine_owners()
{
	std::vector<obs_source_t *> owners;
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *each : g_filters) {
		if (!each->prefix().empty())
			continue; // a Scene Fit follows a rename itself
		if (obs_source_t *held = obs_source_get_ref(each->source()))
			owners.push_back(held);
	}
	return owners;
}

// ------------------------------------------------------------ the controls

namespace {

// What a control's callback needs to know about the fit it belongs to, kept
// for as long as the plugin is loaded (OBS holds the pointer).
struct Keys {
	std::string prefix, list_prefix;
	bool entry = false;
};

const Keys *keys_for(const std::string &prefix, const std::string &list_prefix, bool entry)
{
	static std::mutex lock;
	static std::map<std::string, std::unique_ptr<Keys>> all;
	std::lock_guard hold(lock);
	auto &slot = all[prefix + '\x1f' + list_prefix +
			 (entry ? "\x1f"
				  "1"
				: "\x1f"
				  "0")];
	if (!slot)
		slot = std::make_unique<Keys>(Keys{prefix, list_prefix, entry});
	return slot.get();
}

bool box_modified(void *priv, obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto *keys = static_cast<const Keys *>(priv);
	fit_controls_shown(props, settings, keys->prefix, keys->list_prefix, keys->entry);
	return true;
}

} // namespace

void fit_controls_shown(obs_properties_t *props, obs_data_t *settings, const std::string &prefix,
			const std::string &list_prefix, bool entry)
{
	auto get = [&](const char *name) {
		return obs_properties_get(props, (prefix + name).c_str());
	};
	auto shown = [](obs_property_t *p, bool visible) {
		if (p)
			obs_property_set_visible(p, visible);
	};
	const long long kind = obs_data_get_int(settings, (prefix + "box").c_str());
	for (const char *name : {"box_x", "box_y", "box_width", "box_height"})
		shown(get(name), kind == 1);
	shown(obs_properties_get(props, (list_prefix + "skip|alist").c_str()), kind == 2);
	for (const char *name : {"glide_ms", "ease_curve", "ease_mode", "visible_pixels", "tolerance"})
		shown(get(name), kind == 2);
	// (For a Scene Fit's entry the switch is the Scene Fit's own, for all.)
	if (!entry)
		shown(get("skip_fill_on"), kind == 2);
	const bool rule = kind == 2 &&
			  obs_data_get_bool(settings, entry ? "skip_fill_on" : (prefix + "skip_fill_on").c_str());
	for (const char *name : {"skip_use_pct", "skip_use_px"})
		shown(get(name), rule);
	shown(get("skip_fill"), rule && obs_data_get_bool(settings, (prefix + "skip_use_pct").c_str()));
	for (const char *name : {"skip_px_top", "skip_px_bottom", "skip_px_left", "skip_px_right"})
		shown(get(name), rule && obs_data_get_bool(settings, (prefix + "skip_use_px").c_str()));
	if (obs_property_t *margin = get("margin"))
		obs_property_set_description(margin, obs_module_text(kind == 2 ? "Fit.Gap" : "Fit.Margin"));
}

void fit_add_controls(obs_properties_t *props, const std::string &prefix, const std::string &list_prefix,
		      obs_source_t *owner, obs_source_t *parent, bool entry)
{
	const Keys *keys = keys_for(prefix, list_prefix, entry);
	auto key = [&](const char *name) {
		return prefix + name;
	};
	obs_property_t *box = obs_properties_add_list(props, key("box").c_str(), obs_module_text("Fit.Box"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Scene"), (int)BoxKind::WholeScene);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Custom"), (int)BoxKind::Custom);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Free"), (int)BoxKind::FreeSpace);
	obs_property_set_long_description(box, obs_module_text("Fit.Box.Tip"));
	auto number = [&](const char *name, const char *text) {
		obs_property_t *p = obs_properties_add_float(props, key(name).c_str(), obs_module_text(text), -16384.0,
							     16384.0, 1.0);
		obs_property_float_set_suffix(p, " px");
	};
	number("box_x", "Fit.BoxX");
	number("box_y", "Fit.BoxY");
	number("box_width", "Fit.BoxWidth");
	number("box_height", "Fit.BoxHeight");

	obs_property_t *margin = obs_properties_add_float_slider(props, key("margin").c_str(),
								 obs_module_text("Fit.Margin"), 0.0, 400.0, 1.0);
	obs_property_float_set_suffix(margin, " px");
	obs_property_set_long_description(margin, obs_module_text("Fit.Margin.Tip"));
	obs_property_t *resize = obs_properties_add_bool(props, key("resize").c_str(), obs_module_text("Fit.Resize"));
	obs_property_set_long_description(resize, obs_module_text("Fit.Resize.Tip"));
	obs_property_t *toward = obs_properties_add_list(props, key("toward").c_str(), obs_module_text("Fit.Toward"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// Saved by number: new ones go at the end.
	int toward_value = 0;
	for (const char *name :
	     {"Fit.Toward.Middle", "Fit.Toward.Top", "Fit.Toward.Bottom", "Fit.Toward.Left", "Fit.Toward.Right",
	      "Fit.Toward.TopLeft", "Fit.Toward.TopRight", "Fit.Toward.BottomLeft", "Fit.Toward.BottomRight"})
		obs_property_list_add_int(toward, obs_module_text(name), toward_value++);
	obs_property_set_long_description(toward, obs_module_text("Fit.Toward.Tip"));
	obs_property_t *tolerance = obs_properties_add_float_slider(props, key("tolerance").c_str(),
								    obs_module_text("Fit.Tolerance"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(tolerance, " px");
	obs_property_set_long_description(tolerance, obs_module_text("Fit.Tolerance.Tip"));
	obs_property_t *visible_pixels =
		obs_properties_add_bool(props, key("visible_pixels").c_str(), obs_module_text("Measure.Visible"));
	obs_property_set_long_description(visible_pixels, obs_module_text("Fit.Visible.Tip"));
	obs_property_t *no_shadows =
		obs_properties_add_bool(props, key("no_shadows").c_str(), obs_module_text("Measure.NoShadows"));
	obs_property_set_long_description(no_shadows, obs_module_text("Fit.NoShadows.Tip"));
	obs_property_t *glide =
		obs_properties_add_int(props, key("glide_ms").c_str(), obs_module_text("Fit.Glide"), 0, 10000, 50);
	obs_property_int_set_suffix(glide, " ms");
	obs_property_set_long_description(glide, obs_module_text("Fit.Glide.Tip"));
	add_easing_properties(props, prefix.c_str());

	PickList skip;
	const std::string skip_setting = list_prefix + "skip";
	skip.setting = skip_setting.c_str();
	skip.title = entry ? "SceneFit.SkipFor" : "Fit.Skip";
	skip.add = entry ? "SceneFit.AddSource" : "List.AddItem";
	skip.tip = "Fit.Skip.Tip";
	skip.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, owner, skip);
	if (!entry) {
		obs_property_t *skip_fill_on =
			obs_properties_add_bool(props, key("skip_fill_on").c_str(), obs_module_text("Fit.SkipFillOn"));
		obs_property_set_long_description(skip_fill_on, obs_module_text("Fit.SkipFillOn.Tip"));
		obs_property_set_modified_callback2(skip_fill_on, box_modified, (void *)keys);
	}
	obs_property_t *use_pct =
		obs_properties_add_bool(props, key("skip_use_pct").c_str(), obs_module_text("Fit.SkipUsePct"));
	obs_property_set_long_description(use_pct, obs_module_text("Fit.SkipUsePct.Tip"));
	obs_property_set_modified_callback2(use_pct, box_modified, (void *)keys);
	obs_property_t *skip_fill = obs_properties_add_float_slider(props, key("skip_fill").c_str(),
								    obs_module_text("Fit.SkipFill"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(skip_fill, "%");
	obs_property_set_long_description(skip_fill, obs_module_text("Fit.SkipFill.Tip"));
	obs_property_t *use_px =
		obs_properties_add_bool(props, key("skip_use_px").c_str(), obs_module_text("Fit.SkipUsePx"));
	obs_property_set_long_description(use_px, obs_module_text("Fit.SkipUsePx.Tip"));
	obs_property_set_modified_callback2(use_px, box_modified, (void *)keys);
	static const char *const kEdges[][2] = {{"skip_px_top", "Fit.SkipPxTop"},
						{"skip_px_bottom", "Fit.SkipPxBottom"},
						{"skip_px_left", "Fit.SkipPxLeft"},
						{"skip_px_right", "Fit.SkipPxRight"}};
	for (const auto &edge : kEdges) {
		obs_property_t *px = obs_properties_add_float_slider(props, key(edge[0]).c_str(),
								     obs_module_text(edge[1]), 0.0, 2000.0, 1.0);
		obs_property_float_set_suffix(px, " px");
		obs_property_set_long_description(px, obs_module_text("Fit.SkipPx.Tip"));
	}
	PickList share;
	const std::string share_setting = list_prefix + "share_scenes";
	share.setting = share_setting.c_str();
	share.title = entry ? "SceneFit.ShareFor" : "Fit.Share";
	share.add = "List.AddScene";
	share.tip = "Fit.Share.Tip";
	share.choices = [parent](std::vector<std::string> &names) {
		struct Other {
			std::vector<std::string> *names;
			obs_source_t *self;
		} other{&names, parent};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *o = static_cast<Other *>(param);
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene) && scene != o->self)
					o->names->push_back(name);
				return true;
			},
			&other);
	};
	pick_list_add(props, owner, share);
	// Last, so that everything it shows and hides is there.
	obs_property_set_modified_callback2(box, box_modified, (void *)keys);
}

void fit_entry_defaults(obs_data_t *settings, const std::string &prefix)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	auto key = [&](const char *name) {
		return prefix + name;
	};
	obs_data_set_default_int(settings, key("box").c_str(), (int)BoxKind::FreeSpace);
	obs_data_set_default_double(settings, key("box_x").c_str(), 0.0);
	obs_data_set_default_double(settings, key("box_y").c_str(), 0.0);
	obs_data_set_default_double(settings, key("box_width").c_str(), have ? ovi.base_width : 1920);
	obs_data_set_default_double(settings, key("box_height").c_str(), have ? ovi.base_height : 1080);
	// (A gap of 30, not Fit to Space's 80: a Scene Fit is made for layouts
	// in the free space.)
	obs_data_set_default_double(settings, key("margin").c_str(), 30.0);
	obs_data_set_default_double(settings, key("tolerance").c_str(), 8.0);
	obs_data_set_default_double(settings, key("skip_fill").c_str(), 95.0);
	obs_data_set_default_bool(settings, key("skip_use_pct").c_str(), true);
	obs_data_set_default_bool(settings, key("skip_use_px").c_str(), false);
	for (const char *name : {"skip_px_top", "skip_px_bottom", "skip_px_left", "skip_px_right"})
		obs_data_set_default_double(settings, key(name).c_str(), 50.0);
	obs_data_set_default_bool(settings, key("resize").c_str(), true);
	obs_data_set_default_int(settings, key("toward").c_str(), 0);
	obs_data_set_default_bool(settings, key("visible_pixels").c_str(), true);
	obs_data_set_default_bool(settings, key("no_shadows").c_str(), true);
	obs_data_set_default_int(settings, key("glide_ms").c_str(), 300);
	obs_data_set_default_int(settings, key("ease_curve").c_str(), (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, key("ease_mode").c_str(), (int)EaseMode::InOut);
}

} // namespace stagekit::fit
