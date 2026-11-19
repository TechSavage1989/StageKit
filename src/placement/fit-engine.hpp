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
#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {
// The revision of every shadow's room: raised whenever a Shadow, Glow &
// Outline filter changes the room it takes, so a fit reads the room again
// only then (kept in shadow-backdrop.cpp).
uint64_t shadow_room_revision();
} // namespace stagekit

namespace stagekit::fit {

// Saved in settings: append only.
enum class BoxKind { WholeScene = 0, Custom = 1, FreeSpace = 2 };
// Saved in settings (`space_priority`): append only.
enum class SpacePriority { Shares = 0, First = 1, MakesRoom = 2 };

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
	double orig_k = 0; // legacy collection, converted when the source has an image
	double orig_w = 0, orig_h = 0; // original image size in scene pixels
	bool visible_pixels = true;    // other items are measured by what they draw, not their boxes
	bool no_shadows = true;        // and shadows, glows and outlines do not count, its own included
	std::vector<std::string> skip; // items that are never in the way
	// ... unless a rule says the picture leaves too much of its space empty:
	bool skip_rule = false; // the switch for both rules
	bool skip_pct = false;  // by the share of its space it would fill
	double skip_fill = 0.0; // (that share)
	bool skip_px = false;   // by the empty room it would leave at each edge of its space
	double px_top = 0.0, px_bottom = 0.0, px_left = 0.0, px_right = 0.0;
	// Which fits of the scene make room for which (see `Rank`).
	SpacePriority priority = SpacePriority::Shares;
	bool by_list = false;     // "Priority by list order": the level is not read
	bool have_anchor = false; // the free space is the one round this point
	double anchor_x = 0, anchor_y = 0;
	double glide_s = 0.3;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

// Where a fit stands in its scene. `by_list` ("Priority by list order"):
// it goes by its filter's place in the scene's filter list (0 the top) and,
// for an entry of a Scene Fit, its place in that list; else by its level.
struct Rank {
	bool by_list = false;
	SpacePriority level = SpacePriority::Shares;
	int filter = 0;
	int entry = 0;
};
// Whether `a` takes its space before `b`: `b` makes room for it and `a`
// does not see `b` at all. Two going by list order: the one higher in the
// list. Otherwise by level (one going by list order counts as sharing):
// taking space first, then sharing, then making room. The same level:
// neither, and the two go by each other's claims as before.
inline bool outranks(const Rank &a, const Rank &b)
{
	if (a.by_list && b.by_list)
		return a.filter < b.filter || (a.filter == b.filter && a.entry < b.entry);
	auto tier = [](const Rank &r) {
		const SpacePriority level = r.by_list ? SpacePriority::Shares : r.level;
		return level == SpacePriority::First ? 0 : level == SpacePriority::Shares ? 1 : 2;
	};
	return tier(a) < tier(b);
}

// What a fit will show of its item once it has got there, for the other
// fits of the scene to go by, and where that fit stands.
struct Claim {
	Box box;
	bool resizes = true;
	// For one that resizes: whether more room across (`grows_x`) or
	// down (`grows_y`) would make its picture larger. A picture held by
	// its width (empty room above and below it in its space) has no use
	// for more height, and the other way round.
	bool grows_x = true, grows_y = true;
	// For one that keeps its size: it lies at the top of its space
	// (`leans_y`) because a resized picture above or below it has no use
	// for more height (see `lean_axes`). `leans_x` is never set: side by
	// side, placing is as it was.
	bool leans_x = false, leans_y = false;
	// What it holds whatever the fits it makes room for do (`has_least`):
	// for one keeping its size, that size at its home (its anchor), on the
	// screen (one that resizes holds nothing: it shrinks to make room). A
	// fit that takes its space before this one goes by that, never by
	// `box`: `box` follows that fit's own claim, `least` does not, so the
	// dependence stays one-way (see FitScan::obstacles).
	Box least;
	bool has_least = false;
	Rank rank;
	// Where its picture lies in the layout as it was made by hand
	// (`has_home`, see FitEngine::switched_on): the others look for their
	// own home space with it there.
	Box home;
	bool has_home = false;
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

// `firm`, when given, marks the boxes of `others` placed by a fit that takes
// its space before this one: those count even when the point is inside them
// (the point is then moved just off them, for this answer only).
Box free_space(const Box &within, double ax, double ay, const std::vector<Box> &others, double gap, double shape,
	       double tolerance, const std::vector<bool> *firm = nullptr);

// A home's space found again within this of the one taken, and its gap kept
// within this: what measuring on a small copy can be off by (px).
constexpr double kHomeSlack = 3.0;

// What a fit's goal is worked out from: the screen (or its box), the point
// it is looked for round, the rules, what is in the way (what shows of each
// item, or where another fit is taking it; with `said`, `passed` and
// `above` as in `Obstacle`), and its home. `space_from` is the very working
// out a fit does each time something changes (`FitEngine::solve`), so a
// tool can ask what a fit would do were something else in its way (the
// dock's Fit to space: it must never make another fit give up room).
struct SpaceInputs {
	Box within;
	double ax = 0.0, ay = 0.0;
	double margin = 0.0, tolerance = 8.0;
	double shape = 1.0;                // the shape its space is found for, width over height
	double pw = 0.0, ph = 0.0;         // the size of what is fitted (its shape now)
	double kept_w = 0.0, kept_h = 0.0; // the size kept, with Resize to fit off
	int toward_x = 0, toward_y = 0;    // Align in the space
	bool skip_rule = false, skip_pct = false, skip_px = false;
	double skip_fill = 0.0, px_top = 0.0, px_bottom = 0.0, px_left = 0.0, px_right = 0.0;
	bool claims_firm = false; // a Scene Fit with All sources
	bool with_switch = false; // a window switcher's change is under way
	std::vector<Box> boxes;
	std::vector<int64_t> ids;
	std::vector<bool> said, passed, above;
	// Whether another fit resizes each to make room (`Obstacle::gives`),
	// which way it would grow (1 across, 2 down) and which way a kept-size
	// one leans (`Obstacle::leans`).
	std::vector<bool> gives;
	std::vector<int> grows, leans;
	// Each other fit's item at its home (`Obstacle::home`).
	std::vector<Box> homes;
	std::vector<bool> has_homes;
	// Each one's item's box in the scene when it was looked at (what shows
	// of it is that part of its box).
	std::vector<Box> at;
	// Every visible item of the scene then, in the way or not: an item not
	// among them was hidden, or not there, when this was worked out.
	std::vector<int64_t> visible;
	// Its own home (the layout made by hand): where its picture lies there
	// as it shows now (`FitEngine::home_box`), the space found round it with
	// the other fits' items at their homes, and whether that space is still
	// being found (just taken: it is at home whatever the space).
	bool has_home = false;
	Box home;
	bool home_settling = false, have_home_space = false;
	Box home_space;
	// Whether it has a home at all, matching or not: placed on purpose, it
	// takes its home where it is put (`FitEngine::prepare`), which then
	// matches. (`has_home` is false while the home does not match the item
	// now: a window of another shape, another size kept.)
	bool takes_home = false;
};
// What a fit does from them.
struct SpaceOut {
	Box goal;               // where it takes its picture (w or h under 2: no room at all)
	Box room;               // the space before it is cut to the picture
	bool avoiding = false;  // the items never in the way counted after all
	bool at_home = false;   // its goal is its home
	bool took_home = false; // its home's space was still being found: `home_space` is it now
	Box home_space;
	bool have_home_space = false;
	bool grows_set = false; // for a resized picture: whether more width or height would grow it
	bool grows_x = true, grows_y = true;
};
// The goal (`out`: the rest).
Box space_from(const SpaceInputs &in, SpaceOut *out = nullptr);
// What a fit says it will show of its item once it is in `goal` (its claim,
// `FitEngine::publish_claim`): `kept`, its kept size against the side it is
// kept towards (or away from a side that gives way, or packed towards the
// top beside a picture that would not grow that way), on the screen unless
// `at_home`; otherwise a picture of the shape `pw` by `ph` fitted into the
// space (`room` while it is `reshaping`, else `goal`). `lean_y`: whether a
// kept-size one leans towards the top (`Claim::leans_y`).
Box claim_box(const Box &goal, const Box &room, const Box &within, double margin, bool kept, double kept_w,
	      double kept_h, int toward_x, int toward_y, const std::vector<Box> &boxes, const std::vector<bool> &gives,
	      const std::vector<int> &grows, bool at_home, bool reshaping, double pw, double ph,
	      bool *lean_y = nullptr);
// The same from a fit's inputs (`out`: what `space_from` gives for them,
// worked out when not given).
Box claim_from(const SpaceInputs &in, const SpaceOut *out = nullptr, bool *lean_y = nullptr);
// Every fit of the free space in a scene whose item is there: the item it
// fits (its id, its source's name), whether that item shows, and, once the
// fit has worked its space out (`worked`), that space (`space`: the whole
// space it holds, before it is cut to the picture), its goal and what it was
// worked out from. A hidden item's fit keeps what it last worked out while
// it showed, and works it out again when it shows. Not worked out yet (its
// scene not shown since OBS started, or just given the item): `space` is
// the room it claims, if any. Only enabled fits.
struct HeldSpace {
	int64_t item = 0;
	std::string name;
	bool shown = true;
	bool worked = false;
	Box space, goal;
	bool resizes = true;
	SpaceInputs in;
};
std::vector<HeldSpace> held_spaces(obs_source_t *scene);
// Prefer an enabled owner when two filters name the same source.
bool fit_settings_for(obs_source_t *scene, const char *name, FitSettings &settings, std::string &resizing_filter);
// For the tests: `stagekit_test_fit_predicts`, whether what `held_spaces`
// says of each fit gives, worked out again, the claim that fit makes.
void fit_engine_add_test_calls();

// What a fit has found out about its item in one tick, before its space is
// looked for. `item` is valid for that tick only: the host holds it.
struct Prep {
	obs_sceneitem_t *item = nullptr;
	FitSettings s;
	double sw = 0.0, sh = 0.0; // the size of the item's source
	obs_sceneitem_crop crop{}; // what the item crops away
	Measure how;               // how other items are measured
	double bare_w = 0.0, bare_h = 0.0;
	double left = 0.0, top = 0.0, right = 0.0, bottom = 0.0; // what is not fitted, on each side
	double pw = 0.0, ph = 0.0;                               // the size of what is fitted
	double bx = 0.0, by = 0.0, bw = 0.0, bh = 0.0;           // the screen, or the box
	double keep_w = 0.0, keep_h = 0.0;                       // the size kept, when it is not resized
	double picture = 1.0;                                    // the shape fitted
	bool free = false;                                       // in the free space round it
	// The picture before what shows of it is cut from it ("Use visible
	// pixels"): where it starts in the source, and its size.
	double whole_left = 0.0, whole_top = 0.0, whole_w = 0.0, whole_h = 0.0;
};

// One thing in the way of a fitted item: what shows of it, or where another
// fit is taking it (`said`), whether that fit resizes it (`gives`), and
// whether it is one of the items never in the way (`passed`).
// `above`: placed by a fit that takes its space before this one.
struct Obstacle {
	Box box;
	int64_t id = 0;
	bool said = false, gives = false, passed = false, above = false;
	// (For one that gives way: whether it would take more room across or
	// down. For a kept-size fit's of the same standing: 1, it lies to the
	// left of its space, 2, at its top, for a resized picture's sake.)
	bool grows_x = true, grows_y = true;
	int leans = 0;
	// Another fit's item: where it lies in the layout as made by hand.
	Box home;
	bool has_home = false;
	Box at; // its item's box in the scene now (none for where another fit takes it)
};

// What is in the way: the items of a scene, each looked at (drawn and read
// back) when first met and afterwards followed by its box. One per host: a
// Fit to Space has its own; a Scene Fit has one for all its entries.
class FitScan {
public:
	// Goes through the scene's visible items (into groups, but for a group
	// another fit places). `self` is left out, and so is what is in it.
	void gather(obs_source_t *scene, obs_sceneitem_t *self, const void *asking);
	// The things in the way of `self`, from what was gathered: items another
	// fit (not `asking`) places by where it is taking them, the rest by what
	// shows of them. Items in `skip` are left out, or marked when `keep_skipped`.
	// With `mine`: an item placed by a fit `mine` outranks is in the way
	// only by what that fit holds whatever happens (`Claim::least`, for one
	// keeping its size), not by where it is going, and one placed by a fit that outranks `mine` is firm.
	void obstacles(obs_source_t *scene, obs_sceneitem_t *self, const std::vector<std::string> &skip,
		       bool keep_skipped, const void *asking, const Measure &how, std::vector<Obstacle> &out,
		       const Rank *mine = nullptr);
	// At rest: one glance a tick at the gathered items and `extra`, and one
	// proper look at an item a glance shows to have changed.
	void at_rest(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &extra, const void *glance_owner,
		     const Measure &how);
	// Everything is looked at again.
	void forget() { look_again_ = true; }
	// What was gathered, for a host that also wants other items left out.
	struct Gathered {
		obs_sceneitem_t *item; // valid for the tick
		obs_sceneitem_t *top;  // the item of the scene it is in, or itself
		bool claimed;          // a group placed by a fit: one thing
	};
	std::vector<Gathered> &gathered() { return gathered_; }
	// Every item the last `obstacles` went through, in the way or not.
	const std::vector<int64_t> &visible_ids() const { return visible_; }
	const std::vector<int64_t> &empty_ids() const { return empty_; }

private:
	struct Seen {
		Box box;     // what shows in it, on the canvas, when it was looked at
		Box item_at; // and where its box was then
		bool there = false;
		uint32_t width = 0, height = 0;
	};
	void look(obs_source_t *scene, obs_sceneitem_t *other, const Measure &how);
	std::vector<Gathered> gathered_;
	std::vector<obs_sceneitem_t *> used_; // what the last obstacles were taken from
	std::vector<int64_t> visible_, empty_;
	std::map<int64_t, Seen> seen_;
	std::map<int64_t, Glance> looked_; // what a glance said of each item when it was last looked at
	bool look_again_ = true;
};

// One fitted item: the whole of a Fit to Space filter but its window. A Fit
// to Space has one, kept in its own settings (`prefix` empty); a Scene Fit
// has one per entry, kept under "fit|<source>|" in the Scene Fit's settings.
class FitEngine {
public:
	// `entry`: the source's name for an entry of a Scene Fit; empty for a
	// Fit to Space, whose settings name the item. `look_prefix` and
	// `look_list_prefix`: where how it fits is read from, when that is not
	// its own (a Scene Fit's settings shared by all its entries); what it
	// writes for itself (home, kept size) stays under `prefix`.
	FitEngine(obs_data_t *settings, obs_source_t *owner, std::string prefix = std::string(),
		  std::string entry = std::string(), std::string look_prefix = std::string(),
		  std::string look_list_prefix = std::string());
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
	// Where how it fits is read from at the next `update` (empty: its own).
	void set_look(std::string look_prefix, std::string look_list_prefix);
	// `forget`: what was seen of the scene's items is let go (a Fit to
	// Space joining or leaving; a Scene Fit does it once for all entries).
	void attach(obs_source_t *parent, bool forget = true);
	void detach(bool forget = true);
	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }
	const std::string &prefix() const { return prefix_; }
	std::string item_name()
	{
		std::lock_guard lock(mutex_);
		return settings_.item;
	}
	bool holds_room(obs_sceneitem_t *item, const FitSettings &s);
	// A Scene Fit entry's place in its list (0 the top); read at each prepare.
	void set_entry_index(int index) { entry_index_ = index; }
	// Where it stands in its scene, as found at its last prepare.
	const Rank &rank() const { return rank_; }
	// A Fit to Space's tick: the steps below, in order, with its own scan.
	void tick(float seconds, FitScan &scan);
	// The filter has just been switched on: the item is taken as it is now,
	// its place, its size and the layout round it (its home, see `solve`).
	void switched_on();

