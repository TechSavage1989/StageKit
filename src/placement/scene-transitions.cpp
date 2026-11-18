/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Transitions (see the header).
//
// What it rests on:
// - It writes item transitions only from its own tick (OBS's video thread,
//   where the other StageKit filters act too: two threads setting one item's
//   transitions at once can each let go of the other's), only once
//   `scenes_loaded()`, and only on events: created, switched on, settings
//   changed, an item added, a covered item shown or hidden, a nearest-edge
//   item at rest for a quarter second. Nothing at rest costs more than one
//   look at two flags.
// - It writes only where the item differs from what it should carry
//   (`item_change_matches`), so a collection loaded again writes nothing:
//   OBS keeps item transitions in the collection.
// - Items are known by their id in their scene or group, never held past one
//   call.
// - It passes over items another filter owns (a Standby standing in), private
//   sources (stand-ins, the item Morph's layer), "Ignored sources", items a
//   Show/Hide Animation animates, and items in groups unless asked.
// - After another filter's own choice, the scene's transitions are put back
//   once that change has had time to end, never part-way through it.
// - With "Override other filters" there is no other filter's choice: every
//   call that sets an item's transitions (`resolve_item_change`,
//   `set_item_transitions`) asks this filter first and takes its choice for
//   a covered item, so the put-back finds the item as it should be and
//   writes nothing.

#include "scene-transitions.hpp"
#include "scene-tools.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/filter-order.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <callback/proc.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

// Which sources it sets. Saved by number: new ones go at the end.
enum class Cover : int {
	All = 0,    // every source in the scene, listed ones with their own settings if they have some
	Listed = 1, // only the listed sources
};

constexpr uint64_t kAfterChangeNs = 100'000'000; // a change is over its length plus this
constexpr uint64_t kAtRestNs = 250'000'000;      // a nearest-edge source still for this long
// A source just added, or a scene just met, is looked at only after this:
// OBS adds a scene's items first and sets each item's saved transitions after,
// so acting at once could write a transition that was about to be loaded.
constexpr uint64_t kAddedNs = 100'000'000;
constexpr float kGroupsEvery = 2.0f; // with "Sources in groups": groups found again

// What the settings say, read once in update and swapped whole.
struct Look {
	Cover cover = Cover::All;
	std::set<std::string> listed, except;
	bool in_groups = false;
	ItemChange usual;
	std::map<std::string, ItemChange> own;
	bool any_nearest = false; // a way off the nearest edge: positions matter
	bool overrides = false;   // "Override other filters"
};

// "As the scene sets" cannot be the scene's own answer: a fade of that way's
// length in its place (only reachable through hand-edited settings).
void no_scene_kind(ItemChange &change)
{
	for (ItemWay *way : {&change.show, &change.hide})
		if (way->kind == kTransitionScene)
			*way = ItemWay{kTransitionFade, way->ms, {}};
}

bool off_nearest(const ItemWay &way)
{
	if ((way.kind != "slide_transition" && way.kind != "swipe_transition") || way.own.empty())
		return false;
	obs_data_t *own = obs_data_create_from_json(way.own.c_str());
	const bool nearest = own && strcmp(obs_data_get_string(own, "direction"), "nearest") == 0;
	obs_data_release(own);
	return nearest;
}

std::atomic<long long> g_writes{0}; // items written, for the tests
// Counts the filters with "Override other filters" ticked: while none has,
// the other filters' changes are not looked up at all.
void count_override(bool was, bool now)
{
	if (was != now)
		item_transitions_scene_overrides(now ? 1 : -1);
}

struct Key {
	uintptr_t holder; // the scene or group the item is straight in
	int64_t id;
	bool operator<(const Key &o) const { return holder != o.holder ? holder < o.holder : id < o.id; }
};

class SceneTransitions;
std::mutex g_mutex;
std::vector<SceneTransitions *> g_all;

