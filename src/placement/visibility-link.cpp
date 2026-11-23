/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Visibility Link: a filter you add to a scene. It names a set of the
// scene's items and ties their eye icons together, one of two ways:
//
//   Show only one   showing one of them hides the others (a camera frame
//                   per console, one of several overlays)
//   Together        showing or hiding one does the same to the rest (a
//                   camera with its frame and its label)
//   Follow sources  one or more lead sources lead the listed items (the
//                   followers): while any lead is shown (or, by choice, all
//                   of them) the followers are shown (a Show in Scenes
//                   switch that is off does not stop it: a switch is a
//                   command, not a hold); once the leads say hidden, they
//                   are hidden and kept hidden. A change to a follower moves
//                   nothing else. One scene with several modes (a title and
//                   its extras each) uses this, so a camera switch that acts
//                   in every scene shows the camera only in the mode
//                   showing; so does a chat column that follows every
//                   capture of a layout scene. Saved without
//                   lead sources (the first build, "Follow the first
//                   source"), the first listed item is the one lead.
//
// "Lock hidden sources" locks each listed item while it is hidden and unlocks
// it when it is shown: OBS's preview lets a click select a hidden item, and
// with several captures stacked in one scene a click would pick a hidden one.
//
// It can also reach into the other scenes: sources listed under "Also in
// other scenes" are shown and hidden with the set wherever they are, and
// showing or hiding one of them anywhere does the same here. For that it
// listens to every scene, not only its own.
//
// Whatever shows or hides an item counts: the eye icon, a Stream Deck OBS
// plugin "Source" key, a hotkey, obs-websocket. It replaces the macros that
// watched one item and switched the others.

#include "visibility-link.hpp"
#include "../common/filter-input.hpp"
#include "../common/timing.hpp"

#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/owner-ref.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-rev.hpp"
#include "../common/scene-util.hpp"
#include "../common/tooltip.hpp"
#include "../controller/views-dock.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

enum class LinkMode : int {
	OnlyOne = 0, // saved: append only
	Together = 1,
	Follow = 2, // the lead sources lead the rest (no lead sources: the first listed)
};

// Follow sources: when the followers show. Saved: append only.
enum class LeadRule : int {
	Any = 0, // any lead shown
	All = 1, // every lead shown
};

class VisibilityLink;

// Every Link Visibility there is, so Show in Scenes can ask which items a
// mode that is not showing holds hidden. Only pointers are kept here, under
// this lock; a filter takes itself out in its destructor.
std::mutex g_links_mutex;
std::vector<VisibilityLink *> g_links;

// What a "Follow sources" filter says, copied out under the locks.
struct FollowLook {
	std::vector<std::string> leads, followers;
	LeadRule rule = LeadRule::Any;
	bool acted_valid = false; // it has looked at its leads since it last started
	bool acted_lead = false;  // the leads' state it has followed
};

// What the leads of a "Follow sources" filter say in `scene`: shown or not,
// by the rule. `found` is false when none of them is in the scene (then
// they lead nothing).
bool leads_say_shown(obs_source_t *scene, const std::vector<std::string> &leads, LeadRule rule, bool &found)
{
	size_t there = 0, on = 0;
	for (const std::string &lead : leads) {
		obs_sceneitem_t *item = find_item(scene, lead);
		if (!item)
			continue;
		there++;
		if (obs_sceneitem_visible(item))
			on++;
	}
	found = there > 0;
	return rule == LeadRule::All ? there > 0 && on == there : on > 0;
}

// Who leads and who follows, in mode 2. With lead sources listed, they lead
// and the listed items (any lead left out) follow; saved without any, the
// first listed item leads the rest, as "Follow the first source" did.
void split_follow(const std::vector<std::string> &items, const std::vector<std::string> &lead_list,
		  std::vector<std::string> &leads, std::vector<std::string> &followers)
{
	leads.clear();
	followers.clear();
	if (!lead_list.empty()) {
		leads = lead_list;
		for (const std::string &name : items)
			if (std::find(leads.begin(), leads.end(), name) == leads.end())
				followers.push_back(name);
	} else if (items.size() >= 2) {
		leads.push_back(items[0]);
		followers.assign(items.begin() + 1, items.end());
	}
}

std::vector<FollowLook> follow_links_in(obs_source_t *scene);

std::vector<std::string> names_in(obs_data_t *settings)
{
	std::vector<std::string> names;
	obs_data_array_t *list = obs_data_get_array(settings, "items");
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const std::string name = obs_data_get_string(entry, "value");
		if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	return names;
}

// How the filter's items change: the usual way, and a listed source's own.
struct Changes {
	ItemChange usual;
	std::map<std::string, ItemChange> own;
	const ItemChange &of(const std::string &name) const
	{
		const auto found = own.find(name);
		return found == own.end() ? usual : found->second;
	}
};

class VisibilityLink : public VisibilityLinkRuntime {
	// What the tick goes by, made in `update` and shared with the tick as it
	// is (never copied a frame: the lists and each source's own change are
	// strings).
	struct Config {
		std::vector<std::string> names, elsewhere;
		std::vector<std::string> leads, followers; // Follow sources
		LeadRule rule = LeadRule::Any;
		Changes how;
	};

	// An item that was shown or hidden: its source's name, which of the two,
	// and whether that was in this filter's own scene.
	struct Change {
		std::string name;
		bool visible = false;
		bool own = false;
	};

public:
	VisibilityLink(obs_data_t *settings, obs_source_t *source, bool enabled = true, bool internal = false)
		: source_(source),
		  owner_(source),
		  runtime_on_(enabled),
		  internal_(internal)
	{
		update(settings);
		std::lock_guard lock(g_links_mutex);
		g_links.push_back(this);
		registered_ = true;
	}

	~VisibilityLink()
	{
		if (internal_)
			runtime_retire();
		unregister_runtime();
		detach();
	}