	// ---- the steps, for a host that fits several items together
	// Off screen with nothing to do: true, and the tick is skipped (four
	// times a second it is not, so a hidden item's room stays on record).
	bool idle(float seconds);
	// Everything about the item before its space is looked for. False when
	// there is nothing more to do this tick (hidden, coming back, no picture).
	bool prepare(obs_sceneitem_t *item, float seconds, Prep &p);
	enum class Motion { Stop, Hold, Solve };
	// Off screen, moved by hand, its place taken: whether to look for a space.
	Motion motion(Prep &p, float seconds);
	bool arriving() const { return shown_for_ < kArriving; }
	// What is in the way changed? (`seconds` 0 on a second pass of one tick.)
	// Says whether it is at rest, for the host's glances. `visible`: every
	// item the obstacles were taken from (`FitScan::visible_ids`).
	void observe(const std::vector<Obstacle> &in_way, bool arriving, float seconds, bool &at_rest,
		     const std::vector<int64_t> *visible = nullptr);
	// The goal: false when there is no room at all. `moved` says whether it changed.
	bool solve(Prep &p, float seconds, bool first_pass, bool &moved);
	// What it will show once it has got there, for the other fits.
	void publish_claim(const Prep &p);
	// Along the glide, then the item is put there.
	Box advance(const Prep &p, float seconds);
	void apply(const Prep &p, Box space);
	bool gliding() const { return glide_ < 1.0; }
	bool hidden() const { return hidden_; }
	const Box &goal() const { return goal_; }
	bool has_goal() const { return have_goal_; }
	bool look_again() { return std::exchange(look_again_, false); }
	// A Scene Fit with All sources: another fit's claim is in the way even
	// where this item's place lies inside it (see `solve`).
	void set_claims_firm(bool firm) { claims_firm_ = firm; }
	// For the tests: full ticks had, and glides set off.
	uint64_t full_ticks() const { return full_ticks_; }
	uint64_t glides() const { return glides_; }
	void count_glide() { glides_++; }
	// Off screen with nothing asked of it, for a host that decides that
	// itself: whether anything is asked, and the time skipped.
	bool pending()
	{
		std::lock_guard lock(mutex_);
		return dirty_ || placed_ || shared_in_ || new_item_;
	}
	void skip_time(float seconds) { idle_s_ += seconds; }
	void not_free() { have_cur_ = false; }
	Box current() const { return cur_; }

private:
	void store_bool(const char *key, bool v);
	void store_double(const char *key, double v);
	void store_string(const char *key, const char *v);
	void soft_sides(bool &l, bool &r, bool &t, bool &b) const;
	// Whether a kept-size item lies at the top of its space: a resizing
	// fit's picture above or below it would not grow into more height,
	// and nothing else decides it (`x` is always false).
	void lean_axes(const Prep &p, bool &x, bool &y) const;
	// (A resized picture's space cut to it: `space_from`, with
	// `trim_for_kept_in` and `trim_to_picture_in` in fit-engine.cpp.)
	// The picture is of another shape than its space was cut for: OBS is
	// given the space before the cut, so the picture is fitted at once.
	bool reshaping(const Prep &p) const;
	// Says where the item will be, with where this fit stands.
	void say(int64_t item, Claim claim);
	Box kept_in(const Prep &p, const Box &room, double kw, double kh) const;
	// Where the picture lies at its home, as it shows now; false with no
	// home, or one that no longer fits the picture (another shape, another
	// kept size).
	bool home_box(const Prep &p, Box &out) const;
	// The home taken from the item as it is now.
	bool home_from_item(const Prep &p);
	void store_home_space();