class SceneTransitions {
public:
	SceneTransitions(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", on_enable, this);
		phase_ = float((reinterpret_cast<uintptr_t>(this) >> 4) % 97) / 97.0f * kGroupsEvery; // TODO(stagger)
		std::lock_guard lock(g_mutex);
		g_all.push_back(this);
	}

	~SceneTransitions()
	{
		{
			std::lock_guard lock(g_mutex);
			g_all.erase(std::remove(g_all.begin(), g_all.end(), this), g_all.end());
		}
		count_override(look()->overrides, false);
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", on_enable, this);
		detach();
	}

	obs_source_t *source() const { return source_; }

	std::shared_ptr<const Look> look() const
	{
		std::lock_guard lock(mutex_);
		return look_;
	}

	std::set<std::string> animated() const
	{
		std::lock_guard lock(mutex_);
		return animated_;
	}

	void update(obs_data_t *settings)
	{
		auto look = std::make_shared<Look>();
		look->cover = (Cover)obs_data_get_int(settings, "cover");
		for (const std::string &name : pick_list_read(settings, "sources", false))
			look->listed.insert(name);
		for (const std::string &name : pick_list_read(settings, "except", false))
			look->except.insert(name);
		look->in_groups = obs_data_get_bool(settings, "in_groups");
		look->overrides = obs_data_get_bool(settings, "override_others");
		look->usual = item_change_read(settings);
		no_scene_kind(look->usual);
		for (const std::string &name : look->listed) {
			ItemChange own = item_change_read(settings, name.c_str());
			no_scene_kind(own);
			look->own[name] = own;
		}
		look->any_nearest = off_nearest(look->usual.show) || off_nearest(look->usual.hide);
		for (const auto &[name, own] : look->own)
			look->any_nearest = look->any_nearest || off_nearest(own.show) || off_nearest(own.hide);
		{
			std::lock_guard lock(mutex_);
			count_override(look_ && look_->overrides, look->overrides);
			look_ = std::move(look);
		}
		all_due_ = true;
	}

	// Whether this filter sets the item, and how. `in_group`: the item is in
	// a group of the filter's scene.
	static bool covers(const Look &look, const std::set<std::string> &animated, obs_sceneitem_t *item,
			   bool in_group, ItemChange &out)
	{
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = source ? obs_source_get_name(source) : nullptr;
		if (!name || obs_obj_is_private(source) || is_morph_layer(item) || (in_group && !look.in_groups))
			return false;
		if (look.except.count(name) || animated.count(name) ||
		    (look.cover == Cover::Listed && !look.listed.count(name)))
			return false;
		const auto own = look.own.find(name);
		out = own == look.own.end() ? look.usual : own->second;
		return true;
	}