	void unregister_runtime()
	{
		std::lock_guard lock(g_links_mutex);
		if (registered_) {
			g_links.erase(std::remove(g_links.begin(), g_links.end(), this), g_links.end());
			registered_ = false;
		}
	}

	// For link_visibility_holds_hidden: called with g_links_mutex held.
	bool look_if_follows(obs_source_t *scene, FollowLook &look)
	{
		if (retired() || !scene || parent_ != scene || !obs_source_enabled(source_) || !runtime_on_ ||
		    forwarded())
			return false;
		std::lock_guard lock(mutex_);
		if (mode_ != LinkMode::Follow || leads_.empty() || followers_.empty())
			return false;
		look.leads = leads_;
		look.followers = followers_;
		look.rule = rule_;
		look.acted_valid = acted_valid_;
		look.acted_lead = acted_lead_;
		return true;
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> items = names_in(settings);
		const std::vector<std::string> lead_list = pick_list_read(settings, "leads", false);
		const std::vector<std::string> elsewhere = pick_list_read(settings, "elsewhere", false);
		const bool lock_hidden = obs_data_get_bool(settings, "lock_hidden");
		const auto rule = static_cast<LeadRule>(std::clamp((int)obs_data_get_int(settings, "lead_rule"), 0, 1));
		std::lock_guard lock(mutex_);
		// Switched on (or loaded with it on): the listed items that are hidden
		// now are locked, as soon as that may be done.
		if (lock_hidden && !lock_hidden_)
			lock_sweep_ = true;
		lock_hidden_ = lock_hidden;
		elsewhere_ = elsewhere;
		watch_timer_ = 0.0;
		const auto mode = static_cast<LinkMode>(std::clamp((int)obs_data_get_int(settings, "mode"), 0, 2));
		// Follow sources: who leads and who follows. Every listed source,
		// leads first, is what is watched, locked and given its own change.
		std::vector<std::string> leads, followers, names = items;
		if (mode == LinkMode::Follow) {
			split_follow(items, lead_list, leads, followers);
			if (!lead_list.empty()) {
				names = leads;
				names.insert(names.end(), followers.begin(), followers.end());
			}
		}
		// Other leads perhaps, or back from another mode: look afresh.
		if (names != names_ || mode != mode_ || leads != leads_ || followers != followers_ || rule != rule_)
			acted_valid_ = false;
		mode_ = mode;
		leads_ = leads;
		followers_ = followers;
		rule_ = rule;
		// How items change: the usual way, and each listed source's own
		// (*Settings for*). A filter saved with the plain two controls
		// keeps them under the same names: it reads as it did.
		change_.usual = item_change_read(settings);
		change_.own.clear();
		for (const std::vector<std::string> *list : {&std::as_const(names), &elsewhere})
			for (const std::string &name : *list)
				change_.own[name] = item_change_read(settings, name.c_str());
		names_ = names;
		config_ =
			std::make_shared<const Config>(Config{names_, elsewhere_, leads_, followers_, rule_, change_});
	}

	void attach(obs_source_t *parent)
	{
		if (retired())
			return;
		detach();
		{
			std::lock_guard lock(g_links_mutex);
			parent_ = parent;
			parent_weak_ = obs_source_get_weak_source(parent);
		}
		signal_handler_connect(obs_source_get_signal_handler(parent), "item_visible", visible_cb, this);
		if (internal_)
			watch_owned_groups(parent);
	}

	void detach()
	{
		obs_weak_source_t *previous = nullptr;
		{
			std::lock_guard lock(g_links_mutex);
			previous = parent_weak_;
			parent_weak_ = nullptr;
			parent_ = nullptr;
		}
		obs_source_t *held = previous ? obs_weak_source_get_source(previous) : nullptr;
		if (held)
			signal_handler_disconnect(obs_source_get_signal_handler(held), "item_visible", visible_cb,
						  this);
		obs_source_release(held);
		obs_weak_source_release(previous);
		unwatch();
		unwatch_owned_groups();
	}

	void unwatch_owned_groups()
	{
		for (auto *weak : owned_groups_) {
			obs_source_t *group = obs_weak_source_get_source(weak);
			if (group)
				signal_handler_disconnect(obs_source_get_signal_handler(group), "item_visible",
							  visible_cb, this);
			obs_source_release(group);
			obs_weak_source_release(weak);
		}
		owned_groups_.clear();
		std::lock_guard lock(mutex_);
		owned_group_scenes_.clear();
	}

	void watch_owned_groups(obs_source_t *parent)
	{
		std::vector<obs_source_t *> groups;
		const auto collect = [](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
			if (obs_sceneitem_is_group(item))
				if (auto *held = obs_source_get_ref(obs_sceneitem_get_source(item)))
					static_cast<std::vector<obs_source_t *> *>(opaque)->push_back(held);
			return true;
		};
		if (obs_scene_t *scene = scene_of(parent))
			obs_scene_enum_items(scene, collect, &groups);
		for (size_t i = 0; i < groups.size(); i++)
			obs_scene_enum_items(scene_of(groups[i]), collect, &groups);
		for (auto *group : groups) {
			{
				std::lock_guard lock(mutex_);
				owned_group_scenes_.insert(group);
			}
			signal_handler_connect(obs_source_get_signal_handler(group), "item_visible", visible_cb, this);
			owned_groups_.push_back(obs_source_get_weak_source(group));
			obs_source_release(group);
		}
	}

