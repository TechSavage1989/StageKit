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
#include "../common/filter-order.hpp"
#include "../common/holds-hidden.hpp"

#include "../common/timing.hpp"

#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../dock/arrange-tools.hpp"
#include "../dock/arrange.hpp"
#include "../dock/preview-guides.hpp"
#include "../shadow/shadow-backdrop.hpp"
#include "../capture/app-capture.hpp"
#include "../capture/standby.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
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

// Raised whenever a claim is made, changed or taken back, and whenever a
// glance sees an item draw another part of itself: what a Scene Fit at rest
// goes by besides `scene_revision` (`SceneFit::tick`).
std::atomic<uint64_t> g_claims_rev{0}, g_glance_rev{0};

bool same_box(const Box &a, const Box &b)
{
	return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

bool same_claim(const Claim &a, const Claim &b)
{
	return same_box(a.box, b.box) && a.resizes == b.resizes && same_box(a.least, b.least) &&
	       a.has_least == b.has_least && a.rank.by_list == b.rank.by_list && a.rank.level == b.rank.level &&
	       a.rank.filter == b.rank.filter && a.rank.entry == b.rank.entry && same_box(a.home, b.home) &&
	       a.has_home == b.has_home;
}

void claim_set(obs_source_t *scene, int64_t item, const void *owner, const Claim &claim)
{
	std::lock_guard lock(g_claims_mutex);
	bool changed = false;
	for (auto it = g_claims.begin(); it != g_claims.end();)
		if (it->second.first == owner && it->first != std::make_pair(scene, item)) {
			it = g_claims.erase(it);
			changed = true;
		} else {
			++it;
		}
	auto &slot = g_claims[{scene, item}];
	if (changed || slot.first != owner || !same_claim(slot.second, claim))
		g_claims_rev++;
	slot = {owner, claim};
}

void claim_clear(const void *owner)
{
	std::lock_guard lock(g_claims_mutex);
	for (auto it = g_claims.begin(); it != g_claims.end();)
		if (it->second.first == owner) {
			it = g_claims.erase(it);
			g_claims_rev++;
		} else {
			++it;
		}
}

uint64_t claims_revision()
{
	return g_claims_rev.load();
}

uint64_t glance_revision()
{
	return g_glance_rev.load();
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
			if (was == g.seen.end() || glance_differs(seen, was->second))
				g_glance_rev++;
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
namespace {
Box free_space_round(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape,
		     double tolerance, const std::vector<bool> *firm)
{
	const double l0 = within.x + gap, t0 = within.y + gap;
	const double r0 = within.x + within.w - gap, b0 = within.y + within.h - gap;
	std::vector<Box> in_way;
	std::vector<double> lefts{l0}, rights{r0}, tops{t0}, bottoms{b0};
	for (size_t i = 0; i < others.size(); i++) {
		const Box &o = others[i];
		const bool must = firm && i < firm->size() && (*firm)[i];
		if (!must && ax > o.x && ax < o.x + o.w && ay > o.y && ay < o.y + o.h)
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
} // namespace

Box free_space(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape,
	       double tolerance, const std::vector<bool> *firm)
{
	const double l0 = within.x + gap, t0 = within.y + gap;
	const double r0 = within.x + within.w - gap, b0 = within.y + within.h - gap;
	ax = std::clamp(ax, l0, std::max(l0, r0));
	ay = std::clamp(ay, t0, std::max(t0, b0));
	// A fit that takes its space first may have taken the very point this
	// one's space is looked for round (its home). That one is in the way
	// all the same, and the space is looked for just off each of its sides
	// in turn, the one holding the biggest picture taken. The point itself
	// is not changed: once the other has gone, the space is round it again.
	auto inside_firm = [&](double x, double y) -> int {
		for (size_t i = 0; firm && i < others.size() && i < firm->size(); i++) {
			if (!(*firm)[i])
				continue;
			const Box &o = others[i];
			if (x > o.x - gap && x < o.x + o.w + gap && y > o.y - gap && y < o.y + o.h + gap)
				return (int)i;
		}
		return -1;
	};
	const int in = inside_firm(ax, ay);
	if (in < 0)
		return free_space_round(within, ax, ay, others, gap, shape, tolerance, firm);
	const Box &o = others[(size_t)in];
	const double off = 0.01;
	const double tries[4][2] = {{o.x - gap - off, ay},
				    {o.x + o.w + gap + off, ay},
				    {ax, o.y - gap - off},
				    {ax, o.y + o.h + gap + off}};
	Box best{};
	double best_k = -1.0;
	for (const auto &at : tries) {
		const double x = at[0], y = at[1];
		if (x < l0 || x > r0 || y < t0 || y > b0 || inside_firm(x, y) >= 0)
			continue;
		const Box one = free_space_round(within, x, y, others, gap, shape, tolerance, firm);
		if (one.w < 2.0 || one.h < 2.0)
			continue;
		const double k = shape > 0.0 ? std::min(one.w / shape, one.h) : one.w * one.h;
		if (k > best_k + 0.01) {
			best = one;
			best_k = k;
		}
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

FitEngine::FitEngine(obs_data_t *settings, obs_source_t *owner, std::string prefix, std::string entry,
		     std::string look_prefix, std::string look_list_prefix)
	: source_(owner),
	  prefix_(std::move(prefix)),
	  entry_(std::move(entry)),
	  list_prefix_(entry_.empty() ? std::string() : fit_list_prefix(entry_))
{
	set_look(std::move(look_prefix), std::move(look_list_prefix));
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

// How the fits of one scene settle (the owner, 2026-11-19, shown a kept-size
// chat box packed under a resized camera and the same chat box in the middle
// of the room below it: "b is the correct logic. these features need to work
// with any layout format in the future, and not be specific to mine").
//
// The rule, for any layout: Align in the space always decides where a source
// lies in its space. A source kept at its size lies there whatever is beside
// it; a resized picture lies there in the room it does not fill. Nothing is
// shaped round one scene: a special case has to be something a setting says.
//
// What makes that come to rest in one glide is the order things depend on
// each other in, which is one-way between the two kinds of fit:
//   1. A fit that resizes its item ("Resize to fit" means fill) goes by what
//      a kept-size fit of its own standing HOLDS: that source's size at its
//      home, the place it was given (`Claim::least`). It never looks at where
//      that source lies in its space. So it takes all the room there is round
//      the homes of the kept-size sources, and what it takes changes only
//      when a kept-size source is moved or resized by hand, shown or hidden.
//   2. A fit that keeps its item's size goes by where the resized pictures
//      are going (their claims: the picture, not the space it leaves empty),
//      and lies by its alignment in the room that leaves it. Its home lies
//      in that room, because the resized fits kept clear of it.
// A kept-size source that lies elsewhere than at its home therefore moves
// nothing: no resized fit follows it, and there is nothing to chase. Up to
// betaNEXT a resized fit went by where the kept-size source was going, and
// that source by the resized one's claim: centred between a wall and
// something that makes room, it never came to rest (it made room, the middle
// moved, it made room again), which is what two older rules were for: one
// sent the kept-size source against the far, firm side whatever its
// alignment said, the other packed it against a picture with no use for more
// room (`soft_sides_of`, `lean_y_of`, `trim_for_kept_in`). Both are gone.
// (Between two kept-size fits, and between two resized ones, nothing
// changed: they go by each other's claims as before. Space priority still
// says who makes room for whom where the user has set it.)
namespace {
// Where a kept-size picture lies in a room: by its Align in the space, and
// by nothing else.
Box kept_in_room(const Box &room, double kw, double kh, int toward_x, int toward_y)
{
	return Box{room.x + (room.w - kw) * (toward_x + 1) / 2.0, room.y + (room.h - kh) * (toward_y + 1) / 2.0, kw,
		   kh};
}

// G1 (the owner, 2026-11-18): in their GBA column three resized pictures
// share the column's height, each held by the column's width. Each was given
// the roomiest of the spaces that hold its picture as large (`free_space`),
// and its bounding box was that whole space: the camera's box took about
// 1250 px of the column while its 16:9 picture used some 360, and in the
// preview it looked as if it hogged the column. What the others go by was
// always the picture (the claim); the box is now that too, where another
// fit's resized item lies beyond it along the axis the picture does not fill
// (above or below a picture held by its width, beside one held by its height;
// an item kept at its size beyond it counts for nothing: a 4:3 capture in a
// layout scene keeps the capture's whole space beside the column). The
// picture stays exactly where it was in its space (by Align in the space),
// so nothing moves on screen and the claims are as before; and a goal that
// is the picture changes only when the picture does, so the entries of a
// column stop chasing the edges of each other's spaces from pass to pass
// (G4). Alone in its space (a Fit to Space with a window in it) nothing is
// cut: a window dragged taller is still fitted by OBS on every frame.
void trim_to_picture_in(Box &goal, const std::vector<Box> &boxes, const std::vector<bool> &said,
			const std::vector<bool> &gives, const std::vector<bool> &passed, bool avoiding, double pw,
			double ph, int toward_x, int toward_y)
{
	if (pw <= 0.0 || ph <= 0.0)
		return;
	bool cut_y = false, cut_x = false;
	for (size_t i = 0; i < boxes.size() && i < said.size(); i++) {
		// (Only another resized picture, which could use the room: an
		// item kept at its size beside it has no use for it, and the
		// space is then the item's own, as a layout scene's captures
		// are written: each capture in the one space beside the column,
		// whatever its shape.)
		if (!said[i] || i >= gives.size() || !gives[i] || (i < passed.size() && passed[i] && !avoiding))
			continue;
		const Box &o = boxes[i];
		const bool across = o.y < goal.y + goal.h && o.y + o.h > goal.y;
		const bool along = o.x < goal.x + goal.w && o.x + o.w > goal.x;
		cut_y = cut_y || (along && !across);
		cut_x = cut_x || (across && !along);
	}
	const double kg = std::min(goal.w / pw, goal.h / ph);
	const double w = pw * kg, h = ph * kg;
	if (cut_y && goal.h - h > 0.5) {
		goal.y += (goal.h - h) * (toward_y + 1) / 2.0;
		goal.h = h;
	}
	if (cut_x && goal.w - w > 0.5) {
		goal.x += (goal.w - w) * (toward_x + 1) / 2.0;
		goal.w = w;
	}
}

// The space round a fit's place among `in_way` (what is in the way now, or
// the same with each other fit's item at its home), and whether the items
// never in the way counted after all (`avoid`); `counted` is what counted,
// and (`ax_at`, `ay_at`) the place.
Box space_among(const SpaceInputs &in, const std::vector<Box> &in_way, bool &avoid, std::vector<Box> &counted,
		std::vector<bool> &counted_above, double &ax_at, double &ay_at)
{
	const Box &within = in.within;
	// First without the items never in the way. Should the picture
	// then leave too much of its space empty (a narrow window on a
	// wide screen), they count after all: the picture goes beside
	// the camera rather than under it. Both spaces come from the
	// picture's shape alone, not from where it is now, so the
	// choice does not flip once the picture has moved.
	std::vector<Box> firm;
	std::vector<bool> firm_above;
	for (size_t i = 0; i < in_way.size(); i++)
		if (i >= in.passed.size() || !in.passed[i]) {
			firm.push_back(in_way[i]);
			firm_above.push_back(i < in.above.size() && in.above[i]);
		}
	// Where the space is looked for round. What the place lies inside
	// is not in the way (a background, a frame). With All sources in a
	// Scene Fit every source is fitted, and two resized ones whose
	// places lay inside each other's claims took the same space for
	// good: there the place is first taken out of any other fit's
	// claim, to the nearest point clear of it. A claim of a fit that
	// takes its space before this one (Space priority) is left to
	// `free_space`, which looks off each of its sides for the biggest
	// picture; one this fit outranks is not in `boxes` at all.
	double ax = in.ax, ay = in.ay;
	if (in.claims_firm) {
		for (int round = 0; round < 4; round++) {
			bool inside = false;
			for (size_t i = 0; i < in_way.size() && i < in.said.size(); i++) {
				const Box &b = in_way[i];
				if (!in.said[i] || (i < in.above.size() && in.above[i]) ||
				    !(ax > b.x && ax < b.x + b.w && ay > b.y && ay < b.y + b.h))
					continue;
				inside = true;
				const double out = in.margin + 1.0;
				const double l = b.x - out, r = b.x + b.w + out, t = b.y - out, d = b.y + b.h + out;
				struct Way {
					double x, y;
				};
				const Way ways[] = {{l, ay}, {r, ay}, {ax, t}, {ax, d}};
				double best = 1e18;
				Way to = ways[0];
				for (const Way &w : ways) {
					const bool on = w.x >= within.x && w.x <= within.x + within.w &&
							w.y >= within.y && w.y <= within.y + within.h;
					const double far = std::fabs(w.x - ax) + std::fabs(w.y - ay);
					if (on && far < best) {
						best = far;
						to = w;
					}
				}
				ax = to.x;
				ay = to.y;
			}
			if (!inside)
				break;
		}
	}
	Box goal = free_space(within, ax, ay, firm, in.margin, in.shape, in.tolerance, &firm_above);
	avoid = false;
	if (in.skip_rule && firm.size() != in_way.size() && goal.w >= 2.0 && goal.h >= 2.0) {
		double fw = in.kept_w, fh = in.kept_h;
		if (fw <= 0.0 || fh <= 0.0) {
			fw = std::min(goal.w, goal.h * in.shape);
			fh = fw / in.shape;
		}
		// Either rule that is ticked may say the picture fills its
		// space: by the share it takes up, or by the empty room at
		// each edge, the picture lying where it is kept towards.
		// With neither ticked nothing says so.
		const double fill = std::min(1.0, fw / goal.w) * std::min(1.0, fh / goal.h);
		const double spare_w = std::max(0.0, goal.w - fw), spare_h = std::max(0.0, goal.h - fh);
		const double left = spare_w * (in.toward_x + 1) / 2.0, top = spare_h * (in.toward_y + 1) / 2.0;
		const bool fills = (in.skip_pct && fill >= in.skip_fill) ||
				   (in.skip_px && left <= in.px_left + 0.05 && spare_w - left <= in.px_right + 0.05 &&
				    top <= in.px_top + 0.05 && spare_h - top <= in.px_bottom + 0.05);
		if (!fills) {
			const Box beside =
				free_space(within, ax, ay, in_way, in.margin, in.shape, in.tolerance, &in.above);
			if (beside.w >= 2.0 && beside.h >= 2.0) {
				goal = beside;
				avoid = true;
			}
		}
	}
	counted = avoid ? in_way : firm;
	counted_above = avoid ? in.above : firm_above;
	ax_at = ax;
	ay_at = ay;
	return goal;
}
} // namespace

Box space_from(const SpaceInputs &in, SpaceOut *result)
{
	SpaceOut out;
	const Box &within = in.within;
	const bool kept = in.kept_w > 0.0 && in.kept_h > 0.0;
	std::vector<Box> counted;
	std::vector<bool> counted_above;
	double at_x = 0.0, at_y = 0.0;
	Box goal = space_among(in, in.boxes, out.avoiding, counted, counted_above, at_x, at_y);
	// At home: the layout made by hand. With the other entries at their
	// homes, the space round its place is the one it had when the home was
	// taken, the picture there keeps the gap and, for an entry that resizes,
	// the home image already fills that space within tolerance. Something
	// shown, hidden, moved or resized changes that, and it is not at home;
	// once that is undone it is at home again. (New measurements settle
	// before the comparison is made.) What being at home does is below.
	if (in.has_home) {
		const Box &home = in.home;
		// (A fit that takes its space before this one is where it is,
		// not at its home: it may have left its home to grow over this
		// one's, and this one then makes room as it always did, and comes
		// home when that one does. Taken at its home, it was covered.)
		std::vector<Box> at_homes = in.boxes;
		for (size_t i = 0; i < at_homes.size() && i < in.has_homes.size() && i < in.homes.size(); i++)
			if (in.has_homes[i] && !(i < in.above.size() && in.above[i]))
				at_homes[i] = in.homes[i];
		bool home_avoid = false;
		std::vector<Box> home_counted;
		std::vector<bool> home_above;
		double hx = 0.0, hy = 0.0;
		const Box space = space_among(in, at_homes, home_avoid, home_counted, home_above, hx, hy);
		if (in.home_settling) {
			out.took_home = true;
			out.home_space = space;
			out.have_home_space = space.w >= 2.0 && space.h >= 2.0;
			out.at_home = true;
		} else if (in.have_home_space && std::fabs(space.x - in.home_space.x) <= kHomeSlack &&
			   std::fabs(space.y - in.home_space.y) <= kHomeSlack &&
			   std::fabs(space.x + space.w - in.home_space.x - in.home_space.w) <= kHomeSlack &&
			   std::fabs(space.y + space.h - in.home_space.y - in.home_space.h) <= kHomeSlack) {
			// (The gap kept, give or take the measuring; what its place
			// lies inside is not in the way, as for the space.)
			const double m = in.margin - kHomeSlack;
			bool clear = home.x >= within.x + m && home.y >= within.y + m &&
				     home.x + home.w <= within.x + within.w - m &&
				     home.y + home.h <= within.y + within.h - m;
			for (size_t i = 0; clear && i < home_counted.size(); i++) {
				const Box &b = home_counted[i];
				const bool above = i < home_above.size() && home_above[i];
				if (!above && hx > b.x && hx < b.x + b.w && hy > b.y && hy < b.y + b.h)
					continue;
				clear = !(b.x < home.x + home.w + m && b.x + b.w > home.x - m &&
					  b.y < home.y + home.h + m && b.y + b.h > home.y - m);
			}
			// Test the filled image with the other entries at their homes too.
			// Their current positions can still be returning from a visibility change.
			if (clear && !kept && in.pw > 0.0 && in.ph > 0.0) {
				const double k = std::min(space.w / in.pw, space.h / in.ph);
				clear = std::fabs(in.pw * k - home.w) <= in.tolerance &&
					std::fabs(in.ph * k - home.h) <= in.tolerance;
			}
			out.at_home = clear;
		}
	}
	// (At home, the space it would be given: what OBS is given while the
	// picture is of another shape than the one the home was cut to, so a
	// window dragged to another shape is fitted from the first frame, as
	// away from home, `apply`. Given the home box instead, it could only
	// shrink there until the new shape was taken half a second later.)
	out.room = goal;
	// A home is the space an entry belongs to and the size it was given by
	// hand, not a place: Align in the space decides where it lies in its
	// space, at home or not. (The owner, 2026-11-19: "align in the space is
	// not being respected. chat box is not centering". Up to beta198 the
	// goal at home was the home box exactly, so a source lay where it was
	// put by hand whatever its alignment said.) One kept at its size goes
	// by its space as away from home (`claim_box`, `apply`). One that
	// resizes keeps the size it has at home, by its alignment in the space
	// it has now, while the picture that fills that space is within the
	// Size tolerance of it: switching the filter on over a layout made with
	// the gaps changes nothing. Otherwise it fills its space, as away from
	// home. (Judged on the space it has now as well as on the one round its
	// home, above: a neighbour kept at its size may lie elsewhere than at
	// its home, where its own alignment puts it.)
	if (out.at_home && !kept) {
		bool keeps = false;
		if (goal.w >= 2.0 && goal.h >= 2.0 && in.pw > 0.0 && in.ph > 0.0 && in.home.w >= 1.0 &&
		    in.home.h >= 1.0) {
			const double fill = std::min(goal.w / in.pw, goal.h / in.ph);
			const double slack = std::max(kHomeSlack, in.tolerance);
			if (std::fabs(in.pw * fill - in.home.w) <= slack &&
			    std::fabs(in.ph * fill - in.home.h) <= slack) {
				// (Never larger than the space: a home a little too
				// large for it is the picture that fills it.)
				const double k = std::min({fill, in.home.w / in.pw, in.home.h / in.ph});
				const double w = in.pw * k, h = in.ph * k;
				goal = Box{goal.x + (goal.w - w) * (in.toward_x + 1) / 2.0,
					   goal.y + (goal.h - h) * (in.toward_y + 1) / 2.0, w, h};
				keeps = true;
			}
		}
		out.at_home = keeps;
	}
	if (goal.w >= 2.0 && goal.h >= 2.0 && !kept && !out.at_home)
		trim_to_picture_in(goal, in.boxes, in.said, in.gives, in.passed, out.avoiding, in.pw, in.ph,
				   in.toward_x, in.toward_y);
	out.goal = goal;
	if (result)
		*result = std::move(out);
	return goal;
}

Box claim_box(const Box &goal, const Box &room, const Box &within, double margin, bool kept, double kept_w,
	      double kept_h, int toward_x, int toward_y, bool reshaping, double pw, double ph)
{
	if (kept) {
		// By its Align in the space, at home too, the gap inside the
		// screen's edges, as `apply` puts it.
		Box box = kept_in_room(goal, kept_w, kept_h, toward_x, toward_y);
		const double gap = std::min({margin, within.w / 2.0 - 1.0, within.h / 2.0 - 1.0});
		if (within.w - 2.0 * gap >= kept_w)
			box.x = std::clamp(box.x, within.x + gap, within.x + within.w - gap - kept_w);
		if (within.h - 2.0 * gap >= kept_h)
			box.y = std::clamp(box.y, within.y + gap, within.y + within.h - gap - kept_h);
		return box;
	}
	// (While the picture changes shape it is drawn in the space
	// before the cut, `apply`: that is where it shows.)
	const Box &in = reshaping ? room : goal;
	const double kg = std::min(in.w / pw, in.h / ph);
	return Box{in.x + (in.w - pw * kg) * (toward_x + 1) / 2.0, in.y + (in.h - ph * kg) * (toward_y + 1) / 2.0,
		   pw * kg, ph * kg};
}

Box claim_from(const SpaceInputs &in, const SpaceOut *given)
{
	SpaceOut out;
	if (given)
		out = *given;
	else
		space_from(in, &out);
	const bool kept = in.kept_w > 0.0 && in.kept_h > 0.0;
	const double pw = in.pw > 0.0 && in.ph > 0.0 ? in.pw : std::max(1e-6, in.shape);
	const double ph = in.pw > 0.0 && in.ph > 0.0 ? in.ph : 1.0;
	// (As `FitEngine::reshaping`: a picture of another shape than its space
	// was found for, with no window switcher's change under way.)
	const bool reshaping = !kept && !in.with_switch && in.shape > 0.0 && out.room.w >= 2.0 && out.room.h >= 2.0 &&
			       std::fabs(pw / ph - in.shape) > 0.01 * (pw / ph) && !out.at_home;
	return claim_box(out.goal, out.room, in.within, in.margin, kept, in.kept_w, in.kept_h, in.toward_x, in.toward_y,
			 reshaping, pw, ph);
}

bool FitEngine::held(HeldSpace &out)
{
	std::lock_guard lock(held_mutex_);
	out.name = held_name_;
	if (!have_held_)
		return false;
	out.item = held_item_;
	out.space = held_space_;
	out.goal = held_goal_;
	out.resizes = held_resizes_;
	out.in = held_in_;
	out.worked = true;
	return true;
}

bool FitEngine::recently_hidden() const
{
	const uint64_t since = hidden_since_.load();
	return !since || os_gettime_ns() - since < 2000000000ULL;
}

bool fit_settings_for(obs_source_t *scene, const char *name, FitSettings &settings, std::string &resizing_filter,
		      std::string *enabled_filter)
{
	if (enabled_filter)
		enabled_filter->clear();
	if (!name)
		return false;
	bool found = false, found_enabled = false;
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *engine : g_filters) {
		if (engine->parent() != scene)
			continue;
		const FitSettings candidate = engine->settings();
		if (candidate.item != name)
			continue;
		const bool enabled = obs_source_enabled(engine->source());
		if (enabled && candidate.resize) {
			settings = candidate;
			resizing_filter = obs_source_get_name(engine->source());
			if (enabled_filter)
				*enabled_filter = resizing_filter;
			return true;
		}
		if (!found || (enabled && !found_enabled)) {
			settings = candidate;
			found = true;
			found_enabled = enabled;
			if (enabled_filter)
				*enabled_filter = enabled ? obs_source_get_name(engine->source()) : "";
		}
	}
	return found;
}

bool fit_toward_for(obs_source_t *scene, const char *name, int &toward)
{
	FitSettings s;
	std::string resizing;
	if (!fit_settings_for(scene, name, s, resizing))
		return false;
	toward = toward_number(s.toward_x, s.toward_y);
	return true;
}

int fit_set_toward_for(obs_source_t *scene, const char *name, int toward)
{
	if (!scene || !name || toward < 0 || toward > 8)
		return 0;
	// (Found under the lock, set outside it: an update reaches the engines,
	// and a Scene Fit's every one of its entries.)
	std::vector<std::pair<obs_source_t *, std::string>> found;
	{
		std::lock_guard lock(g_filters_mutex);
		for (FitEngine *engine : g_filters) {
			if (engine->parent() != scene || engine->item_name() != name)
				continue;
			const std::string key = engine->look_prefix() + "toward";
			bool seen = false;
			for (const auto &[source, k] : found)
				seen = seen || (source == engine->source() && k == key);
			if (seen)
				continue;
			if (obs_source_t *source = obs_source_get_ref(engine->source()))
				found.emplace_back(source, key);
		}
	}
	int changed = 0;
	for (auto &[source, key] : found) {
		obs_data_t *settings = obs_source_get_settings(source);
		if (obs_data_get_int(settings, key.c_str()) != toward) {
			obs_data_t *change = obs_data_create();
			obs_data_set_int(change, key.c_str(), toward);
			obs_source_update(source, change);
			obs_data_release(change);
			changed++;
		}
		obs_data_release(settings);
		obs_source_release(source);
	}
	return changed;
}

std::vector<HeldSpace> held_spaces(obs_source_t *scene)
{
	std::vector<HeldSpace> out;
	if (!scene)
		return out;
	{
		std::lock_guard lock(g_filters_mutex);
		for (FitEngine *each : g_filters) {
			bool resizes = true;
			if (each->parent() != scene || !obs_source_enabled(each->source()) ||
			    !each->in_free_space(&resizes))
				continue;
			HeldSpace one;
			const std::string item = each->item_name();
			// (Given another item since: nothing is known of that one.)
			if (!each->held(one) || one.name != item) {
				one = HeldSpace{};
				one.name = item;
				one.resizes = resizes;
			}
			one.recently_hidden = each->recently_hidden();
			if (!one.name.empty())
				out.push_back(std::move(one));
		}
	}
	// Only those whose item is there (inside a group too: found by its
	// name, and taken only when its id is the one worked out for).
	for (auto it = out.begin(); it != out.end();) {
		obs_sceneitem_t *item = find_item(scene, it->name);
		if (!item) {
			it = out.erase(it);
			continue;
		}
		const int64_t id = obs_sceneitem_get_id(item);
		if (it->worked && it->item != id) {
			it->worked = false;
			it->space = it->goal = Box{};
			it->in = SpaceInputs{};
		}
		it->item = id;
		obs_sceneitem_t *group = obs_sceneitem_get_group(scene_of(scene), item);
		it->shown = obs_sceneitem_visible(item) && (!group || obs_sceneitem_visible(group));
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (obs_obj_is_private(source) || is_stand_in(source) || is_morph_layer(item) ||
		    (!it->shown && (!it->recently_hidden || mode_holds_hidden(scene, item) ||
				    (group && mode_holds_hidden(scene, group))))) {
			it = out.erase(it);
			continue;
		}
		Claim claim;
		if (!it->worked && claim_of(scene, id, nullptr, claim))
			it->space = it->goal = claim.box;
		++it;
	}
	return out;
}

void fit_engine_add_test_calls()
{
	// Each fit of the scene's free space that has worked its space out and
	// whose item shows: `alike` those whose space, worked out again from
	// what `held_spaces` gives (`space_from`, `claim_from`), says the very
	// claim the fit makes now; `at_home` those it finds at home. `detail`
	// has a line for each.
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_fit_predicts(in string scene, out int fits, out int alike, out int at_home, "
		"out string detail)",
		[](void *, calldata_t *cd) {
			obs_source_t *scene = obs_get_source_by_name(calldata_string(cd, "scene"));
			long long fits = 0, alike = 0, home = 0;
			std::string detail;
			if (scene) {
				for (const HeldSpace &h : held_spaces(scene)) {
					if (!h.worked || !h.shown)
						continue;
					fits++;
					SpaceOut out;
					space_from(h.in, &out);
					const Box claim = claim_from(h.in, &out);
					Claim said;
					const bool has = claim_of(scene, h.item, nullptr, said);
					const bool same = has && std::fabs(claim.x - said.box.x) <= 0.5 &&
							  std::fabs(claim.y - said.box.y) <= 0.5 &&
							  std::fabs(claim.w - said.box.w) <= 0.5 &&
							  std::fabs(claim.h - said.box.h) <= 0.5;
					alike += same ? 1 : 0;
					home += out.at_home ? 1 : 0;
					char line[320];
					snprintf(
						line, sizeof(line),
						"%s%s: worked out %.1f, %.1f %.1f x %.1f%s, says %.1f, %.1f %.1f x %.1f",
						detail.empty() ? "" : "; ", h.name.c_str(), claim.x, claim.y, claim.w,
						claim.h, out.at_home ? " (at home)" : "", said.box.x, said.box.y,
						said.box.w, said.box.h);
					detail += line;
				}
			}
			obs_source_release(scene);
			calldata_set_int(cd, "fits", fits);
			calldata_set_int(cd, "alike", alike);
			calldata_set_int(cd, "at_home", home);
			calldata_set_string(cd, "detail", detail.c_str());
		},
		nullptr);
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

// See fit_item_refit.
bool FitEngine::refit(obs_source_t *scene, const char *item_name)
{
	std::lock_guard lock(mutex_);
	if (settings_.item != item_name || scene != parent_ || !settings_.resize)
		return false;
	refit_ = true;
	return true;
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

void FitEngine::set_look(std::string look_prefix, std::string look_list_prefix)
{
	std::lock_guard lock(mutex_);
	look_prefix_ = look_prefix.empty() ? prefix_ : std::move(look_prefix);
	look_list_prefix_ = look_prefix_ == prefix_ ? list_prefix_ : std::move(look_list_prefix);
}

void FitEngine::update(obs_data_t *settings)
{
	// What the fit writes for itself (its home, the size it keeps) is
	// always its own; how it fits (`l`, the two lists `look_lists`) may be
	// a set shared by several entries of a Scene Fit (its Same for all
	// sources).
	std::string look, look_lists;
	{
		std::lock_guard lock(mutex_);
		look = look_prefix_;
		look_lists = look_list_prefix_;
	}
	auto k = [this](const char *name) {
		return prefix_ + name;
	};
	auto l = [&look](const char *name) {
		return look + name;
	};
	auto num = [&](const char *name) {
		return obs_data_get_double(settings, k(name).c_str());
	};
	auto lnum = [&](const char *name) {
		return obs_data_get_double(settings, l(name).c_str());
	};
	FitSettings s;
	s.item = entry_.empty() ? obs_data_get_string(settings, k("item").c_str()) : entry_;
	s.box = obs_data_get_int(settings, l("box").c_str()) == 1 ? BoxKind::Custom : BoxKind::WholeScene;
	s.x = lnum("box_x");
	s.y = lnum("box_y");
	s.w = std::max(2.0, lnum("box_width"));
	s.h = std::max(2.0, lnum("box_height"));
	s.margin = std::clamp(lnum("margin"), 0.0, 2000.0);
	s.tolerance = std::clamp(lnum("tolerance"), 0.0, 200.0);
	// (A switch of its own since beta122; a share set before that
	// counts as switched on, and is written down as such.) For an entry
	// of a Scene Fit the switch is the Scene Fit's own, for all entries.
	if (entry_.empty() && !obs_data_has_user_value(settings, "skip_fill_on") &&
	    obs_data_has_user_value(settings, "skip_fill") && obs_data_get_double(settings, "skip_fill") > 0.0)
		obs_data_set_bool(settings, "skip_fill_on", true);
	s.skip_rule = obs_data_get_bool(settings, entry_.empty() ? k("skip_fill_on").c_str() : "skip_fill_on");
	s.skip_pct = obs_data_get_bool(settings, l("skip_use_pct").c_str());
	s.skip_fill = std::clamp(lnum("skip_fill") / 100.0, 0.0, 1.0);
	s.skip_px = obs_data_get_bool(settings, l("skip_use_px").c_str());
	s.px_top = std::max(0.0, lnum("skip_px_top"));
	s.px_bottom = std::max(0.0, lnum("skip_px_bottom"));
	s.px_left = std::max(0.0, lnum("skip_px_left"));
	s.px_right = std::max(0.0, lnum("skip_px_right"));
	s.resize = obs_data_get_bool(settings, l("resize").c_str());
	// 0 the middle, then top, bottom, left, right, and the four corners.
	const long long toward = std::clamp<long long>(obs_data_get_int(settings, l("toward").c_str()), 0, 8);
	s.toward_x = kTowardX[toward];
	s.toward_y = kTowardY[toward];
	s.share_scenes = pick_list_read(settings, (look_lists + "share_scenes").c_str(), false);
	// Which fits make room for which. The level is each fit's own (an
	// entry's under its prefix); the list order switch is a Fit to Space's
	// own, or the Scene Fit's for all its entries. Neither saved (a filter
	// from before they existed): sharing, as every fit did then.
	const long long level = obs_data_get_int(settings, l("space_priority").c_str());
	s.priority = level == 1 ? SpacePriority::First : level == 2 ? SpacePriority::MakesRoom : SpacePriority::Shares;
	s.by_list = obs_data_get_bool(settings, "list_priority");
	s.orig_item = obs_data_get_string(settings, k("orig_item").c_str());
	s.orig_k = num("orig_k");
	s.orig_w = num("orig_w");
	s.orig_h = num("orig_h");
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
		// (The saved size is read at every update, not only the first:
		// a rename of the source rewrites `keep_item`, and a size set
		// by hand must still come back when Resize to fit is unticked
		// after it.)
		// (On the screen since beta198, with the sizes in its source it
		// was kept at since betaNEXT; a filter saved before beta198 has the
		// scale alone, turned into a size once one can be read, `prepare`.)
		saved_keep_item_ = obs_data_get_string(settings, k("keep_item").c_str());
		saved_keep_ = KeptSize{num("keep_w"),
				       num("keep_h"),
				       num("keep_sw"),
				       num("keep_sh"),
				       num("keep_pw"),
				       num("keep_ph"),
				       0.0};
		if (!saved_keep_.sized()) {
			saved_keep_ = KeptSize{};
			saved_keep_.k = num("keep_k");
		}
		// The home as saved, taken up when OBS loads the filter or the
		// filter is given another item (a rename gives it the same one).
		if (first_update_ || s.item != settings_.item) {
			home_loaded_ = true;
			saved_home_item_ = obs_data_get_string(settings, k("home_item").c_str());
			saved_home_ = Box{num("home_x"), num("home_y"), num("home_w"), num("home_h")};
			saved_home_space_ =
				Box{num("home_space_x"), num("home_space_y"), num("home_space_w"), num("home_space_h")};
			saved_home_cut_[0] = num("home_cut_l");
			saved_home_cut_[1] = num("home_cut_t");
			saved_home_cut_[2] = num("home_cut_r");
			saved_home_cut_[3] = num("home_cut_b");
		}
		if (first_update_) {
			first_update_ = false;
			back_to_original_ = false;
			load_keep_ = !s.resize && !s.item.empty() && saved_keep_item_ == s.item;
			// (Loaded with a place it had before: until what shows of
			// it is known it holds its room, `prepare`.)
			as_is_ = !obs_data_get_bool(settings, k("have_anchor").c_str());
			// (The item it was loaded with is no other item to `prepare`.)
			load_item_ = new_item_;
		}
	}
	const long long kind = obs_data_get_int(settings, l("box").c_str());
	if (kind == 2)
		s.box = BoxKind::FreeSpace;
	// The filter's own two measuring options. Before they existed there
	// was one tick box that followed the dock; unticked, it meant boxes.
	s.visible_pixels = obs_data_get_bool(settings, l("visible_pixels").c_str());
	s.no_shadows = obs_data_get_bool(settings, l("no_shadows").c_str());
	if (prefix_.empty() && obs_data_has_user_value(settings, "as_arranged") &&
	    !obs_data_get_bool(settings, "as_arranged") && !obs_data_has_user_value(settings, "visible_pixels"))
		s.visible_pixels = false;
	s.skip = pick_list_read(settings, (look_lists + "skip").c_str(), false);
	s.have_anchor = obs_data_get_bool(settings, k("have_anchor").c_str());
	s.anchor_x = num("anchor_x");
	s.anchor_y = num("anchor_y");
	s.glide_s = std::clamp((double)obs_data_get_int(settings, l("glide_ms").c_str()), 0.0, 10000.0) / 1000.0;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, l("ease_curve").c_str()));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, l("ease_mode").c_str()));
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
		if (dirty_ || placed_ || shared_in_ || new_item_ || refit_)
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
	bool placed_now = false, shared_now = false, new_home = false, other_item = false;
	bool home_loaded = false, refit_now = false;
	std::string loaded_item;
	Box loaded_home, loaded_space;
	double loaded_cut[4] = {0.0, 0.0, 0.0, 0.0};
	{
		std::lock_guard lock(mutex_);
		dirty_ = false;
		if (std::exchange(home_loaded_, false)) {
			home_loaded = true;
			loaded_item = saved_home_item_;
			loaded_home = saved_home_;
			loaded_space = saved_home_space_;
			std::copy(saved_home_cut_, saved_home_cut_ + 4, loaded_cut);
		}
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
			other_item = true;
			// (Not when OBS loads the filter: the item it was saved
			// with is no other item, and one with a place saved holds
			// its room meanwhile, `update`.)
			if (!std::exchange(load_item_, false))
				as_is_ = true;
			unplaced_for_ = 0.0;
		}
		// The dock's Fit to space pressed on it: its space is looked for
		// where it is now, at once.
		if (refit_) {
			refit_ = false;
			refit_now = true;
			settings_.have_anchor = false;
		}
		s = settings_;
	}
	if (other_item) {
		// What was worked out was for the other item.
		std::lock_guard lock(held_mutex_);
		have_held_ = false;
	}
	// Where it stands: its filter's place in the scene's list is looked up
	// at every tick, so moving the filter up or down counts at once.
	{
		const int at = parent_ ? applied_index(parent_, source_) : -1;
		rank_ = Rank{s.by_list && at >= 0, s.priority, std::max(0, at), entry_index_};
	}
	if (home_loaded) {
		// (A home saved before the space round it was: found again.)
		has_home_ = !s.item.empty() && loaded_item == s.item && loaded_home.w >= 1.0 && loaded_home.h >= 1.0;
		home_ = loaded_home;
		home_space_ = loaded_space;
		have_home_space_ = has_home_ && loaded_space.w >= 2.0 && loaded_space.h >= 2.0;
		home_settling_ = has_home_ && !have_home_space_;
		home_settle_ = 0.0;
		// (Before it is looked at, it shows what it showed then: without
		// that, a window's empty margins counted as its home for the half
		// second until it was looked at, and the others left their homes.)
		have_loaded_cut_ = has_home_ && !own_seen_;
		std::copy(loaded_cut, loaded_cut + 4, loaded_cut_);
	}
	if (placed_now || shared_now) {
		// Placed on purpose: a home it had is where it is now.
		if (has_home_)
			home_due_ = true;
		// The room it held is not where it is now.
		have_claim_ = box_asked_ = false;
		have_keep_ = false;
		have_cur_ = have_goal_ = have_set_ = false;
		moving_ = false;
		own_seen_ = false;
		placed_by_tool_ = true;
		as_is_ = true;
		unplaced_for_ = 0.0;
	}
	if (new_home) {
		store_bool("have_anchor", true);
		store_double("anchor_x", s.anchor_x);
		store_double("anchor_y", s.anchor_y);
	}
	if (refit_now) {
		// Fitted now (the owner, 2026-11-19: "if I manually resize a source
		// then click the fit to space button, it doesn't seem to work when
		// scene fit is on ... the button doesn't seem to work while scene
		// fit is on"). Every wait it is in is dropped: a resize or a move by
		// hand being followed to its end, and a home, which would keep a size
		// made by hand within the Size tolerance of the one that fills. It
		// glides from where it is to where it fills its space, by its
		// alignment; if what shows of it is not known it is looked at now (a
		// press is one look, as for the dock's other tools).
		hand_on_ = moving_ = false;
		have_set_ = false; // where it is now is not a move to wait out
		if (has_home_) {
			has_home_ = have_home_space_ = home_settling_ = false;
			at_home_ = false;
			store_string("home_item", "");
			store_double("home_w", 0.0);
			store_double("home_h", 0.0);
			store_double("home_space_w", 0.0);
		}
		home_due_ = false;
		rebase_ = true;
		own_last_look_ = 1e9;
		if (!own_known_)
			own_seen_ = false;
		unplaced_for_ = 0.0;
		obs_log(LOG_INFO, "%s '%s': '%s' is fitted now (Fit to space was pressed on it)",
			prefix_.empty() ? "Fit to Space" : "Scene Fit", obs_source_get_name(source_), s.item.c_str());
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
	obs_sceneitem_t *group = item && obs_sceneitem_get_scene(item) != scene_of(parent_)
					 ? obs_sceneitem_get_group(scene_of(parent_), item)
					 : nullptr;
	// Switched on while its item is hidden (or has no image yet): the
	// layout made by hand is the layout of the items that show. This one
	// keeps the size and home it had, and is not taken as it is when it is
	// shown later. Up to beta198 the switch was kept waiting: when a mode
	// showed the item, maybe minutes later, it was taken as switched on
	// then ("switched on, ... keeps the place and size it has" in the log),
	// its kept size read off whatever was shown at that moment.
	auto not_taken = [&](const char *why) {
		on_as_put_ = false;
		if (!std::exchange(take_home_, false))
			return;
		obs_log(LOG_INFO, "%s '%s': switched on while '%s' %s; it keeps the size and home it had",
			prefix_.empty() ? "Fit to Space" : "Scene Fit", obs_source_get_name(source_), s.item.c_str(),
			why);
	};
	if (item && (!obs_sceneitem_visible(item) || (group && !obs_sceneitem_visible(group)))) {
		not_taken("is hidden");
		as_is_ = false; // shown later, it holds its room until it is placed
		if (!hidden_) {
			hidden_since_ = os_gettime_ns();
			box_asked_ = false;
		}
		if (holds_room(item, s))
			say(obs_sceneitem_get_id(item), last_claim_);
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
		hidden_since_ = 0;
		hidden_ = false;
		box_asked_ = false;
		back_for_ = 0.0;
		coming_back_ = have_claim_;
		still_for_ = 0.0;
	}
	if (coming_back_) {
		if ((back_for_ += seconds) < kComingBack && item) {
			say(obs_sceneitem_get_id(item), last_claim_);
			return false;
		}
		coming_back_ = false;
	}
	obs_source_t *shown = item ? obs_sceneitem_get_source(item) : nullptr;
	const double sw = shown ? obs_source_get_width(shown) : 0.0, sh = shown ? obs_source_get_height(shown) : 0.0;
	if (sw < 2.0 || sh < 2.0) {
		not_taken(item ? "has no image" : "is not in the scene");
		as_is_ = false; // it holds its room until it is placed
		// Nothing to fit yet. An item that is showing but has no
		// picture so far (a display capture before its first frame)
		// still holds its room in the free space. Holding none, it was
		// not there for the other filters, and with five captures
		// stacked in one place and a display switched to, the camera
		// beside it glided to the middle of the screen and stayed
		// there, and the display was fitted smaller in what was left.
		if (item && holds_room(item, s))
			say(obs_sceneitem_get_id(item), last_claim_);
		return false;
	}

	// What is fitted is the picture, without the room a shadow takes
	// around it and without what the item crops away. (The room is read
	// again only when a shadow's room has changed, or another source is
	// shown.)
	const uint64_t rev = shadow_room_revision();
	if (shown != room_src_ || rev != room_rev_) {
		const bool same_source = shown == room_src_;
		const double was[4] = {room_c_[0], room_c_[1], room_c_[2], room_c_[3]};
		room_src_ = shown;
		room_rev_ = rev;
		shadow_room(shown, room_c_);
		in_view_c_ = shadow_kept_in_view(shown);
		// The shadow filter has moved the item and grown its box by the room
		// it changed, at the scale the picture is drawn at, so that the
		// picture stays where it is (keep_picture_in_place): that is where
		// this filter put it, not a move or a resize by something else. (A
		// shadow sized in screen pixels changes its room whenever the item is
		// resized: up to betaNEXT that was taken for a move by hand, and an
		// item grown into a hidden neighbour's room took the grown place for
		// its home and never went back.)
		if (same_source && have_set_ && set_k_ > 0.0) {
			const double dl = room_c_[0] - was[0], dt = room_c_[1] - was[1];
			const double dr = room_c_[2] - was[2], db = room_c_[3] - was[3];
			set_x_ -= dl * set_k_;
			set_y_ -= dt * set_k_;
			set_bw_ += (dl + dr) * set_k_;
			set_bh_ += (dt + db) * set_k_;
		}
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
	const double whole_left = left, whole_top = top, whole_w = pw, whole_h = ph;
	if (s.visible_pixels) {
		own_for_ += seconds;
		// Its picture changed size (a filter was added, a window
		// resized): what was seen no longer holds, and the look waits
		// until the size has stayed put for half a second, so that it
		// is not taken while things are still settling. (The picture,
		// not the source: a shadow sized in screen pixels changes the
		// room it takes round the picture whenever the item is resized,
		// and what shows of the picture stays the same. Up to betaNEXT
		// that was forgotten too, and for half a second after every
		// resize a letterboxed window was fitted with its empty margins.)
		own_last_look_ += seconds;
		if (own_sw_ != pw || own_sh_ != ph) {
			// (Only made larger or smaller, the same shape: what shows is
			// the same part of it, so the parts seen are kept until it is
			// looked at again. A window made larger or smaller, or one a
			// window switcher glides to another size, is not fitted with
			// its empty margins meanwhile.)
			const bool scaled = own_known_ && !have_loaded_cut_ && own_sw_ > 0.0 && own_sh_ > 0.0 &&
					    std::fabs(pw / ph - own_sw_ / own_sh_) <= 0.005 * (pw / ph);
			own_sw_ = pw;
			own_sh_ = ph;
			if (!scaled) {
				own_cut_[0] = own_cut_[1] = own_cut_[2] = own_cut_[3] = 0.0;
				own_known_ = false;
				if (std::exchange(have_loaded_cut_, false)) {
					std::copy(loaded_cut_, loaded_cut_ + 4, own_cut_);
					own_known_ = true;
				}
			}
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
		// A home, the size noted before the filter first touches it and a
		// size to keep that has not been read yet need what shows of it
		// before the first transform can change it: looked at at once,
		// without the half second. But never on every frame: looking draws
		// the item and reads it back, and up to beta198 a source kept at its
		// size was looked at on each tick for as long as its size changed (a
		// window switcher's glide, a Standby Image's, a text that changes),
		// against the rule that nothing is read back while things animate.
		// So at once only when it has not been looked at in the last half
		// second, and one whose size is already kept waits like the rest.
		// (The size before the filter is noted as a scale, below, on the
		// first tick with the item; one saved as a scale by a version
		// before, still to be turned into a size, is not a reason to look.)
		const bool untouched = s.orig_item != s.item ||
				       (s.orig_k <= 0.0 && (s.orig_w <= 0.0 || s.orig_h <= 0.0));
		const bool first = take_home_ || refit_now || untouched || (!s.resize && !have_keep_);
		const bool need_image = first && !own_seen_ && own_last_look_ >= 0.5;
		const bool due = (!own_seen_ && (need_image || own_for_ >= 0.5)) || (own_changed && own_for_ >= 0.5);
		if (due && obs_source_showing(parent_) && !measuring_stopped()) {
			own_for_ = 0.0;
			own_last_look_ = 0.0;
			own_seen_ = true;
			own_known_ = true;
			count_look(Look::FitOwn);
			own_looked_have_ = glance_of(parent_, own_id, own_looked_);
			// (Measured first: that also brings the item's box up to date.)
			Box shows;
			const bool any = measure_seen(parent_, item, how, shows);
			const Box whole = scene_box(parent_, item);
			const double cw = sw - crop.left - crop.right, ch = sh - crop.top - crop.bottom;
			// (Also an item whose box hangs over the screen's edge: the
			// source is drawn by itself to be measured, so what is off the
			// screen counts as it is. Up to betaNEXT such an item was never
			// looked at, and a window letterboxed in a box larger than the
			// gap to the screen's edge was fitted with its empty margins.)
			if (any && whole.w >= 1.0 && whole.h >= 1.0 && cw >= 1.0 && ch >= 1.0) {
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
						// (At home its box is what shows of it: found again
						// now.)
						if (has_home_) {
							changed_ = true;
							store_home_cut();
						}
						// (What is fitted is another part of it: the glide
						// to wherever that goes starts from where that part
						// shows now, not from the box the part before had.)
						rebase_ = have_cur_;
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
	if (sw != steady_sw_ || sh != steady_sh_ || !obs_sceneitem_visible(item)) {
		steady_sw_ = sw;
		steady_sh_ = sh;
		steady_for_ = 0.0;
	} else {
		steady_for_ += seconds;
	}
	// Whether a size can be read off the picture now, to be kept as it
	// shows on the screen: its source has kept its size for half a second
	// (one that is still loading, or gliding to another size, says a wrong
	// one), what shows of it is known (read with a window's empty margins
	// counted, the size came out a margin too large), and it is the picture
	// itself: while a Standby Image stands in for a capture the source has
	// the image's shape, as tall as the capture last was or 1080 px, and a
	// size read then was the image's (the owner's handheld windows are under
	// a Scene Standby: an old kept scale could be turned into a size about
	// twice too large at the first start after the update). Until one can
	// be read a scale does: it gives the picture that scale, as every
	// version up to beta197 did, and nothing waits. (Whether an image
	// stands in is asked of the source's filters four times a second at the
	// most: a scale may wait for hours for a capture to come back.)
	standing_age_ += seconds;
	auto readable = [&]() {
		if (steady_for_ < 0.5 || (s.visible_pixels && !own_known_))
			return false;
		if (standing_age_ >= 0.25) {
			standing_age_ = 0.0;
			standing_ = standby_waiting(shown);
		}
		return !standing_;
	};
	// Before the filter does anything to an item, the size it has is
	// noted, so that "Resize it to fit" can be unticked later and the
	// item gets its own size back. (A new filter starts ticked: the
	// owner's chat box was fitted, and so shrunk, before the box could
	// be unticked, and then kept the shrunken size.)
	// (Noted as the scale it is drawn at, at once; that is turned into the
	// size on the screen once one can be read, as for a scale saved by a
	// version before beta198. Nothing waits for it: up to beta198 a source
	// that never kept its size for half a second was not fitted at all
	// while its old scale waited.)
	if (s.orig_item != s.item || (s.orig_k <= 0.0 && (s.orig_w <= 0.0 || s.orig_h <= 0.0))) {
		double k = 0.0;
		if (drawn_now(k)) {
			// (While a Standby Image stands in, the scale is the image's:
			// what is on the screen is noted, never converted later with
			// the capture's size.)
			const bool stand = standby_waiting(shown);
			s.orig_item = s.item;
			s.orig_k = k;
			s.orig_w = stand ? pw * k : 0.0;
			s.orig_h = stand ? ph * k : 0.0;
			store_string("orig_item", s.item.c_str());
			store_double("orig_k", s.orig_k);
			store_double("orig_w", s.orig_w);
			store_double("orig_h", s.orig_h);
			std::lock_guard lock(mutex_);
			settings_.orig_item = s.item;
			settings_.orig_k = s.orig_k;
			settings_.orig_w = s.orig_w;
			settings_.orig_h = s.orig_h;
		}
	}
	if (s.orig_item == s.item && s.orig_k > 0.0 && (s.orig_w <= 0.0 || s.orig_h <= 0.0) && readable()) {
		s.orig_w = pw * s.orig_k;
		s.orig_h = ph * s.orig_k;
		store_double("orig_w", s.orig_w);
		store_double("orig_h", s.orig_h);
		std::lock_guard lock(mutex_);
		settings_.orig_w = s.orig_w;
		settings_.orig_h = s.orig_h;
	}
	// The same for a size kept before, saved as a scale: turned into a
	// size also while Resize to fit is on, for when it is unticked.
	{
		std::lock_guard lock(mutex_);
		if (saved_keep_item_ == s.item && !saved_keep_.sized() && saved_keep_.k > 0.0 && readable()) {
			const double k = saved_keep_.k;
			saved_keep_ = KeptSize{pw * k, ph * k, whole_w, whole_h, pw, ph, 0.0};
			store_double("keep_w", saved_keep_.w);
			store_double("keep_h", saved_keep_.h);
			store_double("keep_sw", saved_keep_.sw);
			store_double("keep_sh", saved_keep_.sh);
			store_double("keep_pw", saved_keep_.pw);
			store_double("keep_ph", saved_keep_.ph);
			obs_log(LOG_INFO,
				"%s '%s': the size kept for '%s', saved as %.4f of its source, is %.1f x %.1f px on the screen",
				prefix_.empty() ? "Fit to Space" : "Scene Fit", obs_source_get_name(source_),
				s.item.c_str(), k, saved_keep_.w, saved_keep_.h);
			// (In use as a scale: the size from now on.)
			// (Only the very scale saved: one read since, by a resize by
			// hand or a tool while no size could be read, is newer.)
			if (have_keep_ && !keep_.sized() && keep_.k > 0.0 && std::fabs(keep_.k - k) <= 1e-9 * k)
				keep_ = saved_keep_;
		}
	}
	// Switched on just now: the item keeps the size it has (the layout
	// made while the filter was off is the layout), and its home is taken
	// below. (Up to betaNEXT a size kept from before, or the one from
	// before the filter first resized it, was brought back: the owner's
	// second handheld window, laid out by hand, shrank to a small box in
	// the corner the moment the Scene Fit was switched on, 2026-11-18.)
	if (take_home_) {
		take_home_ = false;
		home_due_ = true;
		hand_on_ = false;
		resized_by_hand_ = false;
		back_to_original_ = false;
		// Its place (its anchor: the point its space is looked for round,
		// and where a kept size is held for the resized sources,
		// `Claim::least`) is taken afresh where it lies, unless it lies
		// exactly where this fit last put it: then nothing was done to it
		// while the filter was off, and its place is the one it was given
		// before. A kept-size source may lie elsewhere than at its place,
		// where its Align in the space puts it in the room it is left; its
		// place taken there, the resized sources beside it would take the
		// room it left at every switch off and on, and a layout nobody
		// touched would shift each time.
		if (std::exchange(on_as_put_, false)) {
			vec2 at, box;
			obs_sceneitem_get_pos(item, &at);
			obs_sceneitem_get_bounds(item, &box);
			if (obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_SCALE_INNER &&
			    obs_sceneitem_get_rot(item) == 0.0f && std::fabs(at.x - set_x_) <= 0.5 &&
			    std::fabs(at.y - set_y_) <= 0.5 && std::fabs(box.x - set_bw_) <= 0.5 &&
			    std::fabs(box.y - set_bh_) <= 0.5) {
				s.have_anchor = true;
				std::lock_guard lock(mutex_);
				settings_.have_anchor = true;
			}
		}
		if (!s.resize) {
			// (Read off it below, as after a resize by hand.)
			have_keep_ = false;
			load_keep_ = false;
			switched_keep_ = true;
		}
		obs_log(LOG_INFO, "%s '%s': switched on, '%s' keeps the place and size it has",
			prefix_.empty() ? "Fit to Space" : "Scene Fit", obs_source_get_name(source_), s.item.c_str());
	}
	// Whether the item is being dragged just now: selected, with the mouse
	// button held (asked of Windows itself; the tests say; elsewhere it is
	// not known on this thread, and only its coming to rest tells). Nothing
	// is done to an item that is held, however long it is held still.
	const bool held = obs_sceneitem_selected(item) && mouse_held_any_thread();
	// How long the item has kept its place, for `motion`. Counted here, so
	// that it runs while a resize by hand is waited out below as well: the
	// two waits used to follow one another (0.3 s for the size, then 0.4 s
	// for the place), which was most of the "takes a while to respond" the
	// owner saw after resizing a fitted source by hand (2026-11-19).
	// (`still_from_hold_`: it was the mouse that last held or moved it, so
	// its rest counts from the button being let go.)
	{
		vec2 at;
		obs_sceneitem_get_pos(item, &at);
		if (std::fabs(at.x - seen_x_) > 0.05 || std::fabs(at.y - seen_y_) > 0.05 || held) {
			still_for_ = 0.0;
			still_from_hold_ = held;
		} else {
			still_for_ += seconds;
		}
		seen_x_ = at.x;
		seen_y_ = at.y;
	}
	// Resized by hand in the preview: that is the size to keep from now
	// on (and, for one with a home, its home). Only a person does that to
	// an item that is showing and whose source is not changing size. A
	// shadow filter also changes the bounding box, when its room does (an
	// item hidden or shown changes it): taken for a resize by hand, that
	// made the owner's camera a little smaller each time it was hidden
	// and shown again.

	// (A resized one is watched when it has a home, or while the mouse is
	// seen to hold it: without either it is fitted to its space whatever
	// its size, as it always was. Watched, it is left alone while it is
	// dragged, and glides back from where it is let go.)
	const bool hand_watched = (!s.resize && have_keep_) ||
				  (s.resize && s.box == BoxKind::FreeSpace && (has_home_ || held || hand_on_));
	if (!s.resize || hand_watched) {
		vec2 b_now;
		obs_sceneitem_get_bounds(item, &b_now);
		const bool boxed = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
		// (And only to an item that is selected: the handles
		// that resize one are on the selected item.)
		// It counts once the box has kept its new size for a moment
		// (`kHandStill`), and the item is left alone meanwhile. Read
		// on every tick, a drag in the preview and the dock's glide
		// after it (Stop resizing at edges: the size it allows for a
		// frame, back to where it was let go, then a glide there) were
		// each read once a frame: the owner's log had "the size kept
		// for 'Dolphin-ABG-Link 2' went from 0.9021 to 0.5563 ...
		// (resized in the preview)" and back within two frames, then a
		// run of small steps, the whole column worked out again each
		// time (2026-11-18). Once begun it is followed to its end
		// whether or not the item is still selected, so a click
		// elsewhere just after letting go does not undo it.
		const bool off_size = std::fabs(b_now.x - set_bw_) > 2.0 || std::fabs(b_now.y - set_bh_) > 2.0;
		if (!hand_on_ && hand_watched && boxed && have_set_ && steady_for_ >= 1.0 &&
		    obs_sceneitem_selected(item) && off_size) {
			hand_on_ = true;
			hand_bw_ = b_now.x;
			hand_bh_ = b_now.y;
			hand_still_ = 0.0;
			hand_wait_ = 0.0;
		} else if (hand_on_) {
			if (!off_size || !boxed || !hand_watched) {
				hand_on_ = false; // put back to the size kept
			} else if (std::fabs(b_now.x - hand_bw_) > 0.05 || std::fabs(b_now.y - hand_bh_) > 0.05 ||
				   steady_for_ == 0.0 || held) {
				// (Its source changing size meanwhile, a shadow's room
				// catching up say, starts the wait again: the size is
				// read only off a source that has settled. So does the
				// mouse still holding it: the wait is from letting go.)
				hand_bw_ = b_now.x;
				hand_bh_ = b_now.y;
				hand_still_ = 0.0;
			} else {
				hand_still_ += seconds;
			}
		}
		if (hand_on_) {
			// (Nor while its source has changed size in the last second:
			// a window switcher's image gliding to another size.)
			// (For `kKeepWait` at the most, counted while the mouse does
			// not hold it: a source whose size changes more often, a
			// growing text or a window switcher stepping, is not left
			// waiting for as long as that lasts.)
			hand_wait_ = held ? 0.0 : hand_wait_ + seconds;
			if (hand_still_ < kHandStill || (steady_for_ < 1.0 && hand_wait_ < kKeepWait))
				return false; // still being resized
			hand_on_ = false;
			hand_wait_ = 0.0;
			if (!s.resize) {
				have_keep_ = false;
				resized_by_hand_ = true;
			}
			if (has_home_)
				home_due_ = true;
			// From where it was let go: it glides to where it belongs.
			rebase_ = true;
		}
	}
	if (!s.resize) {
		// A size kept is a rectangle on the screen. A capture of another
		// resolution or shape never writes a new one: only a person (a
		// resize by hand, a tool, the filter switched on over a layout
		// made by hand) or the settings do.
		const char *const kind = prefix_.empty() ? "Fit to Space" : "Scene Fit";
		// Loaded, or unticked just now: the size saved (with a home, the
		// size it has there; never kept, the one from before the filter).
		if (!have_keep_ && (load_keep_ || back_to_original_) && !placed_by_tool_ && !resized_by_hand_ &&
		    !switched_keep_) {
			std::lock_guard lock(mutex_);
			if (back_to_original_ && has_home_ && home_.h > 0.0 &&
			    std::fabs(home_.w / home_.h - whole_w / whole_h) <= 0.01 * whole_w / whole_h) {
				keep_ = KeptSize{
					pw * home_.w / whole_w, ph * home_.h / whole_h, whole_w, whole_h, pw, ph, 0.0};
				have_keep_ = true;
			} else if (saved_keep_item_ == s.item && saved_keep_.known()) {
				keep_ = saved_keep_;
				have_keep_ = true;
			} else if (back_to_original_ && s.orig_item == s.item && s.orig_w > 0.0 && s.orig_h > 0.0) {
				keep_ = KeptSize{s.orig_w, s.orig_h, 0.0, 0.0, 0.0, 0.0, 0.0};
				have_keep_ = true;
			} else if (back_to_original_ && s.orig_item == s.item && s.orig_k > 0.0) {
				keep_ = KeptSize{};
				keep_.k = s.orig_k;
				have_keep_ = true;
			}
		}
		// Read off the item as it is drawn: never kept before, resized by
		// hand, placed by a tool, or switched on just now.
		if (!have_keep_) {
			// (Only from an item whose source has kept its size for half
			// a second: one that is still loading says a wrong size. Not
			// for ever: a source that changes size all the time is read
			// after `kKeepWait`, as a scale.)
			if (!resized_by_hand_ && steady_for_ < 0.5 && (keep_wait_ += seconds) < kKeepWait)
				return false;
			double k = 0.0;
			if (!drawn_now(k))
				return false;
			keep_wait_ = 0.0;
			const double dw = pw * k, dh = ph * k;
			const KeptSize was = keep_;
			// Drawn at the very size the one kept so far gives this
			// picture: it was only moved (or switched off and on), and the
			// size kept stays. Read again off a picture of another shape
			// than the one kept (a 4:3 window fitted in a 3:2 size), it
			// became that picture's, and the first one came back smaller.
			bool same = false;
			if (was.sized()) {
				const double kk = std::min(was.w / pw, was.h / ph);
				same = std::fabs(pw * kk - dw) <= 0.5 && std::fabs(ph * kk - dh) <= 0.5;
			} else if (was.k > 0.0) {
				same = std::fabs(was.k - k) <= 0.002 * k;
			}
			if (!same) {
				if (readable()) {
					keep_ = KeptSize{dw, dh, whole_w, whole_h, pw, ph, 0.0};
				} else if (standby_waiting(shown)) {
					// (A Standby Image stands in: the scale it is drawn
					// at is a scale of the image, not of the capture,
					// and turned into a size later with the capture's,
					// a 480 px window came back some 118 px wide. What
					// is on the screen is kept, as a rectangle.)
					keep_ = KeptSize{dw, dh, 0.0, 0.0, 0.0, 0.0, 0.0};
				} else {
					// (No size can be read off it yet, its source not
					// steady or what shows of it not known: the scale it
					// is drawn at, of this very source, which becomes a
					// size once one can be read.)
					keep_ = KeptSize{};
					keep_.k = k;
				}
				// For the log, so a size that changes by itself can be
				// traced: when the kept size was read, and why.
				const char *why = resized_by_hand_  ? "resized in the preview"
						  : placed_by_tool_ ? "placed by a tool"
						  : switched_keep_  ? "switched on"
								    : "settings changed";
				if (was.sized() && keep_.sized())
					obs_log(LOG_INFO,
						"%s '%s': the size kept for '%s' went from %.1f x %.1f to %.1f x %.1f px (%s)",
						kind, obs_source_get_name(source_), s.item.c_str(), was.w, was.h,
						keep_.w, keep_.h, why);
				else if (was.known())
					obs_log(LOG_INFO,
						"%s '%s': the size kept for '%s' went from %.1f x %.1f to %.1f x %.1f px (%s; %s)",
						kind, obs_source_get_name(source_), s.item.c_str(),
						was.sized() ? was.w : pw * was.k, was.sized() ? was.h : ph * was.k, dw,
						dh, why,
						keep_.sized() ? "it was kept as a scale"
							      : "kept as a scale until its own image can be read");
			}
			have_keep_ = true;
		}
		load_keep_ = back_to_original_ = resized_by_hand_ = placed_by_tool_ = switched_keep_ = false;
		// Only a scale known: the size it gives the picture, once one can
		// be read off the picture itself.
		if (!keep_.sized() && keep_.k > 0.0 && readable()) {
			const double k = keep_.k;
			keep_ = KeptSize{pw * k, ph * k, whole_w, whole_h, pw, ph, 0.0};
			obs_log(LOG_INFO,
				"%s '%s': the size kept for '%s', known as %.4f of its source, is %.1f x %.1f px", kind,
				obs_source_get_name(source_), s.item.c_str(), k, keep_.w, keep_.h);
		}
		// Saved by beta198 without the sizes in its source it was kept
		// at: those of the picture as it is, once it has the shape kept.
		if (keep_.sized() && !keep_.refs() && std::fabs(pw / ph - keep_.w / keep_.h) <= 0.01 * (pw / ph) &&
		    readable()) {
			keep_.sw = whole_w;
			keep_.sh = whole_h;
			keep_.pw = pw;
			keep_.ph = ph;
		}
		// Its source the size it was kept at, and another part of it
		// shows (a chat box with more lines in it, a window's empty margins
		// found): the scale stays, and the size kept is what shows now at
		// that scale. (In beta198 what shows was fitted into the size kept
		// whatever its source: a group that drew more of itself shrank.)
		// Not while what shows is still to be looked at.
		if (keep_.refs() && (!s.visible_pixels || own_known_) && std::fabs(whole_w - keep_.sw) <= 0.5 &&
		    std::fabs(whole_h - keep_.sh) <= 0.5 &&
		    (std::fabs(pw - keep_.pw) > 0.5 || std::fabs(ph - keep_.ph) > 0.5)) {
			const double kx = keep_.w / keep_.pw, ky = keep_.h / keep_.ph;
			keep_ = KeptSize{pw * kx, ph * ky, whole_w, whole_h, pw, ph, 0.0};
		}
		// Saved with the scene collection, for the next time OBS starts.
		bool save = false;
		if (keep_.sized()) {
			std::lock_guard lock(mutex_);
			auto differs = [](double a, double b) {
				return std::fabs(a - b) > 1e-6;
			};
			save = saved_keep_item_ != s.item || differs(saved_keep_.w, keep_.w) ||
			       differs(saved_keep_.h, keep_.h) || differs(saved_keep_.sw, keep_.sw) ||
			       differs(saved_keep_.sh, keep_.sh) || differs(saved_keep_.pw, keep_.pw) ||
			       differs(saved_keep_.ph, keep_.ph);
			if (save) {
				saved_keep_item_ = s.item;
				saved_keep_ = keep_;
			}
		}
		if (save) {
			store_string("keep_item", s.item.c_str());
			store_double("keep_w", keep_.w);
			store_double("keep_h", keep_.h);
			store_double("keep_sw", keep_.sw);
			store_double("keep_sh", keep_.sh);
			store_double("keep_pw", keep_.pw);
			store_double("keep_ph", keep_.ph);
			// (The scale too, for a version before beta198.)
			if (keep_.pw > 0.0)
				store_double("keep_k", keep_.w / keep_.pw);
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
	// The size kept is the size of what shows of it, on the screen: the
	// size kept, or as large as fits in it for a picture of another shape
	// (or, with only a scale known, at that scale).
	p.keep_w = p.keep_h = 0.0;
	if (!s.resize && have_keep_ && keep_.known()) {
		const double k = keep_.sized() ? std::min(keep_.w / pw, keep_.h / ph) : keep_.k;
		p.keep_w = pw * k;
		p.keep_h = ph * k;
	}
	p.picture = pw / ph;
	p.free = s.box == BoxKind::FreeSpace;
	p.whole_left = whole_left;
	p.whole_top = whole_top;
	p.whole_w = whole_w;
	p.whole_h = whole_h;
	if (std::exchange(home_due_, false) && p.free) {
		home_from_item(p);
		rebase_ = true; // taken where it is: any glide starts there
	}
	if (std::exchange(rebase_, false) && p.free)
		rebase(p);
	// Not placed before what shows of it is known, when it has no place
	// yet (a capture whose first image has only just come, a filter loaded
	// with no home saved, an item it was only just given): fitted with a
	// letterboxed window's empty margins, its claim reached into the gap
	// beside it for the half second until it was looked at, and a resized
	// neighbour made room for it and never took that room back (two resized
	// fits side by side are at rest at any split of the room between them).
	// Meanwhile, taken as it is (a new filter or item, switched on, placed
	// by a tool), it says nothing: the others go by what shows of it, which
	// is where it is. Otherwise (hidden until now, or with no image until
	// now) it holds the room it held without a picture, its bounding box.
	// For `kOwnWait` at the most: a source whose size never stays put for
	// half a second is never looked at, and is then placed as it is.
	if (p.free && s.visible_pixels && !have_goal_ && !own_known_ && obs_source_showing(parent_) &&
	    !measuring_stopped() && (unplaced_for_ += seconds) < kOwnWait) {
		if (!as_is_ && holds_room(item, s))
			say(obs_sceneitem_get_id(item), last_claim_);
		else
			claim_clear(this);
		return false;
	}
	unplaced_for_ = 0.0;
	as_is_ = false;
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
	// (How long it has kept its place is counted in `prepare`, `still_for_`,
	// so the time a resize by hand was waited out counts here too. Dragged
	// with the mouse, it is at rest `kHandStill` after the button is let go:
	// the dock's own correction on letting go, snap or Stop resizing at
	// edges, begins within that. Moved by anything else, a key or a filter
	// gliding it, after 0.4 s without moving, as before.)
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	const bool off_place = std::fabs(pos.x - set_x_) > 0.5 || std::fabs(pos.y - set_y_) > 0.5;
	if (have_set_ && !moving_ && off_place)
		moving_ = true;
	if (moving_) {
		if (still_for_ < (still_from_hold_ ? kHandStill : 0.4))
			return Motion::Stop;
		moving_ = false;
		s.have_anchor = false; // taken afresh below
		// Moved by hand (or by a key): where it was put is its home now.
		if (has_home_)
			home_from_item(p);
		// From where it was let go, it glides to where it belongs there.
		// (Up to beta198 it jumped: the place the fit had it at before
		// was all there was to glide from.)
		rebase(p);
	}
	// The space round a home just taken is found while the scene shows,
	// for half a second (the other fits' items say where their homes are
	// meanwhile), and then kept. The half second counts once what shows of
	// the item has been looked at and its shape counted (`solve`), and again
	// from the start whenever another fit's home changes (`observe`): a
	// filter switched on that had never run since OBS started knows nothing
	// of what shows of its items, and the space found round homes that
	// still had a window's empty margins came out a margin too small, so
	// every item left its home the moment its margins were measured.
	if (home_settling_) {
		const bool ready = (!s.visible_pixels || own_seen_) && shape_used_ > 0.0 &&
				   std::fabs(p.picture - shape_used_) <= 0.01 * p.picture;
		if (!ready) {
			home_settle_ = 0.0;
		} else if ((home_settle_ += seconds) >= kHomeSettle) {
			home_settling_ = false;
			store_home_space();
			changed_ = true;
		}
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
void FitEngine::observe(const std::vector<Obstacle> &in_way, bool arriving, float seconds, bool &at_rest,
			const std::vector<int64_t> *visible)
{
	at_rest = false;
	const bool shown_other = visible && *visible != visible_;
	if (shown_other)
		visible_ = *visible;
	std::vector<Box> boxes;
	std::vector<bool> gives;
	std::vector<bool> holds;  // what a kept-size fit holds at its home
	std::vector<int64_t> ids; // which item each is, and whether its
	std::vector<bool> said;   // filter said where it is going
	std::vector<bool> passed; // one of the items never in the way
	std::vector<bool> above;  // placed by a fit that takes its space first
	std::vector<Box> homes;   // where another fit's item lies at its home
	std::vector<bool> has_homes;
	std::vector<Box> at; // its item's box
	for (const Obstacle &o : in_way) {
		homes.push_back(o.home);
		has_homes.push_back(o.has_home);
		above.push_back(o.above);
		boxes.push_back(o.box);
		gives.push_back(o.gives);
		holds.push_back(o.holds);
		ids.push_back(o.id);
		said.push_back(o.said);
		passed.push_back(o.passed);
		at.push_back(o.at);
	}
	if (shown_other) {
		// An item shown or hidden that is not in the way changes nothing
		// worked out, which is now known to hold with it so. (One that is
		// in the way changes the obstacles, and is worked out again.)
		std::lock_guard lock(held_mutex_);
		if (have_held_ && held_in_.ids == ids)
			held_in_.visible = visible_;
	}
	auto differs = [](const Box &a, const Box &b) {
		return std::fabs(a.x - b.x) > 0.25 || std::fabs(a.y - b.y) > 0.25 || std::fabs(a.w - b.w) > 0.25 ||
		       std::fabs(a.h - b.h) > 0.25;
	};
	// What changed since the last look: `moved`, an item that is on its
	// way somewhere (gliding, being dragged, a window growing), or
	// `stepped`, a change made in one go: an item shown or hidden, or
	// another filter saying where its item is going.
	bool homes_changed = has_homes != has_homes_ || homes.size() != homes_.size();
	for (size_t i = 0; !homes_changed && i < homes.size(); i++)
		homes_changed = has_homes[i] && differs(homes[i], homes_[i]);
	// (The space round this one's home is found again, with theirs where
	// they are now: `motion`.)
	if (home_settling_ && homes_changed)
		home_settle_ = 0.0;
	bool moved = false, stepped = ids != ids_ || said != said_ || passed != passed_ || above != above_ ||
				      holds != holds_ || homes_changed;
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
		holds_ = holds;
		ids_ = ids;
		said_ = said;
		passed_ = passed;
		above_ = above;
		homes_ = homes;
		has_homes_ = has_homes;
		at_ = at;
		unsettled_ = true;
		calm_for_ = 0.0;
	} else if (stepped) {
		// Nothing to wait for: the new space is taken at once, in the
		// one glide. (Waiting here as for a moving item, two filters
		// answered each other a moment apart, and the owner's items
		// "adjust multiple times before settling".)
		boxes_ = boxes;
		gives_ = gives;
		holds_ = holds;
		ids_ = ids;
		said_ = said;
		passed_ = passed;
		above_ = above;
		homes_ = homes;
		has_homes_ = has_homes;
		at_ = at;
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
				switching_ = true;
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
		// (The switcher's change is over once the picture has the shape it
		// was going to, or has kept another for half a second.)
		if (switching_ && !with_switch_ &&
		    (std::fabs(shape - shape_used_) <= 0.01 * shape || shape_for_ >= 0.5))
			switching_ = false;
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
		// (The working out a tool asks for too, `space_from`: what is in
		// the way, the rules and the homes.)
		SpaceInputs in;
		in.within = within;
		in.ax = s.anchor_x;
		in.ay = s.anchor_y;
		in.margin = s.margin;
		in.tolerance = s.tolerance;
		in.shape = shape_used_;
		in.pw = p.pw;
		in.ph = p.ph;
		in.kept_w = kept_w;
		in.kept_h = kept_h;
		in.toward_x = s.toward_x;
		in.toward_y = s.toward_y;
		in.skip_rule = s.skip_rule;
		in.skip_pct = s.skip_pct;
		in.skip_px = s.skip_px;
		in.skip_fill = s.skip_fill;
		in.px_top = s.px_top;
		in.px_bottom = s.px_bottom;
		in.px_left = s.px_left;
		in.px_right = s.px_right;
		in.claims_firm = claims_firm_;
		in.with_switch = with_switch_;
		in.boxes = boxes_;
		in.ids = ids_;
		in.said = said_;
		in.passed = passed_;
		in.above = above_;
		in.gives = gives_;
		in.holds = holds_;
		in.homes = homes_;
		in.has_homes = has_homes_;
		in.at = at_;
		in.visible = visible_;
		in.has_home = home_box(p, in.home);
		in.takes_home = has_home_;
		in.home_settling = home_settling_;
		in.have_home_space = have_home_space_;
		in.home_space = home_space_;
		SpaceOut out;
		const Box goal = space_from(in, &out);
		avoiding_ = out.avoiding;
		// At home (see `space_from`): the layout made by hand.
		at_home_ = out.at_home;
		if (out.took_home) {
			home_space_ = out.home_space;
			have_home_space_ = out.have_home_space;
			// (What a tool is told: the space as it is now taken.)
			in.home_space = home_space_;
			in.have_home_space = have_home_space_;
		}
		{
			std::lock_guard lock(held_mutex_);
			have_held_ = true;
			held_item_ = obs_sceneitem_get_id(p.item);
			held_name_ = s.item;
			held_resizes_ = s.resize;
			held_space_ = out.room;
			held_goal_ = goal;
			held_in_ = std::move(in);
		}
		if (goal.w < 2.0 || goal.h < 2.0)
			return false;
		// The space before it is cut to the picture: what OBS is given
		// while the picture is of another shape than the cut was made
		// for (`apply`).
		const Box room = out.room;
		// (The item is elsewhere than the fit last put it, `rebase`: it
		// sets off for its goal from there, also when the goal is the one
		// it had: a source made small by hand glides back to fill. `cur_`
		// is then where what is fitted of it shows now; where it will show
		// once it is in the goal is what the fit claims, `claim_box`. The
		// two alike (a pixel): it is already there, and nothing glides,
		// whatever the space round it has become.)
		const bool rebased = std::exchange(rebased_, false) && have_cur_;
		bool there = false;
		if (rebased && p.pw > 0.0 && p.ph > 0.0) {
			const Box shows = claim_box(goal, room, within, s.margin, kept, kept_w, kept_h, s.toward_x,
						    s.toward_y, false, p.pw, p.ph);
			there = std::fabs(cur_.x - shows.x) <= 1.0 && std::fabs(cur_.y - shows.y) <= 1.0 &&
				std::fabs(cur_.w - shows.w) <= 1.0 && std::fabs(cur_.h - shows.h) <= 1.0;
		}
		const bool from_here = rebased && !there;
		if (!have_goal_ || from_here || std::fabs(goal.x - goal_.x) > 0.05 ||
		    std::fabs(goal.y - goal_.y) > 0.05 || std::fabs(goal.w - goal_.w) > 0.05 ||
		    std::fabs(goal.h - goal_.h) > 0.05) {
			// (From what OBS was last given: the space itself, while
			// the picture was changing shape, else the cut box.)
			from_ = have_cur_ ? (handed_room_ ? room_cur_ : cur_) : goal;
			room_from_ = have_cur_ ? room_cur_ : room;
			room_goal_ = room;
			goal_ = goal;
			// (With a window switcher's change: over its time, not this
			// filter's own.)
			synced_ = with_switch_;
			// (At home too: there its goal follows the space it has,
			// since Align in the space counts at home. Up to beta198
			// nothing ever glided from home to home: its home taken again
			// or what shows of it measured again, it was already there.
			// Now the glide starts from where the item is, `rebase`, and
			// one that is already there, `there`, does not glide.)
			glide_ = have_cur_ && (synced_ ? sync_s_ : s.glide_s) > 0.0 && !arriving() && !there ? 0.0
													     : 1.0;
			have_goal_ = true;
			moved = true;
		} else if (std::fabs(room.x - room_goal_.x) > 0.05 || std::fabs(room.y - room_goal_.y) > 0.05 ||
			   std::fabs(room.w - room_goal_.w) > 0.05 || std::fabs(room.h - room_goal_.h) > 0.05) {
			// Only the space round the picture changed: nothing moves
			// on screen, and it counts as no move.
			room_from_ = room_cur_;
			room_goal_ = room;
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
		room_cur_ = Box{room_from_.x + (room_goal_.x - room_from_.x) * t,
				room_from_.y + (room_goal_.y - room_from_.y) * t,
				std::max(2.0, room_from_.w + (room_goal_.w - room_from_.w) * t),
				std::max(2.0, room_from_.h + (room_goal_.h - room_from_.h) * t)};
	} else {
		cur_ = goal_;
		room_cur_ = room_goal_;
	}
	have_cur_ = true;
	return cur_;
}

// Where a kept-size picture goes in a space: where its Align in the space
// puts it, whatever lies beside it (see the note on settling above
// `kept_in_room`).
Box FitEngine::kept_in(const Prep &p, const Box &room, double kw, double kh) const
{
	return kept_in_room(room, kw, kh, p.s.toward_x, p.s.toward_y);
}

// Switched on (it was seen off): the layout is taken as it is. Nothing of
// where it was going or what it held before counts any more, the place it
// has is its anchor (with no wait), the size it has the size kept, and both
// together its home (`prepare`, `home_from_item`). (Its anchor is kept after
// all when the item still lies exactly where this fit last put it:
// `on_as_put_`, asked in `prepare`, which has the item.)
void FitEngine::switched_on()
{
	std::lock_guard lock(mutex_);
	on_as_put_ = have_set_ && settings_.have_anchor;
	take_home_ = true;
	as_is_ = true; // the layout as it is: it says nothing until placed
	unplaced_for_ = 0.0;
	have_cur_ = have_goal_ = have_set_ = false;
	moving_ = hand_on_ = coming_back_ = switching_ = false;
	have_claim_ = box_asked_ = false;
	glide_ = 1.0;
	at_home_ = false;
	dirty_ = true;
	settings_.have_anchor = false;
}

bool FitEngine::home_box(const Prep &p, Box &out) const
{
	if (!has_home_ || !p.free || p.whole_w < 1.0 || p.whole_h < 1.0 || home_.w < 1.0 || home_.h < 1.0)
		return false;
	// (Of another shape: the picture is not the one laid out, and is fitted.
	// While a window switcher changes to another window, the shape it is
	// changing to: from the first tick of the change the new window is not at
	// home, and a change back to the window laid out is at home at once. Gone
	// by the shape the picture has, the old window passed for the new one at
	// the start of a change away, and the new window was squeezed into the
	// old one's box until something else moved.)
	double shape = p.whole_w / p.whole_h;
	if (switching_ && shape_used_ > 0.0 && p.picture > 0.0)
		shape *= shape_used_ / p.picture;
	if (std::fabs(home_.w / home_.h - shape) > 0.01 * shape)
		return false;
	// What shows of it, cut from the whole picture as it is cut now.
	const double c0 = p.s.visible_pixels ? own_cut_[0] : 0.0, c1 = p.s.visible_pixels ? own_cut_[1] : 0.0;
	const double c2 = p.s.visible_pixels ? own_cut_[2] : 0.0, c3 = p.s.visible_pixels ? own_cut_[3] : 0.0;
	out = Box{home_.x + c0 * home_.w, home_.y + c1 * home_.h, home_.w * (1.0 - c0 - c2), home_.h * (1.0 - c1 - c3)};
	// (Kept at another size than at its home: not at home.)
	if (!p.s.resize && (!have_keep_ || std::fabs(p.keep_w - out.w) > 0.5 || std::fabs(p.keep_h - out.h) > 0.5))
		return false;
	return out.w >= 1.0 && out.h >= 1.0;
}

bool FitEngine::part_on_screen(const Prep &p, double left, double top, double w, double h, Box &out) const
{
	obs_sceneitem_t *item = p.item;
	if (!item || obs_sceneitem_get_rot(item) != 0.0f || w < 1.0 || h < 1.0)
		return false;
	refresh_item(item);
	// Where it is drawn: by OBS's own drawing transform, from the cropped
	// source to the scene (and through its group). (Worked out from the
	// bounding box and its alignment, it was up to a pixel off: OBS places a
	// picture inside its bounding box by whole pixels, so the item moved by
	// that much the moment its box became the picture's own, the first time
	// the filter set it.)
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	const Space space = item_space(parent_, item);
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	for (int i = 0; i < 4; i++) {
		vec3 corner, at;
		vec3_set(&corner, (float)(left - p.crop.left + ((i & 1) ? w : 0.0)),
			 (float)(top - p.crop.top + ((i & 2) ? h : 0.0)), 0.0f);
		vec3_transform(&at, &corner, &draw);
		const double x = space.ox + at.x * space.sx, y = space.oy + at.y * space.sy;
		x0 = std::min(x0, x);
		x1 = std::max(x1, x);
		y0 = std::min(y0, y);
		y1 = std::max(y1, y);
	}
	out = Box{x0, y0, x1 - x0, y1 - y0};
	return out.w >= 1.0 && out.h >= 1.0;
}

// The space that puts the item exactly where it is now is the box what is
// fitted of it has on the screen: taken for where the fit has it, so the
// next glide starts there.
void FitEngine::rebase(const Prep &p)
{
	Box now;
	// (Only in a scene that shows: out of view nothing glides, and an item
	// placed there is left exactly as it was put until its scene comes on.)
	if (!p.free || !obs_source_showing(parent_) || !part_on_screen(p, p.left, p.top, p.pw, p.ph, now)) {
		have_cur_ = false;
		return;
	}
	cur_ = room_cur_ = now;
	handed_room_ = false;
	have_cur_ = true;
	glide_ = 1.0;
	rebased_ = true;
	changed_ = true;
}

bool FitEngine::home_from_item(const Prep &p)
{
	has_home_ = false;
	Box at;
	if (part_on_screen(p, p.whole_left, p.whole_top, p.whole_w, p.whole_h, at)) {
		const double kk = at.w / p.whole_w, ky = at.h / p.whole_h;
		// (Stretched one way more than the other: no home, it is fitted.)
		if (std::fabs(kk - ky) <= 0.005 * kk) {
			home_ = at;
			has_home_ = true;
		}
	}
	have_home_space_ = false;
	home_settling_ = has_home_;
	home_settle_ = 0.0;
	changed_ = true;
	store_string("home_item", has_home_ ? p.s.item.c_str() : "");
	store_double("home_x", home_.x);
	store_double("home_y", home_.y);
	store_double("home_w", has_home_ ? home_.w : 0.0);
	store_double("home_h", has_home_ ? home_.h : 0.0);
	store_double("home_space_w", 0.0);
	store_home_cut();
	return has_home_;
}

void FitEngine::store_home_cut()
{
	store_double("home_cut_l", own_cut_[0]);
	store_double("home_cut_t", own_cut_[1]);
	store_double("home_cut_r", own_cut_[2]);
	store_double("home_cut_b", own_cut_[3]);
}

void FitEngine::store_home_space()
{
	store_double("home_space_x", home_space_.x);
	store_double("home_space_y", home_space_.y);
	store_double("home_space_w", have_home_space_ ? home_space_.w : 0.0);
	store_double("home_space_h", have_home_space_ ? home_space_.h : 0.0);
}

void FitEngine::say(int64_t item, Claim claim)
{
	claim.rank = rank_;
	claim_set(parent_, item, this, claim);
}

// What will show of it once it is where it is going, for any other Fit
// to Space in this scene to go by.
// A resized picture whose shape is not the one its space was cut to
// (`trim_to_picture_in`), with no window switcher's change under way. At home,
// only one that no longer fits its home (a window dragged to another shape):
// a home is cut to what shows of the picture as it is cut now, and what shows
// of it being measured anew changes the shape fitted, not the picture.
bool FitEngine::reshaping(const Prep &p) const
{
	Box at;
	return p.free && have_goal_ && !(!p.s.resize && have_keep_) && !with_switch_ && shape_used_ > 0.0 &&
	       room_goal_.w >= 2.0 && room_goal_.h >= 2.0 && room_cur_.w >= 2.0 && room_cur_.h >= 2.0 &&
	       std::fabs(p.picture - shape_used_) > 0.01 * p.picture && (!at_home_ || !home_box(p, at));
}

void FitEngine::publish_claim(const Prep &p)
{
	const FitSettings &s = p.s;
	const Box outer{p.bx, p.by, p.bw, p.bh};
	if (p.free && have_goal_) {
		Claim claim;
		claim.resizes = s.resize;
		// (The same working out a tool asks for, `claim_from`. Kept at its
		// size, it lies in its space by Align in the space, at home too, as
		// `apply` puts it; resized, while the picture changes shape it is
		// drawn in the space before the cut, `apply`: that is where it shows.)
		const bool kept = !s.resize && have_keep_;
		claim.box = claim_box(goal_, room_goal_, outer, s.margin, kept, p.keep_w, p.keep_h, s.toward_x,
				      s.toward_y, !kept && reshaping(p), p.pw, p.ph);
		// What it holds whatever the other fits do: for one keeping its
		// size, that size at its home, on the screen. The fits that resize
		// their items go by this, and so does any fit it makes room for
		// (see the note on settling above `kept_in_room`): never by
		// `claim.box`, which is where it lies in the room they leave it.
		// (One that resizes holds nothing: it makes room by shrinking, so
		// it is never covered. Its home as a point was tried: it changed
		// the shape of the higher fits' spaces, and two of them sharing one
		// column then crept for good, half a pixel a tick.)
		// Its home is its anchor, the middle of its item's box when its
		// place was taken. What is fitted of it need not lie in the middle
		// of that box (a shadow's room on one side, a group that draws to
		// one side of itself): where it lay in the box at this size is
		// worked out from what is not fitted on each side now, so that a
		// layout made by hand with the gaps is the layout the resized
		// sources go by, to the pixel.
		if (s.have_anchor && !s.resize && have_keep_ && p.pw > 0.0 && p.ph > 0.0) {
			claim.has_least = true;
			const double gap = std::min({s.margin, outer.w / 2.0 - 1.0, outer.h / 2.0 - 1.0});
			auto on = [](double centre, double size, double from, double room) {
				return room >= size ? std::clamp(centre - size / 2.0, from, from + room - size)
						    : from + (room - size) / 2.0;
			};
			const double kx = p.keep_w / p.pw, ky = p.keep_h / p.ph;
			const double off_x = ((p.left - p.crop.left) - (p.right - p.crop.right)) / 2.0 * kx;
			const double off_y = ((p.top - p.crop.top) - (p.bottom - p.crop.bottom)) / 2.0 * ky;
			claim.least = Box{on(s.anchor_x + off_x, p.keep_w, outer.x + gap, outer.w - 2.0 * gap),
					  on(s.anchor_y + off_y, p.keep_h, outer.y + gap, outer.h - 2.0 * gap),
					  p.keep_w, p.keep_h};
		}
		claim.has_home = home_box(p, claim.home);
		say(obs_sceneitem_get_id(p.item), claim);
		last_claim_ = claim;
		have_claim_ = true;
	} else {
		claim_clear(this);
		have_claim_ = false;
		std::lock_guard lock(held_mutex_);
		have_held_ = false;
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
	// A resized picture whose shape is not the one its space was cut to
	// (`trim_to_picture_in`: a window dragged wider, a game changing
	// resolution, an Auto Frame settling) is given the whole space it
	// was cut from, so OBS fits it there on every frame from the first,
	// as before the cut: in the cut box it could only shrink, for the
	// half second the new shape is waited for and the glide after.
	handed_room_ = reshaping(p);
	if (handed_room_)
		space = room_cur_;
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
		// (At home too: it lies in its space by Align in the space, which
		// keeps the gap. Up to beta198 it was left exactly where it was put
		// by hand there, a pixel or two inside the gap or not.)
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
	set_k_ = k;
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
	if (!obs_source_enabled(source_))
		was_off_ = true;
	if (!parent_ || !obs_source_enabled(source_)) {
		claim_clear(this);
		return;
	}
	if (std::exchange(was_off_, false))
		switched_on();
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
			scan.obstacles(parent_, p.item, p.s.skip, p.s.skip_rule, this, p.how, in_way, &rank_,
				       p.s.resize);
			bool at_rest = false;
			observe(in_way, arriving(), seconds, at_rest, &scan.visible_ids());
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
	static const auto excluded = [](obs_sceneitem_t *item) {
		obs_source_t *source = obs_sceneitem_get_source(item);
		return !obs_sceneitem_visible(item) || !source || obs_obj_is_private(source) || is_stand_in(source) ||
		       is_morph_layer(item);
	};
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *other, void *param) {
		auto *g = static_cast<Gather *>(param);
		if (other == g->self || obs_sceneitem_is_group(other) || excluded(other))
			return true;
		g->out->push_back(Gathered{other, g->top ? g->top : other, false});
		return true;
	};
	obs_scene_enum_items(
		obs_scene_from_source(scene),
		[](obs_scene_t *sc, obs_sceneitem_t *other, void *param) {
			auto *g = static_cast<Gather *>(param);
			if (other == g->self || excluded(other))
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

void FitScan::look(obs_source_t *scene, obs_sceneitem_t *other, const Measure &how, bool by_drawing)
{
	Seen seen;
	obs_source_t *source = obs_sceneitem_get_source(other);
	seen.width = obs_source_get_width(source);
	seen.height = obs_source_get_height(source);
	// By the fit's own way of measuring: by boxes, nothing is drawn or
	// read back. Only a dock press looks at what an item draws whatever its
	// measuring says: one that draws nothing is not in its way. (In beta198
	// every fit did: one that goes by boxes drew and read back each thing
	// in its way, and left out the ones that showed nothing.)
	const Measure with = by_drawing ? Measure{true, how.leave_out_shadows} : how;
	if (with.visible_pixels && seen.width && seen.height)
		count_look(Look::FitObstacle);
	seen.there = seen.width && seen.height && measure_seen(scene, other, with, seen.box);
	seen.item_at = scene_box(scene, other);
	if (!how.visible_pixels)
		seen.box = seen.item_at;
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
			const Rank *mine, bool mine_resizes)
{
	out.clear();
	visible_.clear();
	empty_.clear();
	if (look_again_) {
		look_again_ = false;
		seen_.clear();
	}
	// A fit of its own (`mine` given) or a dock press.
	const bool automatic = mine != nullptr;
	for (Gathered &g : gathered_) {
		if (g.item == self || g.top == self)
			continue;
		const int64_t id = obs_sceneitem_get_id(g.item);
		visible_.push_back(id);
		Claim claim;
		const bool claimed = claim_of(scene, id, asking, claim);
		// Placed by a fit that makes room for this one: in its way only by
		// what that fit holds whatever this one does (`Claim::least`): for
		// one keeping its size, that size at its home. Never by where it is
		// going, which follows this fit's claim: the dependence stays
		// one-way and the two cannot chase each other. (Up to betaNEXT not
		// in the way at all: the owner's game, set to take its space first,
		// covered the camera and the chat box beside it. The owner: "they
		// aren't supposed to ever cross borders of other sources unless
		// specifically allowed with the overlap setting", 2026-11-18.)
		const bool lower = claimed && mine && outranks(*mine, claim.rank);
		if (lower && !claim.has_least)
			continue;
		// Placed by a fit of the same standing that keeps its size, and the
		// asking fit resizes its item: in its way by what that fit holds too
		// (its size at its home), never by where it lies in its space by its
		// Align in the space. A resized source fills the room round the
		// kept-size sources' homes; a kept-size source then lies by its
		// alignment in the room that leaves it, and moves no resized source
		// when it does: one-way again, and nothing to chase (see the note on
		// settling above `kept_in_room`).
		const bool held = claimed && !lower && mine && mine_resizes && !claim.resizes && claim.has_least &&
				  !outranks(claim.rank, *mine);
		auto holding = [&](bool passed) {
			Obstacle o{claim.least, id, true, false, passed, false};
			o.holds = true;
			return o;
		};
		bool passed = false;
		if (!g.claimed) {
			const char *name = obs_source_get_name(obs_sceneitem_get_source(g.item));
			if (name && std::find(skip.begin(), skip.end(), name) != skip.end()) {
				if (!keep_skipped)
					continue;
				passed = true;
			}
		}
		if (std::find(used_.begin(), used_.end(), g.item) == used_.end())
			used_.push_back(g.item);
		// For a fit, what another fit places is where that fit is taking
		// it, whatever it draws just now and without a look at it: a
		// capture that shows but has no image yet holds its room (the rule
		// since beta147), and so does a widget with nothing to show for
		// the moment; its own fit looks after it. (In beta198 each was
		// drawn and read back first, and one that drew nothing was left
		// out unless its source had no size at all: an empty widget's room
		// was taken by the sources beside it, which jumped back when it
		// drew again.) A dock press goes by what is drawing, below.
		if (automatic && claimed) {
			if (lower || held) {
				out.push_back(holding(passed));
				continue;
			}
			const bool above = outranks(claim.rank, *mine);
			out.push_back(Obstacle{claim.box, id, true, claim.resizes && !above, passed, above});
			out.back().home = claim.home;
			out.back().has_home = claim.has_home;
			continue;
		}
		// Looked at (drawn and read back) when first met, and again only
		// when its source comes to have a size or to have none (a capture
		// that begins or ends). Never because its size changed: a window
		// switcher's glide, a Standby Image's, an Auto Frame settling and
		// a text that changes each change the size on every frame, and
		// what shows of an item keeps its place in its box meanwhile (a
		// glance says when it does not, `at_rest`). In beta198 any change
		// of size was a look, on every tick for as long as it lasted. The
		// size is asked once a tick (it goes through the source's filters),
		// not once for each fit and pass.
		if (g.sized < 0) {
			obs_source_t *source = obs_sceneitem_get_source(g.item);
			g.sized = obs_source_get_width(source) && obs_source_get_height(source) ? 1 : 0;
		}
		auto found = seen_.find(id);
		if (found == seen_.end() || (g.sized == 1) != (found->second.width && found->second.height)) {
			look(scene, g.item, how, !automatic);
			found = seen_.find(id);
		}
		const Seen &seen = found->second;
		if (!seen.there) {
			empty_.push_back(id);
			continue;
		}
		if (claimed) {
			// (Only a dock press gets here: a fit of its own took every
			// claimed item above, ranks and all. To a press, another
			// fit's item is where that fit is taking it.)
			out.push_back(Obstacle{claim.box, id, true, claim.resizes, passed, false});
			out.back().home = claim.home;
			out.back().has_home = claim.has_home;
			continue;
		}

		const Box now = scene_box(scene, g.item);
		if (seen.item_at.w < 1.0 || seen.item_at.h < 1.0 || now.w < 1.0 || now.h < 1.0) {
			out.push_back(Obstacle{seen.box, id, false, false, passed});
			out.back().at = seen.item_at;
			continue;
		}
		const double kx = now.w / seen.item_at.w, ky = now.h / seen.item_at.h;
		out.push_back(
			Obstacle{Box{now.x + (seen.box.x - seen.item_at.x) * kx,
				     now.y + (seen.box.y - seen.item_at.y) * ky, seen.box.w * kx, seen.box.h * ky},
				 id, false, false, passed});
		out.back().at = now;
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
		// (What another fit places is gone by where that fit takes it,
		// never by a look: its own fit looks after it.)
		if (claim_of(scene, id, asking, claim) || !glance_of(scene, id, glance))
			continue;
		// (Its first glance since it was looked at is no measure
		// of that look: the item may have changed in between,
		// seconds apart as they can be. It is looked at once
		// more, and the two go together from then on.)
		const auto was = looked_.find(id);
		if (was == looked_.end() || glance_differs(glance, was->second)) {
			look(scene, other, how, false);
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

bool engines_refit(obs_source_t *scene, const char *item_name, std::string *filter)
{
	bool any = false;
	std::lock_guard lock(g_filters_mutex);
	for (FitEngine *each : g_filters) {
		if (!obs_source_enabled(each->source()) || !each->refit(scene, item_name))
			continue;
		if (!any && filter) {
			const char *name = obs_source_get_name(each->source());
			*filter = name ? name : "";
		}
		any = true;
	}
	return any;
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
	for (const char *name :
	     {"glide_ms", "ease_curve", "ease_mode", "visible_pixels", "tolerance", "space_priority"})
		shown(get(name), kind == 2);
	// The level is not read while the list order decides (the Scene Fit's
	// own switch, for an entry).
	if (!entry)
		shown(get("list_priority"), kind == 2);
	if (obs_property_t *level = get("space_priority"))
		obs_property_set_enabled(level, !obs_data_get_bool(settings, "list_priority"));
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
	set_tip(box, obs_module_text("Fit.Box.Tip"));
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
	set_tip(margin, obs_module_text("Fit.Margin.Tip"));
	obs_property_t *resize = obs_properties_add_bool(props, key("resize").c_str(), obs_module_text("Fit.Resize"));
	set_tip(resize, obs_module_text("Fit.Resize.Tip"));
	// Which fits make room for which: saved by number, new ones at the end.
	obs_property_t *level = obs_properties_add_list(props, key("space_priority").c_str(),
							obs_module_text("Fit.Priority"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(level, obs_module_text("Fit.Priority.Shares"), (int)SpacePriority::Shares);
	obs_property_list_add_int(level, obs_module_text("Fit.Priority.First"), (int)SpacePriority::First);
	obs_property_list_add_int(level, obs_module_text("Fit.Priority.MakesRoom"), (int)SpacePriority::MakesRoom);
	set_tip(level, obs_module_text("Fit.Priority.Tip"));
	if (!entry) {
		obs_property_t *by_list =
			obs_properties_add_bool(props, "list_priority", obs_module_text("Fit.ListPriority"));
		set_tip(by_list, obs_module_text("Fit.ListPriority.Tip"));
		obs_property_set_modified_callback2(by_list, box_modified, (void *)keys);
	}
	obs_property_t *toward = obs_properties_add_list(props, key("toward").c_str(), obs_module_text("Fit.Toward"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// Saved by number: new ones go at the end. Listed as OBS's Edit
	// Transform lays out a bounding box's alignment, row by row from the
	// top left, each with its arrow (`kTowardGrid`). OBS's filter window has
	// no control a plugin can draw itself, so a grid of nine buttons cannot
	// be put there; nine tick boxes would be nine rows. (The dock has the
	// real grid.)
	static const char *const kTowardNames[9] = {"Fit.Toward.Middle",     "Fit.Toward.Top",
						    "Fit.Toward.Bottom",     "Fit.Toward.Left",
						    "Fit.Toward.Right",      "Fit.Toward.TopLeft",
						    "Fit.Toward.TopRight",   "Fit.Toward.BottomLeft",
						    "Fit.Toward.BottomRight"};
	for (int cell = 0; cell < 9; cell++)
		obs_property_list_add_int(toward, obs_module_text(kTowardNames[kTowardGrid[cell]]), kTowardGrid[cell]);
	set_tip(toward, obs_module_text("Fit.Toward.Tip"));
	obs_property_t *tolerance = obs_properties_add_float_slider(props, key("tolerance").c_str(),
								    obs_module_text("Fit.Tolerance"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(tolerance, " px");
	set_tip(tolerance, obs_module_text("Fit.Tolerance.Tip"));
	obs_property_t *visible_pixels =
		obs_properties_add_bool(props, key("visible_pixels").c_str(), obs_module_text("Measure.Visible"));
	set_tip(visible_pixels, obs_module_text("Fit.Visible.Tip"));
	obs_property_t *no_shadows =
		obs_properties_add_bool(props, key("no_shadows").c_str(), obs_module_text("Measure.NoShadows"));
	set_tip(no_shadows, obs_module_text("Fit.NoShadows.Tip"));
	obs_property_t *glide =
		obs_properties_add_int(props, key("glide_ms").c_str(), obs_module_text("Fit.Glide"), 0, 10000, 50);
	obs_property_int_set_suffix(glide, " ms");
	set_tip(glide, obs_module_text("Fit.Glide.Tip"));
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
		set_tip(skip_fill_on, obs_module_text("Fit.SkipFillOn.Tip"));
		obs_property_set_modified_callback2(skip_fill_on, box_modified, (void *)keys);
	}
	obs_property_t *use_pct =
		obs_properties_add_bool(props, key("skip_use_pct").c_str(), obs_module_text("Fit.SkipUsePct"));
	set_tip(use_pct, obs_module_text("Fit.SkipUsePct.Tip"));
	obs_property_set_modified_callback2(use_pct, box_modified, (void *)keys);
	obs_property_t *skip_fill = obs_properties_add_float_slider(props, key("skip_fill").c_str(),
								    obs_module_text("Fit.SkipFill"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(skip_fill, "%");
	set_tip(skip_fill, obs_module_text("Fit.SkipFill.Tip"));
	obs_property_t *use_px =
		obs_properties_add_bool(props, key("skip_use_px").c_str(), obs_module_text("Fit.SkipUsePx"));
	set_tip(use_px, obs_module_text("Fit.SkipUsePx.Tip"));
	obs_property_set_modified_callback2(use_px, box_modified, (void *)keys);
	static const char *const kEdges[][2] = {{"skip_px_top", "Fit.SkipPxTop"},
						{"skip_px_bottom", "Fit.SkipPxBottom"},
						{"skip_px_left", "Fit.SkipPxLeft"},
						{"skip_px_right", "Fit.SkipPxRight"}};
	for (const auto &edge : kEdges) {
		obs_property_t *px = obs_properties_add_float_slider(props, key(edge[0]).c_str(),
								     obs_module_text(edge[1]), 0.0, 2000.0, 1.0);
		obs_property_float_set_suffix(px, " px");
		set_tip(px, obs_module_text("Fit.SkipPx.Tip"));
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
	obs_data_set_default_int(settings, key("space_priority").c_str(), (int)SpacePriority::Shares);
	obs_data_set_default_bool(settings, key("visible_pixels").c_str(), true);
	obs_data_set_default_bool(settings, key("no_shadows").c_str(), true);
	obs_data_set_default_int(settings, key("glide_ms").c_str(), 300);
	obs_data_set_default_int(settings, key("ease_curve").c_str(), (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, key("ease_mode").c_str(), (int)EaseMode::InOut);
}

} // namespace stagekit::fit