	void tick(float seconds)
	{
		if (!scenes_loaded()) {
			all_due_ = true;
			return;
		}
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (parent != connected_) {
			attach(parent);
			all_due_ = true;
			settle_until_ = os_gettime_ns() + kAddedNs;
		}
		const std::shared_ptr<const Look> look = this->look();
		if (look->in_groups) {
			phase_ += seconds;
			if (phase_ >= kGroupsEvery) {
				phase_ = 0.0f;
				find_groups();
			}
		} else if (!groups_.empty()) {
			let_go_of_groups();
		}
		if (!all_due_ && !any_due_)
			return;
		if (!parent || !obs_source_enabled(source_) || !obs_scene_from_source(parent) ||
		    obs_source_is_group(parent))
			return; // (kept due: switched on again, it acts)
		if (first_here(parent) != source_) {
			// Another Scene Transitions on this scene comes first and is the one used.
			all_due_ = false;
			std::lock_guard lock(due_mutex_);
			due_.clear();
			any_due_ = false;
			return;
		}
		const uint64_t now = os_gettime_ns();
		// What has come due, and what is still waiting.
		std::vector<Key> ready;
		std::set<Key> waiting;
		{
			std::lock_guard lock(due_mutex_);
			for (auto it = due_.begin(); it != due_.end();) {
				if (it->second <= now) {
					ready.push_back(it->first);
					it = due_.erase(it);
				} else {
					waiting.insert(it->first);
					++it;
				}
			}
			any_due_ = !due_.empty();
		}
		// Where each due item lives.
		std::map<uintptr_t, obs_source_t *> holders;
		holders[reinterpret_cast<uintptr_t>(parent)] = parent;
		std::vector<obs_source_t *> groups = held_groups();
		for (obs_source_t *group : groups)
			holders[reinterpret_cast<uintptr_t>(group)] = group;

		if (now >= settle_until_ && all_due_.exchange(false))
			full_pass(parent, *look, waiting);
		const std::set<std::string> animated = this->animated();
		for (const Key &key : ready) {
			const auto holder = holders.find(key.holder);
			if (holder == holders.end())
				continue;
			obs_scene_t *scene = obs_source_is_group(holder->second)
						     ? obs_group_from_source(holder->second)
						     : obs_scene_from_source(holder->second);
			obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, key.id) : nullptr;
			if (item)
				put_right(item, *look, animated, holder->second != parent, parent);
		}
		for (obs_source_t *group : groups)
			obs_source_release(group);
	}

	// Writes what the item should carry, where it does not already.
	void put_right(obs_sceneitem_t *item, const Look &look, const std::set<std::string> &animated, bool in_group,
		       obs_source_t *scene)
	{
		ItemChange change;
		if (item_transitions_owned(item) || !covers(look, animated, item, in_group, change))
			return;
		if (item_change_matches(item, change, scene))
			return;
		apply_item_change(item, change, scene);
		g_writes++;
	}

	void full_pass(obs_source_t *parent, const Look &look, const std::set<Key> &waiting)
	{
		obs_scene_t *scene = obs_scene_from_source(parent);
		// Items a Show/Hide Animation animates (it re-shows an item to animate
		// its hide: a fade on top would double), found here, not per tick.
		std::set<std::string> animated;
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_unversioned_id(filter);
				if (!id || strcmp(id, kShowHideId) != 0 || !obs_source_enabled(filter))
					return;
				obs_data_t *settings = obs_source_get_settings(filter);
				const char *item = obs_data_get_string(settings, "item");
				if (item && *item)
					static_cast<std::set<std::string> *>(param)->insert(item);
				obs_data_release(settings);
			},
			&animated);
		{
			std::lock_guard lock(mutex_);
			animated_ = animated;
		}
		struct Found {
			std::vector<std::pair<obs_sceneitem_t *, bool>> items; // and whether in a group
			bool in_groups;
		} found{{}, look.in_groups};
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *f = static_cast<Found *>(param);
				obs_sceneitem_addref(item);
				f->items.push_back({item, false});
				if (f->in_groups && obs_sceneitem_is_group(item))
					obs_sceneitem_group_enum_items(
						item,
						[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
							obs_sceneitem_addref(inner);
							static_cast<Found *>(p)->items.push_back({inner, true});
							return true;
						},
						f);
				return true;
			},
			&found);
		for (const auto &[item, in_group] : found.items) {
			const Key key{reinterpret_cast<uintptr_t>(obs_scene_get_source(obs_sceneitem_get_scene(item))),
				      obs_sceneitem_get_id(item)};
			// One with a change of its own still under way is put right when
			// that has ended.
			if (!waiting.count(key))
				put_right(item, look, animated, in_group, parent);
			obs_sceneitem_release(item);
		}
	}

	// `parent`: the scene it is being taken off, from `filter_remove`. A scene
	// being destroyed has no references left, so the weak one cannot reach it
	// any more, yet it still signals each item it removes after this filter
	// is gone: it must be let go of by the pointer it came with.
	void detach(obs_source_t *parent = nullptr)
	{
		std::lock_guard lock(conn_mutex_);
		if (obs_source_t *scene = obs_weak_source_get_source(scene_)) {
			disconnect(scene);
			obs_source_release(scene);
		} else if (parent && parent == connected_.load()) {
			disconnect(parent);
		}
		obs_weak_source_release(scene_);
		scene_ = nullptr;
		connected_ = nullptr;
		for (obs_weak_source_t *weak : groups_) {
			if (obs_source_t *group = obs_weak_source_get_source(weak)) {
				disconnect(group);
				obs_source_release(group);
			}
			obs_weak_source_release(weak);
		}
		groups_.clear();
	}