	// Listens to every other scene (and group) while there are sources to
	// follow there; scenes made later are picked up within a couple of
	// seconds. Each is held weakly, so one that is gone is simply skipped.
	void watch_others()
	{
		if (internal_) {
			std::vector<obs_source_t *> scenes;
			obs_enum_scenes(
				[](void *opaque, obs_source_t *scene) {
					if (auto *held = obs_source_get_ref(scene))
						static_cast<std::vector<obs_source_t *> *>(opaque)->push_back(held);
					return true;
				},
				&scenes);
			// Signal locks are taken after releasing OBS's source-list lock.
			for (auto *scene : scenes) {
				bool seen;
				{
					std::lock_guard lock(mutex_);
					seen = scene == parent_ || owned_group_scenes_.count(scene);
				}
				for (auto *weak : watched_)
					seen = seen || obs_weak_source_references_source(weak, scene);
				if (!seen) {
					signal_handler_connect(obs_source_get_signal_handler(scene), "item_visible",
							       visible_cb, this);
					watched_.push_back(obs_source_get_weak_source(scene));
				}
				obs_source_release(scene);
			}
			return;
		}
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *self = static_cast<VisibilityLink *>(param);
				if (scene == self->parent_ ||
				    (self->internal_ && self->owned_group_scenes_.count(scene)))
					return true;
				for (obs_weak_source_t *weak : self->watched_)
					if (obs_weak_source_references_source(weak, scene))
						return true;
				signal_handler_connect(obs_source_get_signal_handler(scene), "item_visible", visible_cb,
						       self);
				self->watched_.push_back(obs_source_get_weak_source(scene));
				return true;
			},
			this);
	}

	void unwatch()
	{
		for (obs_weak_source_t *weak : watched_) {
			if (obs_source_t *scene = obs_weak_source_get_source(weak)) {
				signal_handler_disconnect(obs_source_get_signal_handler(scene), "item_visible",
							  visible_cb, this);
				obs_source_release(scene);
			}
			obs_weak_source_release(weak);
		}
		watched_.clear();
	}

	// The others are switched on the next frame, not inside the signal: OBS
	// has not finished showing or hiding the item while the signal runs.
	void tick(float seconds) override
	{
		if (!internal_) {
			tick_work(seconds);
			return;
		}
		if (retired())
			return;
		struct Pinned {
			obs_source_t *owner, *parent;
			~Pinned()
			{
				obs_source_release(parent);
				obs_source_release(owner);
			}
		} pinned{owner_.get(), parent_weak_ ? obs_weak_source_get_source(parent_weak_) : nullptr};
		if (pinned.owner && pinned.parent && !retired())
			tick_work(seconds);
	}

	void tick_work(float seconds)
	{
		if (forwarded() || !parent_)
			return;
		std::vector<Change> changes;
		LinkMode mode;
		std::shared_ptr<const Config> config;
		bool look_for_scenes = false;
		bool lock_hidden = false;
		{
			std::lock_guard lock(mutex_);
			lock_hidden = lock_hidden_;
			if (lock_sweep_)
				sweep_due_ = true;
			lock_sweep_ = false;
			changes.swap(pending_);
			mode = mode_;
			config = config_;
			if ((watch_timer_ -= seconds) <= 0.0) {
				watch_timer_ = 2.0;
				look_for_scenes = true;
			}
		}
		const std::vector<std::string> &names = config->names, &elsewhere = config->elsewhere;
		const Changes &how = config->how;
		if (look_for_scenes && parent_) {
			if (elsewhere.empty())
				unwatch();
			else
				watch_others();
		}
		if (!parent_ || !obs_source_enabled(source_) || !runtime_on_) {
			if (was_on_)
				scene_revision_bump(); // it holds nothing hidden now
			was_on_ = false;
			std::lock_guard lock(mutex_);
			acted_valid_ = false;
			return;
		}
		// Nothing is locked while OBS is still loading its scenes; the hidden
		// ones are locked once it has.
		const bool locking = lock_hidden && scenes_loaded();
		// The ones hidden already: when the option is switched on, once after
		// loading, and when the filter is switched on again.
		if (!was_on_)
			sweep_due_ = true;
		was_on_ = true;
		if (!lock_hidden)
			sweep_due_ = false;
		// A transition left under way on a listed item (its scene was left
		// with a cut part-way through) is ended, twice a second (each link
		// on a half second of its own).
		if (!staggered_) {
			staggered_ = true;
			stale_timer_ = stagger_phase(source_, 0.5);
			follow_skip_ = 1 + stagger_phase(source_, 15);
		}
		if ((stale_timer_ -= seconds) <= 0.0) {
			stale_timer_ = 0.5;
			for (const std::string &name : names)
				if (end_stale_transition(find_item(parent_, name)))
					obs_log(LOG_INFO,
						"Link Visibility '%s': ended a transition left under way on '%s' in '%s'",
						obs_source_get_name(source_), name.c_str(),
						obs_source_get_name(parent_));
		}
		if (sweep_due_ && locking) {
			for (const std::string &name : names) {
				obs_sceneitem_t *item = find_item(parent_, name);
				if (item && !obs_sceneitem_visible(item) && !obs_sceneitem_locked(item))
					obs_sceneitem_set_locked(item, true);
			}
			sweep_due_ = false;
		}
		// Locked while hidden, unlocked while shown: a listed item of this
		// scene that has just been shown or hidden, by anything.
		const auto lock_as_shown = [&](const std::string &name) {
			obs_sceneitem_t *item = locking ? find_item(parent_, name) : nullptr;
			if (item && obs_sceneitem_locked(item) == obs_sceneitem_visible(item))
				obs_sceneitem_set_locked(item, !obs_sceneitem_visible(item));
		};
		if (mode == LinkMode::Follow) {
			// Looked at only when something it goes by may have changed:
			// an item shown or hidden in any scene (its own listed ones
			// among them), a link's leader followed or a switch pressed
			// (both raise `scene_revision`), its settings, or no state
			// followed yet; and every fifteenth tick anyway.
			const uint64_t rev = scene_revision();
			bool valid;
			{
				std::lock_guard lock(mutex_);
				valid = acted_valid_;
			}
			if (changes.empty() && valid && rev == follow_rev_ && --follow_skip_ > 0)
				return;
			follow_skip_ = 15;
			follow_rev_ = rev;
			count_look(Look::LinkFollow);
			applying_ = std::this_thread::get_id();
			follow_leads(*config, changes, lock_as_shown);
			applying_ = std::thread::id();
			return;
		}
		if (changes.empty())
			return;
		// Its own changes are known by the thread they are made on: a show
		// made at the same moment from anywhere else still counts.
		applying_ = std::this_thread::get_id();
		// Only one: of several shown in this scene since the last tick (two
		// keys pressed in one frame), the last one shown that still shows is
		// the one kept. Acting on each in turn, the second hid the first and
		// the first the second, and nothing was left showing.
		size_t keep = changes.size();
		if (mode == LinkMode::OnlyOne)
			for (size_t i = changes.size(); i-- > 0;) {
				const Change &c = changes[i];
				obs_sceneitem_t *item = c.own && c.visible ? find_item(parent_, c.name) : nullptr;
				if (item && obs_sceneitem_visible(item)) {
					keep = i;
					break;
				}
			}
		for (size_t i = 0; i < changes.size(); i++) {
			const Change &change = changes[i];
			const std::string &changed = change.name;
			const bool visible = change.visible;
			if (change.own)
				lock_as_shown(changed);
			if (mode == LinkMode::OnlyOne && !visible)
				continue; // hiding one says nothing about the others
			if (mode == LinkMode::OnlyOne && change.own && keep < changes.size() && i != keep)
				continue; // a later one is kept
			for (const std::string &name : names) {
				if (name == changed)
					continue;
				obs_sceneitem_t *item = find_item(parent_, name);
				const bool want = mode == LinkMode::Together ? visible : false;
				if (item && obs_sceneitem_visible(item) != want) {
					apply_item_change(item, how.of(name), parent_);
					set_item_visible(item, want);
					lock_as_shown(name);
				}
			}
			// And the sources followed into the other scenes, groups included.
			if (!elsewhere.empty())
				follow_elsewhere(elsewhere, changed, how, mode == LinkMode::OnlyOne, visible);
		}
		applying_ = std::thread::id();
	}

	// Shows or hides the sources followed into the other scenes, groups
	// included: as `changed` was (Together, Follow sources), or, for
	// Show only one, all but the one shown.
	//
	// The items are found first, each held, and changed after: never while a
	// scene is gone through. Going through one holds its locks (and OBS's
	// list of sources); changing an item's transitions and showing or hiding
	// it makes and unmakes sources and runs every listener's handler, and
	// with those locks held that is how two threads come to wait for each
	// other for good.
	void follow_elsewhere(const std::vector<std::string> &elsewhere, const std::string &changed,
			      const Changes &change, bool only_one, bool visible)
	{
		struct Found {
			obs_sceneitem_t *item; // held
			obs_source_t *scene;   // held; null for an item in a group
		};
		struct Work {
			const std::vector<std::string> *sources;
			const std::string *changed;
			bool only_one, visible;
			obs_source_t *scene; // the scene gone through (null for a group)
			std::vector<Found> found;
		} work{&elsewhere, &changed, only_one, visible, nullptr, {}};
		// Together: the same as what changed. Only one: the others go; the
		// one that was shown stays as it is.
		const bool want = only_one ? false : visible;
		static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *w = static_cast<Work *>(param);
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			const bool listed = name && std::find(w->sources->begin(), w->sources->end(), name) !=
							    w->sources->end();
			const bool act = listed && !(w->only_one && *w->changed == name);
			if (act && obs_sceneitem_visible(item) != (w->only_one ? false : w->visible)) {
				obs_sceneitem_addref(item);
				w->found.push_back({item, w->scene ? obs_source_get_ref(w->scene) : nullptr});
			}
			return true;
		};
		struct Scenes {
			Work *work;
			obs_source_t *own;
		} scenes{&work, parent_};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *s = static_cast<Scenes *>(param);
				// Groups are listed as scenes too, so their items are reached.
				// (An item in a group goes by its own group's scene: none
				// is named, and the group's is looked up.)
				s->work->scene = obs_source_is_group(scene) ? nullptr : scene;
				if (scene != s->own)
					obs_scene_enum_items(obs_scene_from_source(scene), each, s->work);
				return true;
			},
			&scenes);
		for (const Found &f : work.found) {
			// (Taken out of its scene, or changed already, meanwhile.)
			const char *name = obs_source_get_name(obs_sceneitem_get_source(f.item));
			if (!name || !obs_sceneitem_get_scene(f.item) || obs_sceneitem_visible(f.item) == want)
				continue;
			apply_item_change(f.item, change.of(name), f.scene);
			set_item_visible(f.item, want);
		}
		for (const Found &f : work.found) {
			obs_sceneitem_release(f.item);
			obs_source_release(f.scene);
		}
	}

	// Follow sources: the leads' state is followed once each time it
	// changes, and while they say hidden none of the followers may show.
	void follow_leads(const Config &config, const std::vector<Change> &changes,
			  const std::function<void(const std::string &)> &lock_as_shown)
	{
		for (const Change &c : changes)
			if (c.own)
				lock_as_shown(c.name);
		bool found = false;
		const bool lead_on = !config.leads.empty() && !config.followers.empty() &&
				     leads_say_shown(parent_, config.leads, config.rule, found);
		// (Not while OBS loads its scenes: only some have their items yet.)
		if (!found || config.followers.empty() || !scenes_loaded()) {
			std::lock_guard lock(mutex_);
			acted_valid_ = false;
			return;
		}
		bool valid, acted;
		{
			std::lock_guard lock(mutex_);
			valid = acted_valid_;
			acted = acted_lead_;
		}
		// The state found at the first look is taken as it is: the others
		// are as they were left (a camera hidden by hand stays hidden).
		const bool act = valid && acted != lead_on;
		for (const std::string &name : config.followers) {
			obs_sceneitem_t *item = find_item(parent_, name);
			if (!item)
				continue;
			const bool shown = obs_sceneitem_visible(item);
			bool want = shown;
			if (act && lead_on)
				// Shown with the leads, whatever a Show in Scenes switch
				// on it says: a switch is a command, not a hold (the
				// owner: "show in scenes should not prevent any other
				// filter from turning on or off a source later").
				want = true;
			else if (!lead_on && shown)
				// Hidden with the leads, and hidden again if something
				// shows it meanwhile; unless another mode showing has it.
				want = !link_visibility_holds_hidden(parent_, item);
			if (want == shown)
				continue;
			apply_item_change(item, config.how.of(name), parent_);
			set_item_visible(item, want);
			lock_as_shown(name);
		}
		if (act && !config.elsewhere.empty())
			follow_elsewhere(config.elsewhere, config.leads[0], config.how, false, lead_on);
		std::lock_guard lock(mutex_);
		// Another state followed: the other links and switches that ask
		// what this one holds hidden look again.
		if (!valid || acted != lead_on)
			scene_revision_bump();
		acted_valid_ = true;
		acted_lead_ = lead_on;
	}

	// Adds the item chosen in the "pick" box to the list.
	bool add_picked()
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::string picked = obs_data_get_string(settings, "pick");
		std::vector<std::string> names = names_in(settings);
		const bool add = !picked.empty() && std::find(names.begin(), names.end(), picked) == names.end();
		if (add) {
			obs_data_array_t *list = obs_data_get_array(settings, "items");
			if (!list) {
				list = obs_data_array_create();
				obs_data_set_array(settings, "items", list);
			}
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", picked.c_str());
			obs_data_array_push_back(list, entry);
			obs_data_release(entry);
			obs_data_array_release(list);
			obs_source_update(source_, settings);
		}
		obs_data_release(settings);
		return add;
	}

	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }
	void runtime_enabled(bool enabled) override
	{
		if (!retired())
			runtime_on_ = enabled;
	}
	void runtime_update(obs_data_t *settings, bool enabled) override
	{
		if (!internal_ || !settings)
			return;
		if (retired())
			return;
		update(settings);
		runtime_enabled(enabled);
		if (retired())
			return;
		// Refresh nested membership without reattaching the engine or clearing
		// its queued eyes and established Follow state.
		unwatch();
		unwatch_owned_groups();
		obs_source_t *parent = parent_weak_ ? obs_weak_source_get_source(parent_weak_) : nullptr;
		if (parent)
			watch_owned_groups(parent);
		obs_source_release(parent);
	}
	void runtime_retire() override
	{
		if (!internal_)
			return;
		if (retired_.exchange(true))
			return;
		runtime_on_ = false;
		unregister_runtime();
		scene_revision_bump();
		// An in-flight tick pins its owner and parent. Disconnect subscriptions
		// only when the last retained runtime call ends and destroys this object;
		// waiting on OBS signal locks here can deadlock a reentrant removal.
		std::lock_guard lock(mutex_);
		pending_.clear();
		acted_valid_ = false;
	}
	bool runtime_retired() const override { return retired(); }
	std::string runtime_controller_id() override
	{
		if (!internal_ || retired())
			return {};
		obs_source_t *owner = owner_.get();
		const std::string id = owner ? obs_source_get_uuid(owner) : "";
		obs_source_release(owner);
		return id;
	}
	std::shared_ptr<VisibilityLinkRuntime> forward() const
	{
		if (!has_forward_.load(std::memory_order_relaxed))
			return {};
		std::lock_guard lock(forward_mutex_);
		auto target = forward_.lock();
		if (!target)
			has_forward_ = false;
		return target;
	}
	bool forwarded() const
	{
		if (!has_forward_.load(std::memory_order_relaxed))
			return false;
		std::lock_guard lock(forward_mutex_);
		const bool alive = !forward_.expired();
		if (!alive)
			has_forward_ = false;
		return alive;
	}
	bool forward_to(const std::shared_ptr<VisibilityLinkRuntime> &runtime)
	{
		bool changed;
		{
			std::lock_guard lock(forward_mutex_);
			if (runtime && runtime->runtime_retired())
				return false;
			const bool was_active = !forward_.expired();
			changed = was_active != bool(runtime);
			forward_ = runtime;
			has_forward_ = bool(runtime);
		}
		if (changed)
			refresh_owner_properties();
		return true;
	}
	bool unforward(const std::shared_ptr<VisibilityLinkRuntime> &expected)
	{
		std::shared_ptr<VisibilityLinkRuntime> previous;
		{
			std::lock_guard lock(forward_mutex_);
			previous = forward_.lock();
			if (!previous || previous != expected)
				return false;
			forward_.reset();
			has_forward_ = false;
		}
		refresh_owner_properties();
		return true;
	}
	bool runtime_press(const char *source, bool shown, std::string &why) override
	{
		if (retired()) {
			why = "The source group is no longer available";
			return false;
		}
		struct Pinned {
			obs_source_t *owner, *parent;
			~Pinned()
			{
				obs_source_release(parent);
				obs_source_release(owner);
			}
		} pinned{owner_.get(), nullptr};
		{
			// A queued press can overlap frontend filter removal. Obtain its
			// parent pin while detach still owns the corresponding weak handle.
			std::lock_guard lock(g_links_mutex);
			if (parent_weak_)
				pinned.parent = obs_weak_source_get_source(parent_weak_);
		}
		if (!pinned.owner || !pinned.parent || retired() || !obs_source_enabled(source_) || !runtime_on_ ||
		    !scenes_loaded()) {
			why = "The source group is unavailable or disabled";
			return false;
		}
		std::shared_ptr<const Config> config;
		LinkMode mode;
		{
			std::lock_guard lock(mutex_);
			config = config_;
			mode = mode_;
		}
		if (mode == LinkMode::Follow || config->names.empty()) {
			why = "A Follow sources rule is controlled by its lead sources";
			return false;
		}
		const std::string name = source && *source ? source : config->names.front();
		if (std::find(config->names.begin(), config->names.end(), name) == config->names.end() ||
		    (mode == LinkMode::OnlyOne && (!source || !*source || !shown))) {
			why = "The requested source is not a selectable choice of this group";
			return false;
		}
		struct Selection {
			const std::string &name;
			obs_sceneitem_t *item = nullptr;
			bool visible = false;
			~Selection() { obs_sceneitem_release(item); }
			static bool visit(obs_scene_t *, obs_sceneitem_t *item, void *opaque)
			{
				auto &self = *static_cast<Selection *>(opaque);
				if (self.name == obs_source_get_name(obs_sceneitem_get_source(item))) {
					obs_sceneitem_addref(item);
					self.item = item;
					self.visible = obs_sceneitem_visible(item);
					return false;
				}
				if (obs_sceneitem_is_group(item))
					obs_scene_enum_items(obs_sceneitem_group_get_scene(item), visit, opaque);
				return !self.item;
			}
		} selection{name};
		// Pin the item and read its eye while enumeration owns the scene lock.
		// A synchronous visibility listener can remove it during the press.
		obs_scene_enum_items(scene_of(pinned.parent), Selection::visit, &selection);
		obs_sceneitem_t *item = selection.item;
		if (!item || !obs_sceneitem_get_scene(item)) {
			why = "The selected source is missing from the scene";
			return false;
		}
		applying_ = std::this_thread::get_id();
		if (selection.visible != shown) {
			apply_item_change(item, config->how.of(name), pinned.parent);
			if (obs_sceneitem_get_scene(item))
				set_item_visible(item, shown);
		}
		applying_ = std::thread::id();
		if (!obs_sceneitem_get_scene(item)) {
			why = "The selected source was removed before the control could be applied";
			return false;
		}
		// A press also repairs an inconsistent set when its lead already has
		// the requested visibility and OBS therefore emits no visibility signal.
		std::lock_guard lock(mutex_);
		pending_.push_back({name, shown, true});
		return true;
	}

