/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Fit: the sources of a scene fitted together. Each entry is a Fit to
// Space (the same engine, the same settings, kept under "fit|<source>|"),
// but they are worked out in one tick, in the order of the list, and again
// (up to four times) while one entry's new place moves another's: so after
// a change every entry sets off once, all at the same moment, instead of one
// filter answering another a tick later.
//
// With All sources, every source of the scene is an entry too, after the
// listed ones and from the top of the scene down, with the entry settings a
// new entry starts with (each can be changed under Settings for). Left out:
// Ignored sources, the Morph layer, Standby stand-ins, what a Fit to Space
// or another Scene Fit of the scene already fits, and a source that fills
// the screen (nine tenths of it or more) when first met, such as the
// background: it has nowhere to go but smaller, and the free space round a
// place inside it is not in its way anyway. A source put down inside a
// fitted source's space waits until that one has made room, and no entry's
// space is looked for round a place inside another's claim. Once fitted (its size noted
// before the first fit, `orig_item`, is saved) a source stays an entry
// however large it is fitted to.

#include "scene-fit.hpp"
#include "fit-engine.hpp"
#include "fit-item.hpp"

#include "../common/filter-input.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-rev.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

using namespace fit;

namespace {

constexpr int kPasses = 8;          // the most times the entries are worked out in one tick (each way round in turn)
constexpr uint32_t kIdleEvery = 15; // off screen, a full tick four times a second
// On screen at rest (nothing in the scene changed, no claim, glance, shadow
// room or settings, and nothing under way in any entry for `kQuietTicks` full
// ticks in a row): a full tick every `kRestEvery` only, a slow look for
// whatever no revision tells of.
constexpr uint32_t kQuietTicks = 30;
constexpr uint32_t kRestEvery = 30;
constexpr double kLookForSources = 0.5; // with All sources, how often the scene's sources are counted
constexpr double kCountAnyway = 2.0;    // and how often when nothing in the scenes changed
constexpr double kFillsScreen = 0.9;    // a source this much of the screen is never fitted unasked

// Sources fitted. Saved by number: append only. A filter saved before there
// was a choice has no number and reads as Chosen sources, its list, which is
// all it fitted then.
enum class Which : int { Chosen = 0, AllSources = 1 };

// Settings (`settings_mode`). Saved by number: append only. A filter saved
// before there was a choice has no number and reads as each its own.
enum class Shared : int { Each = 0, Same = 1 };
// Where the set shared by every entry is kept.
const char *const kGroupPrefix = "group|";
const char *const kGroupLists = "group_";

// The names of the fit keys a Fit to Space keeps, with what kind each is,
// and whether it is written by the fit itself (`own`: its home and the size
// it keeps), not set by the user.
enum class Kind { Int, Double, Bool, String };
struct FitKey {
	const char *name;
	Kind kind;
	bool own;
};
const FitKey kFitKeys[] = {
	{"box", Kind::Int, false},
	{"box_x", Kind::Double, false},
	{"box_y", Kind::Double, false},
	{"box_width", Kind::Double, false},
	{"box_height", Kind::Double, false},
	{"margin", Kind::Double, false},
	{"tolerance", Kind::Double, false},
	{"resize", Kind::Bool, false},
	{"toward", Kind::Int, false},
	{"space_priority", Kind::Int, false},
	{"visible_pixels", Kind::Bool, false},
	{"no_shadows", Kind::Bool, false},
	{"glide_ms", Kind::Int, false},
	{"ease_curve", Kind::Int, false},
	{"ease_mode", Kind::Int, false},
	{"skip_use_pct", Kind::Bool, false},
	{"skip_fill", Kind::Double, false},
	{"skip_use_px", Kind::Bool, false},
	{"skip_px_top", Kind::Double, false},
	{"skip_px_bottom", Kind::Double, false},
	{"skip_px_left", Kind::Double, false},
	{"skip_px_right", Kind::Double, false},
	{"have_anchor", Kind::Bool, true},
	{"anchor_x", Kind::Double, true},
	{"anchor_y", Kind::Double, true},
	{"orig_item", Kind::String, true},
	{"orig_k", Kind::Double, true},
	{"orig_w", Kind::Double, true},
	{"orig_h", Kind::Double, true},
	{"keep_item", Kind::String, true},
	{"keep_k", Kind::Double, true},
	{"keep_w", Kind::Double, true},
	{"keep_h", Kind::Double, true},
	{"keep_sw", Kind::Double, true},
	{"keep_sh", Kind::Double, true},
	{"keep_pw", Kind::Double, true},
	{"keep_ph", Kind::Double, true},
	{"home_item", Kind::String, true},
	{"home_x", Kind::Double, true},
	{"home_y", Kind::Double, true},
	{"home_w", Kind::Double, true},
	{"home_h", Kind::Double, true},
	{"home_space_x", Kind::Double, true},
	{"home_space_y", Kind::Double, true},
	{"home_space_w", Kind::Double, true},
	{"home_space_h", Kind::Double, true},
	{"home_cut_l", Kind::Double, true},
	{"home_cut_t", Kind::Double, true},
	{"home_cut_r", Kind::Double, true},
	{"home_cut_b", Kind::Double, true},
};

std::string prefix_of(const std::string &entry)
{
	return "fit|" + entry + "|";
}

// Whether an entry fits at all (`fit|<source>|enabled`, on as it comes, so
// every filter saved before reads on). Always the source's own, also with
// the same settings for all. Not one of kFitKeys: Take over would copy a
// Fit to Space's missing key as off.
bool entry_on(obs_data_t *settings, const std::string &name)
{
	const std::string key = prefix_of(name) + "enabled";
	return !obs_data_has_user_value(settings, key.c_str()) || obs_data_get_bool(settings, key.c_str());
}

bool has(const std::vector<std::string> &list, const std::string &name)
{
	return std::find(list.begin(), list.end(), name) != list.end();
}

// With All sources, the sources of the scene fitted besides the listed ones,
// from the top of the scene down; empty otherwise. `self` is the Scene Fit's
// own filter. Read from the scene as it is, so the window and the tick agree.
std::vector<std::string> unlisted_sources(obs_source_t *self, obs_source_t *scene, obs_data_t *settings)
{
	std::vector<std::string> out;
	obs_scene_t *s = scene && !obs_source_is_group(scene) ? obs_scene_from_source(scene) : nullptr;
	if (!s || obs_data_get_int(settings, "which") != (int)Which::AllSources)
		return out;
	const std::vector<std::string> listed = pick_list_read(settings, "entries", false);
	const std::vector<std::string> skip = pick_list_read(settings, "skip", false);
	// What another fit of the scene looks after, and whether a Scene Fit
	// with All sources comes before this one (that one fits the rest).
	struct Others {
		obs_source_t *self;
		bool reached = false, first = true;
		std::vector<std::string> taken;
	} others{self};
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *o = static_cast<Others *>(param);
			if (f == o->self) {
				o->reached = true;
				return;
			}
			const char *id = obs_source_get_unversioned_id(f);
			if (!id || !obs_source_enabled(f))
				return;
			obs_data_t *theirs = nullptr;
			if (std::strcmp(id, kFitItemId) == 0) {
				theirs = obs_source_get_settings(f);
				o->taken.push_back(obs_data_get_string(theirs, "item"));
			} else if (std::strcmp(id, kSceneFitId) == 0) {
				theirs = obs_source_get_settings(f);
				for (const std::string &name : pick_list_read(theirs, "entries", false))
					o->taken.push_back(name);
				if (!o->reached && obs_data_get_int(theirs, "which") == (int)Which::AllSources)
					o->first = false;
			}
			obs_data_release(theirs);
		},
		&others);
	if (!others.first)
		return out;
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(
		s,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
			return true;
		},
		&items);
	// Fitted before: its size before the first fit is on record.
	auto fitted_before = [settings](const std::string &name) {
		return name == obs_data_get_string(settings, (prefix_of(name) + "orig_item").c_str());
	};
	// Where the sources already fitted are (a fitted item is kept by its
	// top-left corner and its bounding box).
	std::vector<Box> fitted_boxes;
	for (obs_sceneitem_t *item : items) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
		if (!name || obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE ||
		    !(has(listed, name) || fitted_before(name)))
			continue;
		vec2 pos, bounds;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_bounds(item, &bounds);
		fitted_boxes.push_back({pos.x, pos.y, bounds.x, bounds.y});
	}
	for (auto at = items.rbegin(); at != items.rend(); ++at) {
		obs_sceneitem_t *item = *at;
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name_c = obs_source_get_name(source);
		if (!name_c || !*name_c || is_morph_layer(item) || is_stand_in(source) || obs_obj_is_private(source) ||
		    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
			continue;
		const std::string name = name_c;
		if (has(listed, name) || has(skip, name) || has(others.taken, name) || has(out, name))
			continue;
		if (!fitted_before(name)) {
			const double share = screen_share(item);
			if (share <= 0.0 || share >= kFillsScreen)
				continue; // no size yet (looked at again later), or fills the screen
			// Put down inside a fitted source's space: not fitted yet. Its
			// place lies inside that space, so the fit would take the
			// same space for it (what a place lies inside is not in its
			// way) and the two would overlap for good. Meanwhile it is in
			// the other's way: that one makes room, and this one is
			// fitted into the room once its place is clear.
			// (Its middle from its position, size and alignment: an item
			// just added has a box of 1x1 until its scene is drawn.)
			vec2 pos, scale;
			obs_sceneitem_get_pos(item, &pos);
			obs_sceneitem_get_scale(item, &scale);
			obs_sceneitem_crop crop;
			obs_sceneitem_get_crop(item, &crop);
			double w = std::max(0.0, (double)obs_source_get_width(source) - crop.left - crop.right) *
				   std::fabs(scale.x);
			double h = std::max(0.0, (double)obs_source_get_height(source) - crop.top - crop.bottom) *
				   std::fabs(scale.y);
			if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
				vec2 bounds;
				obs_sceneitem_get_bounds(item, &bounds);
				w = bounds.x;
				h = bounds.y;
			}
			const uint32_t align = obs_sceneitem_get_alignment(item);
			const double cx = pos.x + ((align & OBS_ALIGN_LEFT)    ? w / 2.0
						   : (align & OBS_ALIGN_RIGHT) ? -w / 2.0
									       : 0.0);
			const double cy = pos.y + ((align & OBS_ALIGN_TOP)      ? h / 2.0
						   : (align & OBS_ALIGN_BOTTOM) ? -h / 2.0
										: 0.0);
			bool inside = false;
			for (const Box &b : fitted_boxes)
				inside = inside || (cx > b.x && cx < b.x + b.w && cy > b.y && cy < b.y + b.h);
			if (inside)
				continue;
		}
		out.push_back(name);
	}
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
	return out;
}