private:
	void attach(obs_source_t *parent)
	{
		detach();
		std::lock_guard lock(conn_mutex_);
		connected_ = parent;
		if (!parent || !obs_scene_from_source(parent) || obs_source_is_group(parent))
			return;
		scene_ = obs_source_get_weak_source(parent);
		connect(parent);
	}

	void connect(obs_source_t *holder)
	{
		signal_handler_t *sh = obs_source_get_signal_handler(holder);
		signal_handler_connect(sh, "item_add", on_item_add, this);
		signal_handler_connect(sh, "item_remove", on_item_remove, this);
		signal_handler_connect(sh, "item_visible", on_item_visible, this);
		signal_handler_connect(sh, "item_transform", on_item_transform, this);
	}

	void disconnect(obs_source_t *holder)
	{
		signal_handler_t *sh = obs_source_get_signal_handler(holder);
		signal_handler_disconnect(sh, "item_add", on_item_add, this);
		signal_handler_disconnect(sh, "item_remove", on_item_remove, this);
		signal_handler_disconnect(sh, "item_visible", on_item_visible, this);
		signal_handler_disconnect(sh, "item_transform", on_item_transform, this);
	}

	// The groups in the scene, each listened to; gone ones let go of.
	void find_groups()
	{
		obs_source_t *parent = obs_weak_source_get_source(scene_);
		if (!parent)
			return;
		std::vector<obs_source_t *> now;
		obs_scene_enum_items(
			obs_scene_from_source(parent),
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				if (obs_sceneitem_is_group(item))
					static_cast<std::vector<obs_source_t *> *>(param)->push_back(
						obs_source_get_ref(obs_sceneitem_get_source(item)));
				return true;
			},
			&now);
		obs_source_release(parent);
		std::lock_guard lock(conn_mutex_);
		std::vector<obs_weak_source_t *> kept;
		for (obs_weak_source_t *weak : groups_) {
			obs_source_t *group = obs_weak_source_get_source(weak);
			const auto still = std::find(now.begin(), now.end(), group);
			if (group && still != now.end()) {
				kept.push_back(weak);
				obs_source_release(*still);
				now.erase(still);
			} else {
				if (group)
					disconnect(group);
				obs_weak_source_release(weak);
			}
			obs_source_release(group);
		}
		for (obs_source_t *group : now) {
			if (!group)
				continue;
			connect(group);
			kept.push_back(obs_source_get_weak_source(group));
			obs_source_release(group);
			all_due_ = true; // its items are new to this filter
		}
		groups_ = std::move(kept);
	}

	void let_go_of_groups()
	{
		std::lock_guard lock(conn_mutex_);
		for (obs_weak_source_t *weak : groups_) {
			if (obs_source_t *group = obs_weak_source_get_source(weak)) {
				disconnect(group);
				obs_source_release(group);
			}
			obs_weak_source_release(weak);
		}
		groups_.clear();
	}

	std::vector<obs_source_t *> held_groups()
	{
		std::lock_guard lock(conn_mutex_);
		std::vector<obs_source_t *> out;
		for (obs_weak_source_t *weak : groups_)
			if (obs_source_t *group = obs_weak_source_get_source(weak))
				out.push_back(group);
		return out;
	}

	// The first enabled Scene Transitions in the scene's filter list.
	static obs_source_t *first_here(obs_source_t *parent) { return first_filter(parent, kSceneTransitionsId); }

	void due_at(obs_sceneitem_t *item, uint64_t at, bool later_only)
	{
		if (!item)
			return;
		const Key key{reinterpret_cast<uintptr_t>(obs_scene_get_source(obs_sceneitem_get_scene(item))),
			      obs_sceneitem_get_id(item)};
		std::lock_guard lock(due_mutex_);
		auto [it, made] = due_.try_emplace(key, at);
		if (!made && (!later_only || at > it->second))
			it->second = later_only ? std::max(it->second, at) : at;
		any_due_ = true;
	}

	static obs_sceneitem_t *item_of(calldata_t *cd)
	{
		return static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	}

	// Whether a signal still reaches a live filter (with g_mutex held): a
	// group or scene being destroyed signals its items' removal after the
	// filter that listened may be gone, and can no longer be let go of.
	static bool live(void *data)
	{
		return std::find(g_all.begin(), g_all.end(), static_cast<SceneTransitions *>(data)) != g_all.end();
	}

	static void on_item_add(void *data, calldata_t *cd)
	{
		std::lock_guard alive(g_mutex);
		if (!live(data))
			return;
		static_cast<SceneTransitions *>(data)->due_at(item_of(cd), os_gettime_ns() + kAddedNs, false);
	}

	static void on_item_remove(void *data, calldata_t *cd)
	{
		std::lock_guard alive(g_mutex);
		if (!live(data))
			return;
		auto *self = static_cast<SceneTransitions *>(data);
		obs_sceneitem_t *item = item_of(cd);
		if (!item)
			return;
		const Key key{reinterpret_cast<uintptr_t>(obs_scene_get_source(obs_sceneitem_get_scene(item))),
			      obs_sceneitem_get_id(item)};
		std::lock_guard lock(self->due_mutex_);
		self->due_.erase(key);
	}

	// Shown or hidden: once that change has had time to end, the item gets
	// the scene's transitions back (another filter may have set its own for
	// the change), and a nearest-edge way is turned for the next change.
	static void on_item_visible(void *data, calldata_t *cd)
	{
		std::lock_guard alive(g_mutex);
		if (!live(data))
			return;
		obs_sceneitem_t *item = item_of(cd);
		if (!item)
			return;
		const bool visible = calldata_bool(cd, "visible");
		const uint64_t ms = obs_sceneitem_get_transition(item, visible)
					    ? obs_sceneitem_get_transition_duration(item, visible)
					    : 0;
		static_cast<SceneTransitions *>(data)->due_at(item, os_gettime_ns() + ms * 1'000'000 + kAfterChangeNs,
							      false);
	}

	// Moved (each frame of a glide): only a nearest-edge way cares, once the
	// item has been still a moment.
	static void on_item_transform(void *data, calldata_t *cd)
	{
		std::lock_guard alive(g_mutex);
		if (!live(data))
			return;
		auto *self = static_cast<SceneTransitions *>(data);
		if (!self->look()->any_nearest)
			return;
		self->due_at(item_of(cd), os_gettime_ns() + kAtRestNs, true);
	}

	static void on_enable(void *data, calldata_t *cd)
	{
		if (calldata_bool(cd, "enabled"))
			static_cast<SceneTransitions *>(data)->all_due_ = true;
	}

	obs_source_t *source_;
	mutable std::mutex mutex_; // guards look_ and animated_ (read by the lookup, any thread)
	std::shared_ptr<const Look> look_;
	std::set<std::string> animated_;

	std::mutex conn_mutex_; // guards what is connected to
	obs_weak_source_t *scene_ = nullptr;
	std::atomic<obs_source_t *> connected_{nullptr}; // (compared only)
	std::vector<obs_weak_source_t *> groups_;
	float phase_ = 0.0f;

	std::atomic<bool> all_due_{true};
	uint64_t settle_until_ = 0; // no full pass before this (a scene just met), tick thread only
	std::atomic<bool> any_due_{false};
	std::mutex due_mutex_;
	std::map<Key, uint64_t> due_; // item -> when to look at it
};