private:
	bool retired() const { return internal_ && retired_.load(); }
	void refresh_owner_properties()
	{
		// OBS's properties views queue ReloadProperties onto their Qt thread.
		// No forwarding or engine lock is held while emitting this signal.
		if (auto *owner = owner_.get()) {
			obs_source_update_properties(owner);
			obs_source_release(owner);
		}
	}

	static void visible_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<VisibilityLink *>(data);
		if (self->retired() || self->forwarded())
			return;
		if (self->applying_.load() == std::this_thread::get_id())
			return; // our own doing
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		auto *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
		const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
		if (!name)
			return;
		// In its own scene the listed items count; in any other, the
		// sources followed there.
		std::lock_guard lock(self->mutex_);
		const bool own = !scene || obs_scene_get_source(scene) == self->parent_ ||
				 (self->internal_ && self->owned_group_scenes_.count(obs_scene_get_source(scene)));
		const std::vector<std::string> &list = own ? self->names_ : self->elsewhere_;
		if (std::find(list.begin(), list.end(), name) != list.end())
			self->pending_.push_back({name, calldata_bool(cd, "visible"), own});
	}

	obs_source_t *source_;
	OwnerRef owner_;
	std::atomic<bool> runtime_on_{true};
	bool internal_ = false;
	std::atomic<bool> retired_{false};
	bool registered_ = false; // guarded by g_links_mutex
	mutable std::mutex forward_mutex_;
	mutable std::atomic<bool> has_forward_{false};
	std::weak_ptr<VisibilityLinkRuntime> forward_;
	obs_source_t *parent_ = nullptr;
	obs_weak_source_t *parent_weak_ = nullptr;
	std::mutex mutex_;
	Changes change_;
	std::shared_ptr<const Config> config_;
	LinkMode mode_ = LinkMode::OnlyOne;
	std::vector<std::string> names_, elsewhere_;
	std::vector<std::string> leads_, followers_; // Follow sources: who leads, who follows
	LeadRule rule_ = LeadRule::Any;
	std::vector<obs_weak_source_t *> watched_; // the other scenes listened to
	std::vector<obs_weak_source_t *> owned_groups_;
	std::set<obs_source_t *> owned_group_scenes_;
	double watch_timer_ = 0.0;
	std::vector<Change> pending_;
	bool acted_valid_ = false;                // Follow sources: the leads have been looked at
	bool acted_lead_ = false;                 // and this is the state of them last followed
	bool lock_hidden_ = false;                // lock the listed items while they are hidden
	bool lock_sweep_ = false;                 // the option has just been switched on
	bool sweep_due_ = false;                  // the hidden ones are still to be locked (the tick's own)
	bool was_on_ = false;                     // the filter was enabled at its last tick
	double stale_timer_ = 0.0;                // until the next look for transitions left under way
	bool staggered_ = false;                  // the timers have been given their phase
	uint64_t follow_rev_ = 0;                 // Follow sources: scene_revision() at the last look
	int follow_skip_ = 0;                     // and ticks until a look comes anyway
	std::atomic<std::thread::id> applying_{}; // the thread acting for the filter, while it does
};