bool same_for_all(obs_data_t *settings)
{
	return obs_data_get_int(settings, "settings_mode") == (int)Shared::Same;
}

// How one set fits (not what a fit writes for itself) copied over another:
// a value left on its default stays so (both sets have the same defaults).
void copy_look(obs_data_t *settings, const std::string &from, const std::string &from_lists, const std::string &to,
	       const std::string &to_lists)
{
	for (const FitKey &key : kFitKeys) {
		if (key.own)
			continue;
		const std::string a = from + key.name, b = to + key.name;
		if (!obs_data_has_user_value(settings, a.c_str())) {
			obs_data_unset_user_value(settings, b.c_str());
			continue;
		}
		switch (key.kind) {
		case Kind::Int:
			obs_data_set_int(settings, b.c_str(), obs_data_get_int(settings, a.c_str()));
			break;
		case Kind::Double:
			obs_data_set_double(settings, b.c_str(), obs_data_get_double(settings, a.c_str()));
			break;
		case Kind::Bool:
			obs_data_set_bool(settings, b.c_str(), obs_data_get_bool(settings, a.c_str()));
			break;
		case Kind::String:
			obs_data_set_string(settings, b.c_str(), obs_data_get_string(settings, a.c_str()));
			break;
		}
	}
	for (const char *list : {"skip", "share_scenes"})
		pick_list_write(settings, (to_lists + list).c_str(), false,
				pick_list_read(settings, (from_lists + list).c_str(), false));
}

// Whether two sets fit alike (as read, defaults included).
bool same_look(obs_data_t *settings, const std::string &a, const std::string &a_lists, const std::string &b,
	       const std::string &b_lists)
{
	for (const FitKey &key : kFitKeys) {
		if (key.own)
			continue;
		const std::string x = a + key.name, y = b + key.name;
		switch (key.kind) {
		case Kind::Int:
			if (obs_data_get_int(settings, x.c_str()) != obs_data_get_int(settings, y.c_str()))
				return false;
			break;
		case Kind::Double:
			if (obs_data_get_double(settings, x.c_str()) != obs_data_get_double(settings, y.c_str()))
				return false;
			break;
		case Kind::Bool:
			if (obs_data_get_bool(settings, x.c_str()) != obs_data_get_bool(settings, y.c_str()))
				return false;
			break;
		case Kind::String:
			if (std::strcmp(obs_data_get_string(settings, x.c_str()),
					obs_data_get_string(settings, y.c_str())))
				return false;
			break;
		}
	}
	for (const char *list : {"skip", "share_scenes"})
		if (pick_list_read(settings, (a_lists + list).c_str(), false) !=
		    pick_list_read(settings, (b_lists + list).c_str(), false))
			return false;
	return true;
}