bool lookup(obs_source_t *scene, obs_sceneitem_t *item, ItemChange &out, bool &overrides)
{
	overrides = false;
	if (!scene || !item)
		return false;
	obs_source_t *holder = obs_scene_get_source(obs_sceneitem_get_scene(item));
	const bool in_group = holder && obs_source_is_group(holder);
	// The scene's filters: a reference to each, and what it says, copied out.
	struct Candidate {
		obs_source_t *filter, *parent;
		std::shared_ptr<const Look> look;
		std::set<std::string> animated;
	};
	std::vector<Candidate> candidates;
	{
		std::lock_guard lock(g_mutex);
		for (SceneTransitions *each : g_all) {
			if (!obs_source_enabled(each->source()))
				continue;
			obs_source_t *parent = obs_filter_get_parent(each->source());
			if (!parent || (parent != scene && !(in_group && scene == holder)))
				continue;
			if (obs_source_t *held = obs_source_get_ref(each->source()))
				candidates.push_back({held, parent, each->look(), each->animated()});
		}
	}
	// An item in a group asked about with the group as its scene: the
	// scene holding the group is looked for among the filters' scenes.
	if (in_group && scene == holder) {
		const char *group_name = obs_source_get_name(holder);
		for (Candidate &c : candidates) {
			obs_sceneitem_t *group_item =
				c.parent && group_name
					? obs_scene_find_source(obs_scene_from_source(c.parent), group_name)
					: nullptr;
			if (!group_item || obs_sceneitem_get_source(group_item) != holder) {
				obs_source_release(c.filter);
				c.filter = nullptr;
			}
		}
		candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
						[](const Candidate &c) { return !c.filter; }),
				 candidates.end());
	}
	// The first in its scene's filter list is the one used.
	const Candidate *used = nullptr;
	int best = 0;
	for (const Candidate &c : candidates) {
		const int at = applied_index(c.parent, c.filter);
		if (at >= 0 && (!used || at < best)) {
			used = &c;
			best = at;
		}
	}
	const bool found = used && SceneTransitions::covers(*used->look, used->animated, item, in_group, out);
	overrides = found && used->look->overrides;
	for (const Candidate &c : candidates)
		obs_source_release(c.filter);
	return found;
}