// Follow sources' controls are shown in that mode only, and the list of
// linked items is called what it is there: the sources that follow.
void show_follow_controls(obs_properties_t *props, obs_data_t *settings)
{
	const bool follow = obs_data_get_int(settings, "mode") == (int)LinkMode::Follow;
	const bool no_leads = pick_list_read(settings, "leads", false).empty();
	if (obs_property_t *leads = obs_properties_get(props, "leads|alist"))
		obs_property_set_visible(leads, follow);
	if (obs_property_t *rule = obs_properties_get(props, "lead_rule"))
		obs_property_set_visible(rule, follow);
	// Saved without lead sources: the first listed leads. Offered to be
	// made a lead source; nothing is rewritten unasked.
	if (obs_property_t *take = obs_properties_get(props, "take_first"))
		obs_property_set_visible(take, follow && no_leads && names_in(settings).size() >= 2);
	if (obs_property_t *items = obs_properties_get(props, "items|alist"))
		obs_property_set_description(items, obs_module_text(follow ? "Link.Following" : "Link.Items"));
}

bool mode_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	show_follow_controls(props, settings);
	return true;
}

// A list of this kind's was changed in its window.
void lists_changed(obs_properties_t *top, obs_source_t *, obs_data_t *settings)
{
	show_follow_controls(top, settings);
}