// Settings changed from each its own to the same for all, or back, since it
// was last settled (`settings_mode_was`): to the same, the shared set starts
// as the first entry's (the defaults with no entry); back, every entry is
// given the shared set. Either way nothing moves. True when it did either.
// `names`: the entries, the first one first.
bool settle_mode(obs_data_t *settings, const std::vector<std::string> &names)
{
	const long long mode = obs_data_get_int(settings, "settings_mode");
	if (mode == obs_data_get_int(settings, "settings_mode_was"))
		return false;
	fit_entry_defaults(settings, kGroupPrefix);
	if (mode == (int)Shared::Same) {
		if (!names.empty()) {
			fit_entry_defaults(settings, prefix_of(names.front()));
			copy_look(settings, prefix_of(names.front()), fit_list_prefix(names.front()), kGroupPrefix,
				  kGroupLists);
		} else {
			for (const FitKey &key : kFitKeys)
				if (!key.own)
					obs_data_unset_user_value(settings,
								  (std::string(kGroupPrefix) + key.name).c_str());
			for (const char *list : {"skip", "share_scenes"})
				pick_list_write(settings, (std::string(kGroupLists) + list).c_str(), false, {});
		}
	} else {
		for (const std::string &name : names) {
			fit_entry_defaults(settings, prefix_of(name));
			copy_look(settings, kGroupPrefix, kGroupLists, prefix_of(name), fit_list_prefix(name));
		}
	}
	obs_data_set_int(settings, "settings_mode_was", mode);
	return true;
}

// Every source fitted: the listed ones, then (with All sources) the rest.
std::vector<std::string> fitted_names(obs_source_t *self, obs_data_t *settings)
{
	std::vector<std::string> names = pick_list_read(settings, "entries", false);
	obs_source_t *scene = self ? obs_filter_get_parent(self) : nullptr;
	for (const std::string &name : unlisted_sources(self, scene, settings))
		names.push_back(name);
	return names;
}

class SceneFit;
std::mutex g_fits_mutex;
std::vector<SceneFit *> g_fits;

class SceneFit {
public:
	SceneFit(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		idle_turn_ = (uint32_t)((reinterpret_cast<uintptr_t>(this) >> 4) % kIdleEvery);
		rest_turn_ = (uint32_t)((reinterpret_cast<uintptr_t>(this) >> 4) % kRestEvery);
		update(settings);
		std::lock_guard lock(g_fits_mutex);
		g_fits.push_back(this);
	}
	~SceneFit()
	{
		std::lock_guard lock(g_fits_mutex);
		g_fits.erase(std::remove(g_fits.begin(), g_fits.end(), this), g_fits.end());
	}

	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

	void update(obs_data_t *settings)
	{
		std::vector<std::string> names = pick_list_read(settings, "entries", false);
		const bool all = obs_data_get_int(settings, "which") == (int)Which::AllSources;
		{
			// The unlisted sources as last counted, less any now listed or
			// ignored; counted again in the next tick.
			const std::vector<std::string> skip = pick_list_read(settings, "skip", false);
			std::lock_guard lock(mutex_);
			all_ = all;
			if (!all)
				unlisted_.clear();
			std::vector<std::string> kept;
			for (const std::string &name : unlisted_)
				if (!has(names, name) && !has(skip, name))
					kept.push_back(name);
			unlisted_ = kept;
			for (const std::string &name : kept)
				names.push_back(name);
			count_now_ = true;
		}
		// Settings changed between each its own and the same for all
		// (from outside the window: the window settles it at once). As
		// OBS makes or loads the filter its settings are read as they
		// were saved, never settled.
		if (std::exchange(first_update_, false)) {
			if (obs_data_get_int(settings, "settings_mode") !=
			    obs_data_get_int(settings, "settings_mode_was"))
				obs_data_set_int(settings, "settings_mode_was",
						 obs_data_get_int(settings, "settings_mode"));
		} else if (settle_mode(settings, names)) {
			obs_log(LOG_INFO, "Scene Fit '%s': settings now %s", obs_source_get_name(source_),
				same_for_all(settings) ? "the same for all sources" : "each source's own");
		}
		build(settings, names);
	}

	void tick(float seconds);

private:
	// The entries for these names, in this order: a new one is made, and one
	// kept is given the settings again when `again` (new settings, not a
	// source come or gone).
	void build(obs_data_t *settings, const std::vector<std::string> &names, bool again = true)
	{
		for (const std::string &name : names)
			fit_entry_defaults(settings, prefix_of(name));
		// The same for all sources: each entry reads how to fit from the
		// shared set (what it writes for itself stays its own).
		const bool same = same_for_all(settings);
		const std::string look = same ? kGroupPrefix : std::string();
		const std::string look_lists = same ? kGroupLists : std::string();
		std::vector<std::shared_ptr<Entry>> was;
		{
			std::lock_guard lock(mutex_);
			was = entries_;
		}
		std::vector<std::shared_ptr<Entry>> now;
		for (const std::string &name : names) {
			// An entry switched off is no entry: no placing, no claim, as
			// for a source not fitted (its engine, gone, takes its claim
			// with it).
			if (!entry_on(settings, name))
				continue;
			std::shared_ptr<Entry> entry;
			for (const auto &old : was)
				if (old->name == name)
					entry = old;
			if (entry) {
				if (again) {
					entry->engine->set_look(look, look_lists);
					entry->engine->update(settings);
				}
			} else {
				entry = std::make_shared<Entry>();
				entry->name = name;
				entry->engine = std::make_unique<FitEngine>(settings, source_, prefix_of(name), name,
									    look, look_lists);
				if (parent_)
					entry->engine->attach(parent_, false);
			}
			now.push_back(entry);
		}
		std::lock_guard lock(mutex_);
		entries_ = now;
	}