// A source renamed: its name follows in both lists and in its own settings.
void rename_in(obs_source_t *filter, const char *was, const char *now)
{
	obs_data_t *settings = obs_source_get_settings(filter);
	bool changed = false;
	for (const char *list : {"sources", "except"}) {
		std::vector<std::string> names = pick_list_read(settings, list, false);
		bool here = false;
		for (std::string &name : names)
			if (name == was) {
				name = now;
				here = true;
			}
		if (here) {
			pick_list_write(settings, list, false, names);
			changed = true;
		}
	}
	const std::string from = std::string("for|") + was + "|", to = std::string("for|") + now + "|";
	std::vector<std::string> keys;
	for (obs_data_item_t *it = obs_data_first(settings); it; obs_data_item_next(&it)) {
		const char *name = obs_data_item_get_name(it);
		if (name && strncmp(name, from.c_str(), from.size()) == 0 && obs_data_item_has_user_value(it))
			keys.push_back(name);
	}
	for (const std::string &old_key : keys) {
		const std::string new_key = to + old_key.substr(from.size());
		obs_data_item_t *it = obs_data_item_byname(settings, old_key.c_str());
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING:
			obs_data_set_string(settings, new_key.c_str(), obs_data_item_get_string(it));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(settings, new_key.c_str(), obs_data_item_get_bool(it));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_DOUBLE)
				obs_data_set_double(settings, new_key.c_str(), obs_data_item_get_double(it));
			else
				obs_data_set_int(settings, new_key.c_str(), obs_data_item_get_int(it));
			break;
		default:
			break;
		}
		obs_data_item_release(&it);
		obs_data_erase(settings, old_key.c_str());
		changed = true;
	}
	if (strcmp(obs_data_get_string(settings, "change|for"), was) == 0) {
		obs_data_set_string(settings, "change|for", now);
		changed = true;
	}
	if (changed)
		obs_source_update(filter, settings);
	obs_data_release(settings);
}