// "Make the first a lead source": the first listed item goes to Lead
// sources, which leads exactly as before.
bool take_first_clicked(obs_properties_t *props, obs_property_t *, void *data)
{
	auto *self = static_cast<VisibilityLink *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	if (!owner)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> items = names_in(settings);
	if (pick_list_read(settings, "leads", false).empty() && items.size() >= 2) {
		pick_list_write(settings, "leads", false, {items[0]});
		items.erase(items.begin());
		pick_list_write(settings, "items", false, items);
		obs_source_update(owner, settings);
		pick_list_refresh(props, owner, "leads", false);
		pick_list_refresh(props, owner, "items", false);
	}
	show_follow_controls(props, settings);
	obs_data_release(settings);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<VisibilityLink *>(data);
	obs_properties_t *props = obs_properties_create();
	if (self)
		if (auto target = self->forward(); target && !target->runtime_controller_id().empty()) {
			auto *help = obs_properties_add_text(props, "controller_managed",
							     obs_module_text("Views.ManagedLinkFilter"), OBS_TEXT_INFO);
			obs_property_text_set_info_word_wrap(help, true);
			obs_properties_add_button2(
				props, "controller_configure", obs_module_text("Views.Configure"),
				[](obs_properties_t *, obs_property_t *, void *opaque) {
					auto *legacy = static_cast<VisibilityLink *>(opaque);
					if (auto runtime = legacy ? legacy->forward() : nullptr) {
						const auto id = runtime->runtime_controller_id();
						if (!id.empty())
							show_views_controller(id.c_str());
					}
					return false;
				},
				self);
			return props;
		}
	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Link.Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.OnlyOne"), (int)LinkMode::OnlyOne);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.Together"), (int)LinkMode::Together);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.Follow"), (int)LinkMode::Follow);
	set_tip(mode, obs_module_text("Link.Mode.Tip"));
	obs_property_set_modified_callback(mode, mode_modified);

	// Follow sources: the sources that lead, and when the rest show.
	PickList leads;
	leads.setting = "leads";
	leads.title = "Link.Leads";
	leads.add = "Link.Leads.Add";
	leads.tip = "Link.Leads.Tip";
	leads.choices = [self](std::vector<std::string> &names) {
		scene_item_names(self ? self->parent() : nullptr, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, leads);
	obs_property_t *rule = obs_properties_add_list(props, "lead_rule", obs_module_text("Link.LeadRule"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(rule, obs_module_text("Link.LeadRule.Any"), (int)LeadRule::Any);
	obs_property_list_add_int(rule, obs_module_text("Link.LeadRule.All"), (int)LeadRule::All);
	set_tip(rule, obs_module_text("Link.LeadRule.Tip"));
	obs_property_t *take = obs_properties_add_button2(props, "take_first", obs_module_text("Link.TakeFirst"),
							  take_first_clicked, self);
	set_tip(take, obs_module_text("Link.TakeFirst.Tip"));

	PickList items;
	items.setting = "items";
	items.title = "Link.Items";
	items.add = "List.AddItem";
	items.tip = "Link.Items.Tip";
	items.choices = [self](std::vector<std::string> &names) {
		scene_item_names(self ? self->parent() : nullptr, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, items);
	// Sources in the other scenes that are shown and hidden with them.
	PickList elsewhere;
	elsewhere.setting = "elsewhere";
	elsewhere.title = "Link.Elsewhere";
	elsewhere.add = "Link.Elsewhere.Add";
	elsewhere.tip = "Link.Elsewhere.Tip";
	elsewhere.choices = [](std::vector<std::string> &names) {
		const auto add = [](void *param, obs_source_t *source) {
			auto *list = static_cast<std::vector<std::string> *>(param);
			const char *name = obs_source_get_name(source);
			if (name && *name && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) &&
			    std::find(list->begin(), list->end(), name) == list->end())
				list->push_back(name);
			return true;
		};
		obs_enum_sources(add, &names);
		obs_enum_scenes(add, &names); // scenes and groups can be items too
	};
	pick_list_add(props, self ? self->source() : nullptr, elsewhere);

	obs_property_t *lock_hidden = obs_properties_add_bool(props, "lock_hidden", obs_module_text("Link.LockHidden"));
	set_tip(lock_hidden, obs_module_text("Link.LockHidden.Tip"));
	// The fuller Show/Hide transition, with Settings for each source of
	// both lists: last but the line of help, as in every window.
	add_item_change_properties(props, self ? self->source() : nullptr, "leads,items,elsewhere");
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Link.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	if (self) {
		obs_data_t *settings = obs_source_get_settings(self->source());
		show_follow_controls(props, settings);
		obs_data_release(settings);
	}
	return props;
}

std::vector<FollowLook> follow_links_in(obs_source_t *scene)
{
	std::vector<FollowLook> looks;
	std::lock_guard lock(g_links_mutex);
	for (VisibilityLink *link : g_links) {
		FollowLook look;
		if (link->look_if_follows(scene, look))
			looks.push_back(std::move(look));
	}
	return looks;
}

bool is_follower(const FollowLook &look, const char *name)
{
	return name && std::find(look.followers.begin(), look.followers.end(), name) != look.followers.end();
}

} // namespace

bool visibility_link_press(obs_source_t *filter, const char *source, bool shown, std::string &why)
{
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	if (!id || strcmp(id, kVisibilityLinkId) != 0 || obs_source_removed(filter) || !obs_filter_get_parent(filter)) {
		why = "The Link Visibility filter is no longer available";
		return false;
	}
	if (!obs_source_enabled(filter)) {
		why = "Link Visibility is disabled";
		return false;
	}
	auto *self = static_cast<VisibilityLink *>(obs_obj_get_data(filter));
	if (!self) {
		why = "Link Visibility is not ready";
		return false;
	}
	struct Press {
		obs_source_t *filter = nullptr, *parent = nullptr, *member = nullptr;
		bool shown = false;
		~Press()
		{
			obs_source_release(member);
			obs_source_release(parent);
			obs_source_release(filter);
		}
	};
	auto press = std::make_unique<Press>();
	press->filter = obs_source_get_ref(filter);
	press->parent = obs_source_get_ref(obs_filter_get_parent(filter));
	press->shown = shown;
	if (source && *source) {
		press->member = obs_get_source_by_name(source);
		if (!press->member || obs_source_removed(press->member)) {
			why = "The selected source is missing from the scene";
			return false;
		}
	}
	if (!press->parent) {
		why = "The Link Visibility filter is no longer attached";
		return false;
	}
	// runtime_press and tick share the applying_ marker and transition state.
	// Run both on OBS's video thread, never concurrently from the frontend.
	obs_queue_task(
		OBS_TASK_GRAPHICS,
		[](void *opaque) {
			std::unique_ptr<Press> press(static_cast<Press *>(opaque));
			if (!scenes_loaded() || obs_source_removed(press->filter) ||
			    obs_source_removed(press->parent) || !obs_source_enabled(press->filter) ||
			    obs_filter_get_parent(press->filter) != press->parent ||
			    (press->member && obs_source_removed(press->member)))
				return;
			auto *self = static_cast<VisibilityLink *>(obs_obj_get_data(press->filter));
			if (!self)
				return;
			const std::string name = press->member ? obs_source_get_name(press->member) : "";
			std::string ignored;
			// OBS_TASK_GRAPHICS does not itself enter a graphics context.
			// Native item transitions can create graphics resources here.
			obs_enter_graphics();
			if (auto forwarded = self->forward())
				forwarded->runtime_press(name.c_str(), press->shown, ignored);
			else
				self->runtime_press(name.c_str(), press->shown, ignored);
			obs_leave_graphics();
		},
		press.release(), false);
	return true;
}

std::shared_ptr<VisibilityLinkRuntime> visibility_link_runtime(obs_data_t *settings, obs_source_t *owner,
							       obs_source_t *parent, bool enabled)
{
	item_change_defaults(settings);
	auto runtime = std::make_shared<VisibilityLink>(settings, owner, enabled, true);
	runtime->attach(parent);
	return runtime;
}

bool visibility_link_forward(obs_source_t *legacy, const std::shared_ptr<VisibilityLinkRuntime> &runtime)
{
	const char *id = legacy ? obs_source_get_unversioned_id(legacy) : nullptr;
	if (!id || strcmp(id, kVisibilityLinkId) != 0)
		return false;
	auto *self = static_cast<VisibilityLink *>(obs_obj_get_data(legacy));
	if (!self)
		return false;
	return self->forward_to(runtime);
}

bool visibility_link_unforward(obs_source_t *legacy, const std::shared_ptr<VisibilityLinkRuntime> &expected)
{
	const char *id = legacy ? obs_source_get_unversioned_id(legacy) : nullptr;
	if (!id || strcmp(id, kVisibilityLinkId) != 0)
		return false;
	auto *self = static_cast<VisibilityLink *>(obs_obj_get_data(legacy));
	return self && self->unforward(expected);
}

bool link_visibility_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item)
{
	const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
	if (!name || !scene)
		return false;
	bool held = false;
	// (No lock is held while the leaders are looked up.)
	for (const FollowLook &look : follow_links_in(scene)) {
		if (!is_follower(look, name))
			continue;
		// (The item the link manages: the first of that source straight in
		// the scene, not another copy or one inside a group.)
		if (find_item(scene, name) != item)
			continue;
		bool found = false;
		const bool on = leads_say_shown(scene, look.leads, look.rule, found);
		if (!found)
			continue; // a mode without its leads leads nothing
		if (on)
			return false; // a mode that is showing has it
		held = true;
	}
	return held;
}