	// With All sources: the scene's sources counted again, twice a second
	// (at once after new settings), and the entries made again when they
	// changed.
	void count_sources(float seconds)
	{
		bool all, now;
		{
			std::lock_guard lock(mutex_);
			all = all_;
			now = std::exchange(count_now_, false);
		}
		count_s_ += seconds;
		// Nothing in the scenes changed: counted every `kCountAnyway` only
		// (another filter's list, a source's size off screen).
		const uint64_t rev = scene_revision();
		if (!all || (!now && (count_s_ < kLookForSources || (rev == count_rev_ && count_s_ < kCountAnyway))))
			return;
		count_s_ = 0.0;
		count_rev_ = rev;
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::vector<std::string> found = unlisted_sources(source_, parent_, settings);
		bool changed;
		{
			std::lock_guard lock(mutex_);
			changed = found != unlisted_;
			if (changed)
				unlisted_ = found;
		}
		if (changed) {
			std::vector<std::string> names = pick_list_read(settings, "entries", false);
			names.insert(names.end(), found.begin(), found.end());
			build(settings, names, false);
		}
		obs_data_release(settings);
	}

public:
	void attach(obs_source_t *parent)
	{
		parent_ = parent;
		glance_forget(parent);
		std::lock_guard lock(mutex_);
		for (const auto &entry : entries_)
			entry->engine->attach(parent, false);
		forget_ = true;
	}
	void detach()
	{
		glance_forget(parent_);
		std::lock_guard lock(mutex_);
		for (const auto &entry : entries_)
			entry->engine->detach(false);
		parent_ = nullptr;
	}

private:
	struct Entry {
		std::string name;
		std::unique_ptr<FitEngine> engine;
	};
	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::vector<std::shared_ptr<Entry>> entries_; // under mutex_
	bool all_ = false;                            // All sources (under mutex_)
	std::vector<std::string> unlisted_;           // the sources fitted besides the listed ones (under mutex_)
	bool count_now_ = true;                       // (under mutex_)
	double count_s_ = 0.0;                        // on the tick only
	uint64_t count_rev_ = 0;                      // scene_revision() at the last count (on the tick only)
	// One look at what is in the way for each way of measuring the
	// entries use (normally one): by visible pixels, without shadows.
	std::map<int, FitScan> scans_;
	std::atomic<bool> forget_{false};
	uint32_t idle_turn_ = 0;
	// At rest on screen: what the last full tick went by, and how many
	// full ticks in a row found nothing changed and nothing under way.
	struct RestKey {
		uint64_t scenes = 0, claims = 0, glances = 0, rooms = 0;
		size_t entries = 0;
		bool operator==(const RestKey &o) const
		{
			return scenes == o.scenes && claims == o.claims && glances == o.glances && rooms == o.rooms &&
			       entries == o.entries;
		}
	};
	RestKey rest_key_;
	uint32_t quiet_ = 0, rest_turn_ = 0;
	bool rest_showing_ = false;
	size_t entries_count()
	{
		std::lock_guard lock(mutex_);
		return entries_.size();
	}
	void glance_at_rest();
	uint64_t unsettled_logged_ = 0;
	bool first_update_ = true; // the settings read as OBS made or loaded the filter
	bool was_off_ = false;     // seen switched off (on the tick only)
};