void on_source_rename(void *, calldata_t *cd)
{
	const char *was = calldata_string(cd, "prev_name");
	const char *now = calldata_string(cd, "new_name");
	if (!was || !now || !*was || !*now)
		return;
	std::vector<obs_source_t *> filters;
	{
		std::lock_guard lock(g_mutex);
		for (SceneTransitions *each : g_all)
			if (obs_source_t *held = obs_source_get_ref(each->source()))
				filters.push_back(held);
	}
	for (obs_source_t *filter : filters) {
		rename_in(filter, was, now);
		obs_source_release(filter);
	}
}

// The scene's sources, for the two lists (and those in its groups).
void scene_source_names(obs_source_t *parent, std::vector<std::string> &names)
{
	obs_scene_t *scene = parent ? obs_scene_from_source(parent) : nullptr;
	if (!scene)
		return;
	auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *list = static_cast<std::vector<std::string> *>(param);
		obs_source_t *source = obs_sceneitem_get_source(item);
		const char *name = source ? obs_source_get_name(source) : nullptr;
		if (name && *name && !obs_obj_is_private(source) && !is_morph_layer(item) &&
		    std::find(list->begin(), list->end(), name) == list->end())
			list->push_back(name);
		return true;
	};
	struct Ctx {
		std::vector<std::string> *names;
		bool (*add)(obs_scene_t *, obs_sceneitem_t *, void *);
	} ctx{&names, add};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *s, obs_sceneitem_t *item, void *param) {
			auto *c = static_cast<Ctx *>(param);
			c->add(s, item, c->names);
			if (obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(item, c->add, c->names);
			return true;
		},
		&ctx);
}

// The list's heading and tip say what it is for under each choice of
// "Sources covered": with All sources it only gives sources settings of
// their own; with Chosen sources it is what is covered. The owner, of the
// one wording for both: "this tooltip makes no sense."
const char *sources_title(Cover cover)
{
	return obs_module_text(cover == Cover::Listed ? "SceneTrans.Chosen" : "SceneTrans.Sources");
}

const char *sources_tip(Cover cover)
{
	return obs_module_text(cover == Cover::Listed ? "SceneTrans.Chosen.Tip" : "SceneTrans.Sources.Tip");
}