bool link_visibility_settling(obs_source_t *scene, obs_sceneitem_t *item)
{
	const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
	if (!name || !scene)
		return false;
	for (const FollowLook &look : follow_links_in(scene)) {
		if (!look.acted_valid || !is_follower(look, name))
			continue;
		bool found = false;
		const bool on = leads_say_shown(scene, look.leads, look.rule, found);
		if (found && on != look.acted_lead)
			return true;
	}
	return false;
}

void register_visibility_link()
{
	obs_source_info info = {};
	info.id = kVisibilityLinkId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Link.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new VisibilityLink(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<VisibilityLink *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<VisibilityLink *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "mode", (int)LinkMode::OnlyOne);
		obs_data_set_default_bool(settings, "lock_hidden", false);
		obs_data_set_default_int(settings, "lead_rule", (int)LeadRule::Any);
		item_change_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<VisibilityLink *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		skip_filter_input(static_cast<VisibilityLink *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<VisibilityLink *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<VisibilityLink *>(data)->detach();
	};
	register_timed(&info);
	pick_list_on_change_for(kVisibilityLinkId, lists_changed);
	// Its "Follow sources" modes hold the sources of the modes not
	// showing hidden, for every filter that shows things.
	holds_hidden_register(link_visibility_holds_hidden, link_visibility_settling);
}

} // namespace stagekit