void SceneFit::tick(float seconds)
{
	std::vector<std::shared_ptr<Entry>> entries;
	{
		std::lock_guard lock(mutex_);
		entries = entries_;
	}
	// Not on a group (a group is only as large as what is in it), not
	// while a collection loads, not while switched off. (Switched off, the
	// sources are counted again at the first tick on, before the entries
	// take their items as they are: a source added meanwhile, or one that
	// was waiting inside another's box until the layout was made by hand,
	// is an entry with a home like the others, not one fitted half a second
	// later whose new room moves them all off theirs.)
	if (!obs_source_enabled(source_)) {
		was_off_ = true;
		std::lock_guard lock(mutex_);
		count_now_ = true;
	}
	if (!parent_ || !obs_source_enabled(source_) || obs_source_is_group(parent_) || !scenes_loaded()) {
		for (const auto &entry : entries)
			claim_clear(entry->engine.get());
		return;
	}
	// Off screen with nothing asked of any entry: four times a second only.
	// On screen at rest: twice a second only. Skipped, nothing is counted,
	// gathered or worked out; the time goes to each entry's next full tick
	// (`FitEngine::prepare`), and the sources are counted on a full one.
	const bool showing = obs_source_showing(parent_);
	bool asked = false;
	{
		std::lock_guard lock(mutex_);
		asked = count_now_;
	}
	for (const auto &entry : entries)
		asked = asked || entry->engine->pending();
	const RestKey key{scene_revision(), claims_revision(), glance_revision(), shadow_room_revision(),
			  entries.size()};
	if (showing != rest_showing_ || asked || !(key == rest_key_))
		quiet_ = 0;
	rest_showing_ = showing;
	const bool skip = !asked && (showing ? quiet_ >= kQuietTicks && ++rest_turn_ % kRestEvery != 0
					     : ++idle_turn_ % kIdleEvery != 0);
	if (skip) {
		count_s_ += seconds;
		for (const auto &entry : entries)
			entry->engine->skip_time(seconds);
		// The glances go on (every other frame is as often as they can
		// be started and read back), so a widget that starts or stops
		// drawing is noticed as soon as before (`glance_revision`).
		if (showing && rest_turn_ % 2 == 0)
			glance_at_rest();
		return;
	}
	count_sources(seconds);
	bool all;
	{
		std::lock_guard lock(mutex_);
		entries = entries_;
		all = all_;
	}
	// Switched on (it was seen off): every entry takes its item as it is
	// now, the layout made by hand while the filter was off (FitEngine::
	// switched_on). Not after loading: a filter saved on goes by its homes
	// as they were saved.
	if (std::exchange(was_off_, false))
		for (const auto &entry : entries)
			entry->engine->switched_on();
	// With All sources, every source is fitted: another entry's claim is
	// in the way even where an entry's place lies inside it (FitEngine::
	// solve), or two resized entries could take one space for good.
	for (const auto &entry : entries)
		entry->engine->set_claims_firm(all);
	if (forget_.exchange(false))
		scans_.clear();
	// Whether this full tick found everything at rest (see `quiet_`).
	bool quiet = showing;
	const size_t n = entries.size();
	// Each entry's item, held for this tick only.
	std::vector<obs_sceneitem_t *> items(n, nullptr);
	struct Release {
		std::vector<obs_sceneitem_t *> *items;
		~Release()
		{
			for (obs_sceneitem_t *item : *items)
				if (item)
					obs_sceneitem_release(item);
		}
	} release{&items};
	for (size_t i = 0; i < n; i++) {
		items[i] = find_item(parent_, entries[i]->name);
		if (items[i])
			obs_sceneitem_addref(items[i]);
	}

	// A: everything about each item before its space is looked for. A
	// hidden one, one coming back and one with no picture say where their
	// room is, and the others go by that in this same tick.
	std::vector<Prep> preps(n);
	enum class Way { Skip, Place, Hold, Solve };
	std::vector<Way> way(n, Way::Skip);
	for (size_t i = 0; i < n; i++) {
		// (Its place in the list, for "Priority by list order".)
		entries[i]->engine->set_entry_index((int)i);
		if (entries[i]->engine->prepare(items[i], seconds, preps[i]))
			way[i] = Way::Place;
	}
	// B: whether each in the free space has a space to look for.
	bool any_solve = false;
	for (size_t i = 0; i < n; i++) {
		if (way[i] != Way::Place)
			continue;
		FitEngine &engine = *entries[i]->engine;
		if (!preps[i].free) {
			engine.not_free();
			continue;
		}
		const FitEngine::Motion m = engine.motion(preps[i], seconds);
		way[i] = m == FitEngine::Motion::Stop   ? Way::Skip
			 : m == FitEngine::Motion::Hold ? Way::Hold
							: Way::Solve;
		any_solve = any_solve || way[i] == Way::Solve;
	}
	// C: what is in the way, gathered once, and every entry worked out
	// against it and against the other entries' new places.
	std::vector<Box> spaces(n);
	if (any_solve) {
		auto scan_of = [this](const Measure &how) -> FitScan & {
			return scans_[(how.visible_pixels ? 1 : 0) | (how.leave_out_shadows ? 2 : 0)];
		};
		bool again = false;
		for (size_t i = 0; i < n; i++)
			again = entries[i]->engine->look_again() || again;
		std::map<int, bool> gathered;
		for (size_t i = 0; i < n; i++) {
			if (way[i] != Way::Solve)
				continue;
			const int key = (preps[i].how.visible_pixels ? 1 : 0) |
					(preps[i].how.leave_out_shadows ? 2 : 0);
			if (gathered[key])
				continue;
			gathered[key] = true;
			FitScan &scan = scan_of(preps[i].how);
			if (again)
				scan.forget();
			scan.gather(parent_, nullptr, this);
		}
		std::vector<bool> at_rest(n, false), moved_any(n, false), live(n, false);
		std::vector<Obstacle> in_way;
		bool moved = true;
		int pass = 0;
		for (; pass < kPasses && moved; pass++) {
			moved = false;
			// Down the list, then back up it, and so on: an entry whose
			// space ends at one listed after it (a camera between two
			// handheld windows) waited a pass for that one's new place,
			// and a column of three needed more passes than there are,
			// "its sources did not settle in 4 passes" in the owner's
			// logs (2026-11-18). Each way round, what an entry goes by on
			// either side was worked out just before it.
			for (size_t k = 0; k < n; k++) {
				const size_t i = pass % 2 == 0 ? k : n - 1 - k;
				if (way[i] != Way::Solve)
					continue;
				FitEngine &engine = *entries[i]->engine;
				Prep &p = preps[i];
				scan_of(p.how).obstacles(parent_, p.item, p.s.skip, p.s.skip_rule, &engine, p.how,
							 in_way, &engine.rank());
				bool rest = false;
				engine.observe(in_way, engine.arriving(), pass == 0 ? seconds : 0.0f, rest,
					       &scan_of(p.how).visible_ids());
				if (pass == 0)
					at_rest[i] = rest;
				bool changed = false;
				if (!engine.solve(p, pass == 0 ? seconds : 0.0f, pass == 0, changed)) {
					way[i] = Way::Skip; // no room at all
					continue;
				}
				live[i] = true;
				if (changed) {
					moved = true;
					moved_any[i] = true;
				}
				// Said at once: the entries after it in this pass go by it.
				engine.publish_claim(p);
			}
		}
		for (size_t i = 0; i < n; i++)
			if (moved_any[i] || (way[i] == Way::Solve && !at_rest[i]))
				quiet = false;
		if (moved && pass == kPasses) {
			// Kept as the last pass left them; said in the log, now and then.
			const uint64_t now = os_gettime_ns();
			if (now - unsettled_logged_ > 60000000000ULL || !unsettled_logged_) {
				unsettled_logged_ = now;
				obs_log(LOG_INFO, "Scene Fit '%s': its sources did not settle in %d passes",
					obs_source_get_name(source_), kPasses);
			}
		}
		// At rest: one glance a tick for all of them.
		bool all_rest = true;
		for (size_t i = 0; i < n; i++)
			if (way[i] == Way::Solve && !at_rest[i])
				all_rest = false;
		if (all_rest) {
			for (size_t i = 0; i < n; i++) {
				if (way[i] != Way::Solve)
					continue;
				std::vector<obs_sceneitem_t *> mine;
				for (size_t j = 0; j < n; j++)
					if (way[j] == Way::Solve &&
					    preps[j].how.visible_pixels == preps[i].how.visible_pixels &&
					    preps[j].how.leave_out_shadows == preps[i].how.leave_out_shadows)
						mine.push_back(preps[j].item);
				scan_of(preps[i].how).at_rest(parent_, mine, this, preps[i].how);
				break; // one glance a tick
			}
		}
		// Every entry whose place changed sets off now, from where it is.
		for (size_t i = 0; i < n; i++) {
			if (way[i] != Way::Solve || !live[i])
				continue;
			if (moved_any[i])
				entries[i]->engine->count_glide();
			spaces[i] = entries[i]->engine->advance(preps[i], seconds);
		}
	}
	// D: each put in place.
	for (size_t i = 0; i < n; i++) {
		FitEngine &engine = *entries[i]->engine;
		switch (way[i]) {
		case Way::Skip:
			continue;
		case Way::Hold:
			spaces[i] = engine.current();
			engine.publish_claim(preps[i]);
			break;
		case Way::Place:
			engine.publish_claim(preps[i]);
			break;
		case Way::Solve:
			break; // its claim was said while it was worked out
		}
		engine.apply(preps[i], spaces[i]);
	}
	for (const auto &entry : entries)
		if (entry->engine->settling() || entry->engine->pending())
			quiet = false;
	// Taken after the tick: what it changed itself (a claim said again
	// the same changes nothing) is the next tick's key.
	const RestKey after{scene_revision(), claims_revision(), glance_revision(), shadow_room_revision(),
			    entries_count()};
	quiet_ = quiet && after == key ? quiet_ + 1 : 0;
	rest_key_ = after;
}

// Every item that shows in the scene (in groups too), glanced at as a full
// tick at rest would (`FitScan::at_rest`), held for the call only.
bool add_glanced(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!obs_sceneitem_visible(item) || !source || obs_obj_is_private(source) || is_stand_in(source) ||
	    is_morph_layer(item))
		return true;
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, add_glanced, param);
		return true;
	}
	obs_sceneitem_addref(item);
	static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
	return true;
}