// "Sources covered" changed: the list's heading and tip follow. Only text
// changes, so nothing is taken off while OBS goes through the controls.
bool cover_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const Cover cover = (Cover)obs_data_get_int(settings, "cover");
	if (obs_property_t *group = obs_properties_get(props, "sources|alist"))
		obs_property_set_description(group, sources_title(cover));
	if (obs_property_t *pick = obs_properties_get(props, "sources|apick"))
		set_tip(pick, sources_tip(cover));
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneTransitions *>(data);
	obs_source_t *source = self ? self->source() : nullptr;
	obs_source_t *parent = source ? obs_filter_get_parent(source) : nullptr;
	obs_properties_t *props = obs_properties_create();

	if (parent && (!obs_scene_from_source(parent) || obs_source_is_group(parent))) {
		obs_property_t *not_scene = obs_properties_add_text(
			props, "not_scene", obs_module_text("SceneTrans.NotScene"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(not_scene, true);
	} else if (parent && first_filter(parent, kSceneTransitionsId) &&
		   first_filter(parent, kSceneTransitionsId) != source) {
		obs_property_t *second =
			obs_properties_add_text(props, "second", obs_module_text("SceneTrans.Second"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(second, true);
	}

	obs_property_t *cover = obs_properties_add_list(props, "cover", obs_module_text("SceneTrans.Cover"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(cover, obs_module_text("SceneTrans.Cover.All"), (int)Cover::All);
	obs_property_list_add_int(cover, obs_module_text("SceneTrans.Cover.Listed"), (int)Cover::Listed);
	set_tip(cover, obs_module_text("SceneTrans.Cover.Tip"));
	obs_property_set_modified_callback(cover, cover_changed);

	obs_data_t *now = source ? obs_source_get_settings(source) : nullptr;
	const Cover covered = now ? (Cover)obs_data_get_int(now, "cover") : Cover::All;
	obs_data_release(now);
	PickList sources;
	sources.setting = "sources";
	sources.title = covered == Cover::Listed ? "SceneTrans.Chosen" : "SceneTrans.Sources";
	sources.title_text = sources_title(covered);
	sources.add = "SceneTrans.AddSource";
	sources.tip = covered == Cover::Listed ? "SceneTrans.Chosen.Tip" : "SceneTrans.Sources.Tip";
	sources.choices = [parent](std::vector<std::string> &names) {
		scene_source_names(parent, names);
	};
	pick_list_add(props, source, sources);

	PickList except;
	except.setting = "except";
	except.title = "SceneTrans.Except";
	except.add = "SceneTrans.AddSource";
	except.tip = "SceneTrans.Except.Tip";
	except.choices = [parent](std::vector<std::string> &names) {
		scene_source_names(parent, names);
	};
	pick_list_add(props, source, except);

	obs_property_t *in_groups = obs_properties_add_bool(props, "in_groups", obs_module_text("SceneTrans.InGroups"));
	set_tip(in_groups, obs_module_text("SceneTrans.InGroups.Tip"));

	obs_property_t *overrides =
		obs_properties_add_bool(props, "override_others", obs_module_text("SceneTrans.Override"));
	set_tip(overrides, obs_module_text("SceneTrans.Override.Tip"));

	add_item_change_properties(props, source, "sources");
	return props;
}

} // namespace

bool scene_transition_for(obs_source_t *scene, obs_sceneitem_t *item, ItemChange &out)
{
	bool overrides = false;
	return lookup(scene, item, out, overrides);
}

void scene_transitions_stop()
{
	item_transitions_scene(nullptr);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
}

void register_scene_transitions()
{
	item_transitions_scene(&lookup);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	// Let go before OBS takes its scenes apart.
	obs_frontend_add_event_callback(
		[](obs_frontend_event event, void *) {
			if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN || event == OBS_FRONTEND_EVENT_EXIT)
				scene_transitions_stop();
		},
		nullptr);
	static bool test_call = false;
	if (!test_call) {
		test_call = true;
		// For the tests: how many items have been written so far.
		proc_handler_add(
			obs_get_proc_handler(), "void stagekit_scene_transitions_writes(out int writes)",
			[](void *, calldata_t *cd) { calldata_set_int(cd, "writes", g_writes.load()); }, nullptr);
	}

	obs_source_info info = {};
	info.id = kSceneTransitionsId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneTrans.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneTransitions(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<SceneTransitions *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneTransitions *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "cover", (int)Cover::All);
		obs_data_set_default_bool(settings, "in_groups", false);
		obs_data_set_default_bool(settings, "override_others", false);
		item_change_defaults(settings);
		// Its own lists have no "As the scene sets": it is what that asks.
		obs_data_set_default_string(settings, "transition", kTransitionFade);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneTransitions *>(data)->tick(seconds);
	};
	// It only sets how sources show and hide: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<SceneTransitions *>(data)->source());
	};
	info.filter_remove = [](void *data, obs_source_t *parent) {
		static_cast<SceneTransitions *>(data)->detach(parent);
	};
	register_timed(&info);
}

} // namespace stagekit