	obs_source_t *source_;
	std::string prefix_, entry_, list_prefix_;
	std::string look_prefix_, look_list_prefix_; // under mutex_: where how it fits is read from
	obs_source_t *parent_ = nullptr;
	int entry_index_ = 0;
	Rank rank_;
	std::mutex mutex_;
	FitSettings settings_;
	bool dirty_ = true; // under mutex_: settings changed since the last full tick
	double idle_s_ = 0.0, idle_phase_ = 0.0;
	std::atomic<uint64_t> full_ticks_{0}, glides_{0};
	// The room shadows take round the item's source, read again only when it
	// changes (`shadow_room_revision`) or the source does.
	obs_source_t *room_src_ = nullptr;
	uint64_t room_rev_ = 0;
	double room_c_[4] = {0.0, 0.0, 0.0, 0.0};
	bool in_view_c_ = false;
	// A window switcher's change answered in this tick, on every pass.
	bool with_switch_ = false;
	Glance own_looked_; // what a glance said of the item itself at its last look
	bool own_looked_have_ = false;
	std::vector<Box> boxes_;  // what is in the way now
	std::vector<bool> gives_; // and which of them another filter resizes to make room
	std::vector<int> grows_;  // of those, 1: it would take more width, 2: more height
	std::vector<int> leans_;  // of the kept-size fits' of the same standing, which lean (Obstacle::leans)
	std::vector<int64_t> ids_;
	std::vector<Box> at_;          // each one's item's box when it was looked at
	std::vector<int64_t> visible_; // every item they were taken from
	std::vector<bool> said_;
	bool claims_firm_ = false; // on the tick only
	std::vector<bool> passed_; // which are items never in the way, counted only when the picture leaves room
	std::vector<bool> above_;  // which are placed by a fit that takes its space before this one
	bool avoiding_ = false;    // they count just now
	std::array<double, 8> used_rule_{-1.0};
	static constexpr double kArriving = 0.25;
	bool was_showing_ = false;
	double shown_for_ = 0.0;
	bool look_again_ = true, changed_ = false, unsettled_ = false;
	double calm_for_ = 0.0;
	double shape_seen_ = 0.0, shape_used_ = 0.0, shape_for_ = 0.0;
	// A window switcher's change is under way: the shape it is going to is
	// `shape_used_`, until the picture has that shape (`home_box` goes by it).
	bool switching_ = false;
	double used_margin_ = -1.0, used_tolerance_ = -1.0;
	double set_bw_ = 0.0, set_bh_ = 0.0; // the bounding box last given to the item
	double set_k_ = 0.0;                 // and the scale the picture was drawn at in it
	// A resize by hand under way: the box it has, and how long it has kept
	// it. It counts once the box has stopped changing (`kHandStill`).
	static constexpr double kHandStill = 0.3;
	bool hand_on_ = false;
	double hand_bw_ = 0.0, hand_bh_ = 0.0, hand_still_ = 0.0;
	// Whether more width or height would grow the resized picture, from the
	// space it was given before that was cut to the picture.
	bool space_grows_x_ = true, space_grows_y_ = true;
	double keep_w_ = 0.0, keep_h_ = 0.0; // the reference image size, in scene pixels
	// What shows of the fitted item itself: the part of its picture cut off
	// on the left, top, right and bottom, when it was last looked at.
	double own_cut_[4] = {0.0, 0.0, 0.0, 0.0};
	bool own_seen_ = false, was_placed_ = false;
	// The size of the picture (before what shows is cut from it) that what
	// was seen goes with, and how long since it was last looked at.
	double own_sw_ = 0.0, own_sh_ = 0.0, own_for_ = 0.0;
	bool have_keep_ = false;
	// The kept size as it is saved in the settings, and whether the settings
	// have been read yet (the first time is OBS loading the filter).
	bool first_update_ = true;
	std::string saved_keep_item_;
	double saved_keep_k_ = 0.0; // legacy, used only until both pixel dimensions are saved
	double saved_keep_w_ = 0.0, saved_keep_h_ = 0.0;
	bool load_keep_ = false;
	bool back_to_original_ = false, resized_by_hand_ = false;
	bool placed_by_tool_ = false;
	bool placed_ = false; // under mutex_: the item was just placed on purpose by a tool
	// Under mutex_: the item was just given its place by another scene's filter.
	bool shared_in_ = false, shared_home_ = false;
	double shared_home_x_ = 0.0, shared_home_y_ = 0.0;
	// The item's source has kept its size this long.
	double steady_sw_ = 0.0, steady_sh_ = 0.0, steady_for_ = 0.0;
	Box goal_, from_, cur_;
	// The space before `trim_to_picture_in` cut it, glided with the goal, and
	// whether it was what OBS was given last (the picture of another shape).
	Box room_goal_, room_from_, room_cur_;
	bool handed_room_ = false;
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
	std::atomic<uint64_t> hidden_since_{0};
	bool hidden_ = false, coming_back_ = false, have_claim_ = false;
	// What the goal was last worked out from, for `held_spaces` (under
	// held_mutex_: asked from the window's thread). Kept while the item is
	// hidden; let go when the fit is given another item or leaves the free
	// space.
	std::mutex held_mutex_;
	bool have_held_ = false;
	int64_t held_item_ = 0;
	std::string held_name_;
	bool held_resizes_ = true;
	SpaceInputs held_in_;
	Box held_space_, held_goal_;

public:
	// What `held_spaces` gives of this engine: false when it has worked
	// nothing out (and then `name` is the item it is given, if any).
	bool held(HeldSpace &out);
	bool recently_hidden() const;
	FitSettings settings()
	{
		std::lock_guard lock(mutex_);
		return settings_;
	}
	// In the free space? (`resizes`: whether it resizes its item.)
	bool in_free_space(bool *resizes = nullptr)
	{
		std::lock_guard lock(mutex_);
		if (resizes)
			*resizes = settings_.resize;
		return settings_.box == BoxKind::FreeSpace;
	}

private:
	bool box_asked_ = false; // its bounding box was asked for, for want of a room it was given
	bool new_item_ = false;  // the filter was given another item (set under mutex_)
	double back_for_ = 0.0;
	Claim last_claim_;
	double set_x_ = 0.0, set_y_ = 0.0, seen_x_ = 0.0, seen_y_ = 0.0, still_for_ = 0.0;
	// The layout as made by hand (the owner, 2026-11-18: everything changed
	// size when the filter was switched on over a layout made with it off).
	// Taken when the filter is switched on (`switched_on`), and again when
	// the item is moved or resized by hand or placed by a tool: the box its
	// picture had then (`home_`, the whole picture before what shows is cut
	// from it) and the space it found round it with the other fits' items at
	// their homes (`home_space_`). While the space is that again and the
	// picture there keeps the gap, the picture goes exactly there.
	bool was_off_ = false;   // the filter was seen switched off (on the tick)
	bool take_home_ = false; // switched on just now: taken at the next prepare
	bool home_due_ = false;  // taken at the end of this prepare
	bool has_home_ = false, have_home_space_ = false, at_home_ = false;
	Box home_, home_space_;
	bool home_settling_ = false; // the space round the home is still being found
	double home_settle_ = 0.0;
	static constexpr double kHomeSettle = 0.5;
	std::vector<Box> homes_; // of what is in the way: where each lies at home (slack: kHomeSlack)
	std::vector<bool> has_homes_;
	// Under mutex_: the home as saved, to be taken up at the next prepare.
	bool home_loaded_ = false;
	std::string saved_home_item_;
	Box saved_home_, saved_home_space_;
	double saved_home_cut_[4] = {0.0, 0.0, 0.0, 0.0};
	// What showed of it when the home was saved, taken as its first
	// measure until it is looked at (a home is what shows of it).
	bool have_loaded_cut_ = false;
	double loaded_cut_[4] = {0.0, 0.0, 0.0, 0.0};
	void store_home_cut();
};

// Every engine there is: told of an item placed by a tool, or given its place
// by another scene's fit; and the filters that own them (each held).
void engines_placed(obs_source_t *scene, const char *item_name);
void engines_shared(obs_source_t *scene, const char *item_name, const double *home);
std::vector<obs_source_t *> engine_owners();
// Where a Scene Fit entry's two lists are kept: "fit_<the name's bytes in
// hex>_" then "skip" or "share_scenes". (A list's setting cannot have a bar
// in it, and a source's name may.)
std::string fit_list_prefix(const std::string &entry);
// The controls of one fit (a Fit to Space's own, or a Scene Fit entry's)
// added to `props`: settings `prefix` + name, the two lists `list_prefix` +
// "skip" / "share_scenes". For an entry (`entry`) the overlap switch is the
// Scene Fit's own, not added here.
void fit_add_controls(obs_properties_t *props, const std::string &prefix, const std::string &list_prefix,
		      obs_source_t *owner, obs_source_t *parent, bool entry);
// Shows the controls that apply to the kind of box and the overlap rules.
void fit_controls_shown(obs_properties_t *top, obs_data_t *settings, const std::string &prefix,
			const std::string &list_prefix, bool entry);
// A Scene Fit entry's defaults, under its prefix: Fit to Space's, but in the
// free space with a gap of 30.
void fit_entry_defaults(obs_data_t *settings, const std::string &prefix);
// For the tests: the full ticks and glides of the engines `owner` has (of
// the item of that name only, when one is given).
void engine_counts(obs_source_t *owner, const char *item, uint64_t &ticks, uint64_t &glides);
// How many full ticks the engines have had (off screen, most are skipped).
uint64_t engine_full_ticks();

} // namespace stagekit::fit