void SceneFit::glance_at_rest()
{
	std::vector<obs_sceneitem_t *> items;
	obs_scene_enum_items(obs_scene_from_source(parent_), add_glanced, &items);
	glance_step(this, parent_, items);
	for (obs_sceneitem_t *item : items)
		obs_sceneitem_release(item);
}

// ---------------------------------------------------------------- the window

// The entries' groups follow the list (and, with All sources, the scene): one
// is added for an entry just added, and the one of an entry taken off goes.
// Only the chosen entry's shows; with the same settings for all, only the
// shared set's, and no Settings for.
void show_chosen(obs_properties_t *top, obs_data_t *settings, const std::vector<std::string> &names)
{
	const bool same = same_for_all(settings);
	std::string chosen = obs_data_get_string(settings, "fit_for");
	if (std::find(names.begin(), names.end(), chosen) == names.end())
		chosen = names.empty() ? std::string() : names.front();
	for (const std::string &name : names)
		if (obs_property_t *group = obs_properties_get(top, (prefix_of(name) + "g").c_str())) {
			obs_property_set_visible(group, !same && name == chosen);
			fit_controls_shown(top, settings, prefix_of(name), fit_list_prefix(name), true);
		}
	if (obs_property_t *box = obs_properties_get(top, "fit_for"))
		obs_property_set_visible(box, !same && !names.empty());
	if (obs_property_t *group = obs_properties_get(top, (std::string(kGroupPrefix) + "g").c_str())) {
		obs_property_set_visible(group, same);
		fit_controls_shown(top, settings, kGroupPrefix, kGroupLists, true);
	}
	const bool all = obs_data_get_int(settings, "which") == (int)Which::AllSources;
	if (obs_property_t *skip = obs_properties_get(top, "skip|alist"))
		obs_property_set_visible(skip, all);
}

void add_entry_group(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings, const std::string &name)
{
	const std::string prefix = prefix_of(name);
	if (obs_properties_get(top, (prefix + "g").c_str()))
		return;
	fit_entry_defaults(settings, prefix);
	obs_source_t *parent = owner ? obs_filter_get_parent(owner) : nullptr;
	obs_properties_t *content = obs_properties_create();
	obs_data_set_default_bool(settings, (prefix + "enabled").c_str(), true);
	obs_property_t *on =
		obs_properties_add_bool(content, (prefix + "enabled").c_str(), obs_module_text("SceneFit.EntryOn"));
	set_tip(on, obs_module_text("SceneFit.EntryOn.Tip"));
	fit_add_controls(content, prefix, fit_list_prefix(name), owner, parent, true);
	obs_properties_add_group(top, (prefix + "g").c_str(), name.c_str(), OBS_GROUP_NORMAL, content);
}

void sync_groups(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings)
{
	const std::vector<std::string> names = fitted_names(owner, settings);
	// Groups of entries no longer listed.
	std::vector<std::string> gone;
	for (obs_property_t *p = obs_properties_first(top); p; obs_property_next(&p)) {
		const std::string name = obs_property_name(p);
		if (name.rfind("fit|", 0) != 0 || name.size() < 6 || name.compare(name.size() - 2, 2, "|g") != 0)
			continue;
		const std::string entry = name.substr(4, name.size() - 6);
		if (std::find(names.begin(), names.end(), entry) == names.end())
			gone.push_back(name);
	}
	for (const std::string &name : gone)
		obs_properties_remove_by_name(top, name.c_str());
	for (const std::string &name : names)
		add_entry_group(top, owner, settings, name);
	if (obs_property_t *box = obs_properties_get(top, "fit_for")) {
		const std::string was = obs_data_get_string(settings, "fit_for");
		obs_property_list_clear(box);
		for (const std::string &name : names)
			obs_property_list_add_string(box, name.c_str(), name.c_str());
		// An entry just added is the one shown.
		if (!names.empty() && std::find(names.begin(), names.end(), was) == names.end())
			obs_data_set_string(settings, "fit_for", names.back().c_str());
	}
	show_chosen(top, settings, names);
}

// "Take over Fit to Space": each enabled Fit to Space of the scene whose
// source is not listed yet becomes an entry, with all its settings and what
// it has written down (the place it is kept round, the size it keeps), and is
// switched off (not deleted: switched on again, it is as it was).
bool take_over(obs_properties_t *top, obs_property_t *, void *data)
{
	auto *self = static_cast<SceneFit *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *scene = self ? self->parent() : nullptr;
	if (!owner || !scene)
		return false;
	struct Found {
		std::vector<obs_source_t *> fits;
	} found;
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (id && std::strcmp(id, kFitItemId) == 0 && obs_source_enabled(filter))
				static_cast<Found *>(param)->fits.push_back(obs_source_get_ref(filter));
		},
		&found);
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> names = pick_list_read(settings, "entries", false);
	const bool had_none = names.empty();
	bool first = true;
	std::vector<std::string> taken;
	for (obs_source_t *filter : found.fits) {
		obs_data_t *theirs = obs_source_get_settings(filter);
		const std::string item = obs_data_get_string(theirs, "item");
		if (!item.empty() && std::find(names.begin(), names.end(), item) == names.end()) {
			names.push_back(item);
			const std::string prefix = prefix_of(item);
			for (const FitKey &key : kFitKeys) {
				const std::string to = prefix + key.name;
				switch (key.kind) {
				case Kind::Int:
					obs_data_set_int(settings, to.c_str(), obs_data_get_int(theirs, key.name));
					break;
				case Kind::Double:
					obs_data_set_double(settings, to.c_str(),
							    obs_data_get_double(theirs, key.name));
					break;
				case Kind::Bool:
					obs_data_set_bool(settings, to.c_str(), obs_data_get_bool(theirs, key.name));
					break;
				case Kind::String:
					obs_data_set_string(settings, to.c_str(),
							    obs_data_get_string(theirs, key.name));
					break;
				}
			}
			// (Read by the filter's own rule: unticked, an old Fit to
			// Space measured by boxes.)
			if (obs_data_has_user_value(theirs, "as_arranged") &&
			    !obs_data_get_bool(theirs, "as_arranged") &&
			    !obs_data_has_user_value(theirs, "visible_pixels"))
				obs_data_set_bool(settings, (prefix + "visible_pixels").c_str(), false);
			const std::string lists = fit_list_prefix(item);
			pick_list_write(settings, (lists + "skip").c_str(), false,
					pick_list_read(theirs, "skip", false));
			pick_list_write(settings, (lists + "share_scenes").c_str(), false,
					pick_list_read(theirs, "share_scenes", false));
			// The overlap switch is the Scene Fit's own, for all: it is
			// taken from the first one taken over into an empty Scene Fit.
			if (first && had_none) {
				obs_data_set_bool(settings, "skip_fill_on", obs_data_get_bool(theirs, "skip_fill_on"));
				// So is "Priority by list order".
				obs_data_set_bool(settings, "list_priority",
						  obs_data_get_bool(theirs, "list_priority"));
			}
			first = false;
			taken.push_back(item);
			obs_source_set_enabled(filter, false);
			obs_log(LOG_INFO, "Scene Fit '%s': took over Fit to Space '%s' (%s)",
				obs_source_get_name(owner), obs_source_get_name(filter), item.c_str());
		}
		obs_data_release(theirs);
		obs_source_release(filter);
	}
	pick_list_write(settings, "entries", false, names);
	// Taken over into an empty Scene Fit, two or more that all fit alike
	// become one group: the same settings for all (each its own copy is
	// kept, for going back). Different ones stay each its own. Into a Scene
	// Fit that already has the same for all, they take its shared set.
	if (had_none && taken.size() >= 2 && !same_for_all(settings)) {
		bool alike = true;
		for (size_t i = 1; i < taken.size() && alike; i++)
			alike = same_look(settings, prefix_of(taken[0]), fit_list_prefix(taken[0]), prefix_of(taken[i]),
					  fit_list_prefix(taken[i]));
		if (alike) {
			obs_data_set_int(settings, "settings_mode", (int)Shared::Same);
			settle_mode(settings, names);
			obs_log(LOG_INFO,
				"Scene Fit '%s': the %zu Fit to Space taken over fit alike: the same settings for all",
				obs_source_get_name(owner), taken.size());
		}
	}
	obs_source_update(owner, settings);
	if (top) {
		pick_list_refresh(top, owner, "entries", false);
		for (const char *list : {"skip", "share_scenes"})
			pick_list_refresh(top, owner, (std::string(kGroupLists) + list).c_str(), false);
		sync_groups(top, owner, settings);
	}
	obs_data_release(settings);
	return true;
}

bool chosen_modified(void *data, obs_properties_t *top, obs_property_t *, obs_data_t *settings)
{
	auto *self = static_cast<SceneFit *>(data);
	show_chosen(top, settings, fitted_names(self ? self->source() : nullptr, settings));
	return true;
}

// Each its own and the same for all: settled at once (so the window shows
// the values the sources now go by), the lists' rows made again.
bool mode_modified(void *data, obs_properties_t *top, obs_property_t *, obs_data_t *settings)
{
	auto *self = static_cast<SceneFit *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	const std::vector<std::string> names = fitted_names(owner, settings);
	if (settle_mode(settings, names) && owner) {
		for (const char *list : {"skip", "share_scenes"}) {
			pick_list_refresh(top, owner, (std::string(kGroupLists) + list).c_str(), false);
			for (const std::string &name : names)
				pick_list_refresh(top, owner, (fit_list_prefix(name) + list).c_str(), false);
		}
	}
	if (owner)
		sync_groups(top, owner, settings);
	else
		show_chosen(top, settings, names);
	return true;
}

// All sources and back: the entries' groups follow.
bool which_modified(void *data, obs_properties_t *top, obs_property_t *, obs_data_t *settings)
{
	auto *self = static_cast<SceneFit *>(data);
	if (self)
		sync_groups(top, self->source(), settings);
	else
		show_chosen(top, settings, pick_list_read(settings, "entries", false));
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneFit *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *parent = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();
	if (parent && obs_source_is_group(parent)) {
		obs_property_t *wrong =
			obs_properties_add_text(props, "on_group", obs_module_text("SceneFit.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(wrong, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(wrong, true);
	}
	obs_property_t *which = obs_properties_add_list(props, "which", obs_module_text("SceneFit.Which"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(which, obs_module_text("SceneFit.Which.All"), (int)Which::AllSources);
	obs_property_list_add_int(which, obs_module_text("SceneFit.Which.Listed"), (int)Which::Chosen);
	set_tip(which, obs_module_text("SceneFit.Which.Tip"));
	obs_property_set_modified_callback2(which, which_modified, self);
	PickList entries;
	entries.setting = "entries";
	entries.title = "SceneFit.Entries";
	entries.add = "SceneFit.AddSource";
	entries.tip = "SceneFit.Entries.Tip";
	entries.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, owner, entries);
	PickList skip;
	skip.setting = "skip";
	skip.title = "SceneFit.Skip";
	skip.add = "SceneFit.AddSource";
	skip.tip = "SceneFit.Skip.Tip";
	skip.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, owner, skip);
	obs_property_t *take =
		obs_properties_add_button2(props, "take_over", obs_module_text("SceneFit.TakeOver"), take_over, self);
	set_tip(take, obs_module_text("SceneFit.TakeOver.Tip"));
	obs_property_t *rule = obs_properties_add_bool(props, "skip_fill_on", obs_module_text("Fit.SkipFillOn"));
	set_tip(rule, obs_module_text("SceneFit.SkipFillOn.Tip"));
	obs_property_set_modified_callback2(rule, chosen_modified, self);
	obs_property_t *by_list = obs_properties_add_bool(props, "list_priority", obs_module_text("Fit.ListPriority"));
	set_tip(by_list, obs_module_text("SceneFit.ListPriority.Tip"));
	obs_property_set_modified_callback2(by_list, chosen_modified, self);
	// Each source its own settings, or one set for all of them.
	obs_property_t *mode = obs_properties_add_list(props, "settings_mode", obs_module_text("SceneFit.Settings"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("SceneFit.Settings.Each"), (int)Shared::Each);
	obs_property_list_add_int(mode, obs_module_text("SceneFit.Settings.Same"), (int)Shared::Same);
	set_tip(mode, obs_module_text("SceneFit.Settings.Tip"));
	{
		obs_properties_t *content = obs_properties_create();
		fit_add_controls(content, kGroupPrefix, kGroupLists, owner, parent, true);
		obs_properties_add_group(props, (std::string(kGroupPrefix) + "g").c_str(),
					 obs_module_text("SceneFit.Settings.Group"), OBS_GROUP_NORMAL, content);
	}
	obs_property_t *chosen = obs_properties_add_list(props, "fit_for", obs_module_text("SceneFit.For"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	set_tip(chosen, obs_module_text("SceneFit.For.Tip"));
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	if (settings) {
		sync_groups(props, owner, settings);
		obs_data_release(settings);
	}
	obs_property_set_modified_callback2(chosen, chosen_modified, self);
	obs_property_set_modified_callback2(mode, mode_modified, self);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	// (Not All sources: every source of a scene would be moved and resized
	// the moment the filter was added, and every filter saved before would
	// change.)
	obs_data_set_default_int(settings, "which", (int)Which::Chosen);
	obs_data_set_default_bool(settings, "skip_fill_on", false);
	obs_data_set_default_bool(settings, "list_priority", false);
	obs_data_set_default_int(settings, "settings_mode", (int)Shared::Each);
	obs_data_set_default_int(settings, "settings_mode_was", (int)Shared::Each);
	fit_entry_defaults(settings, kGroupPrefix);
}

// A source is renamed: its entry, its settings and the lists naming it follow.
void on_source_rename(void *, calldata_t *cd)
{
	const char *was_c = calldata_string(cd, "prev_name");
	const char *now_c = calldata_string(cd, "new_name");
	if (!was_c || !now_c || !*was_c || !*now_c)
		return;
	const std::string was = was_c, now = now_c;
	std::vector<obs_source_t *> owners;
	{
		std::lock_guard lock(g_fits_mutex);
		for (SceneFit *each : g_fits)
			if (obs_source_t *held = obs_source_get_ref(each->source()))
				owners.push_back(held);
	}
	auto move = [](obs_data_t *settings, const std::string &from, const std::string &to) -> bool {
		std::vector<std::string> keys;
		for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
			const char *name = obs_data_item_get_name(item);
			if (name && std::strncmp(name, from.c_str(), from.size()) == 0 &&
			    obs_data_item_has_user_value(item))
				keys.push_back(name);
		}
		for (const std::string &key : keys) {
			const std::string moved = to + key.substr(from.size());
			obs_data_item_t *item = obs_data_item_byname(settings, key.c_str());
			switch (obs_data_item_gettype(item)) {
			case OBS_DATA_STRING:
				obs_data_set_string(settings, moved.c_str(), obs_data_item_get_string(item));
				break;
			case OBS_DATA_NUMBER:
				if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
					obs_data_set_int(settings, moved.c_str(), obs_data_item_get_int(item));
				else
					obs_data_set_double(settings, moved.c_str(), obs_data_item_get_double(item));
				break;
			case OBS_DATA_BOOLEAN:
				obs_data_set_bool(settings, moved.c_str(), obs_data_item_get_bool(item));
				break;
			case OBS_DATA_OBJECT: {
				obs_data_t *object = obs_data_item_get_obj(item);
				obs_data_set_obj(settings, moved.c_str(), object);
				obs_data_release(object);
				break;
			}
			case OBS_DATA_ARRAY: {
				obs_data_array_t *array = obs_data_item_get_array(item);
				obs_data_set_array(settings, moved.c_str(), array);
				obs_data_array_release(array);
				break;
			}
			default:
				break;
			}
			obs_data_item_release(&item);
			obs_data_erase(settings, key.c_str());
		}
		return !keys.empty();
	};
	for (obs_source_t *owner : owners) {
		obs_data_t *settings = obs_source_get_settings(owner);
		std::vector<std::string> names = pick_list_read(settings, "entries", false);
		bool changed = false;
		for (std::string &name : names) {
			if (name == was) {
				name = now;
				changed = true;
			}
		}
		const bool all = obs_data_get_int(settings, "which") == (int)Which::AllSources;
		if (changed || all) {
			// (With All sources an unlisted source has its settings there
			// too; nothing is moved for a source that has none.)
			bool moved = move(settings, prefix_of(was), prefix_of(now));
			moved = move(settings, fit_list_prefix(was), fit_list_prefix(now)) || moved;
			changed = changed || moved;
			for (const char *key : {"keep_item", "orig_item", "home_item"}) {
				const std::string full = prefix_of(now) + key;
				if (obs_data_get_string(settings, full.c_str()) == was)
					obs_data_set_string(settings, full.c_str(), now.c_str());
			}
			if (changed)
				pick_list_write(settings, "entries", false, names);
			if (obs_data_get_string(settings, "fit_for") == was) {
				obs_data_set_string(settings, "fit_for", now.c_str());
				changed = true;
			}
		}
		// The ignored sources.
		{
			std::vector<std::string> skip = pick_list_read(settings, "skip", false);
			bool named = false;
			for (std::string &one : skip)
				if (one == was) {
					one = now;
					named = true;
				}
			if (named) {
				pick_list_write(settings, "skip", false, skip);
				changed = true;
			}
		}
		// Every entry's list of ignored sources, and the shared set's.
		std::vector<std::string> lists;
		for (const std::string &name : fitted_names(owner, settings))
			lists.push_back(fit_list_prefix(name) + "skip");
		lists.push_back(std::string(kGroupLists) + "skip");
		for (const std::string &setting : lists) {
			std::vector<std::string> skip = pick_list_read(settings, setting.c_str(), false);
			bool named = false;
			for (std::string &one : skip)
				if (one == was) {
					one = now;
					named = true;
				}
			if (named) {
				pick_list_write(settings, setting.c_str(), false, skip);
				changed = true;
			}
		}
		if (changed)
			obs_source_update(owner, settings);
		obs_data_release(settings);
		obs_source_release(owner);
	}
}

} // namespace

bool scene_fit_covers(obs_source_t *filter, const std::string &source)
{
	if (!filter || source.empty())
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);
	bool covers = has(pick_list_read(settings, "entries", false), source);
	if (!covers && obs_data_get_int(settings, "which") == (int)Which::AllSources)
		covers = has(unlisted_sources(filter, obs_filter_get_parent(filter), settings), source);
	obs_data_release(settings);
	return covers;
}

bool scene_fit_entry_on(obs_source_t *filter, const std::string &source)
{
	obs_data_t *settings = filter ? obs_source_get_settings(filter) : nullptr;
	const bool on = settings && entry_on(settings, source);
	obs_data_release(settings);
	return on;
}

void scene_fit_set_entry_on(obs_source_t *filter, const std::string &source, bool on)
{
	if (!filter || source.empty())
		return;
	obs_data_t *change = obs_data_create();
	obs_data_set_bool(change, (prefix_of(source) + "enabled").c_str(), on);
	obs_source_update(filter, change);
	obs_data_release(change);
}

void scene_fit_stop()
{
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
}

void register_scene_fit()
{
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	pick_list_on_change_for(kSceneFitId, sync_groups);
	obs_source_info info = {};
	info.id = kSceneFitId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneFit.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneFit(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneFit *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneFit *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneFit *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		skip_filter_input(static_cast<SceneFit *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<SceneFit *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneFit *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
