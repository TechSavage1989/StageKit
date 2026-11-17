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
//   Follow the first  the first item listed leads: shown, the others are
//                   shown with it (but not one a Show in Scenes switch holds
//                   off: a camera switched off); hidden, they are hidden and
//                   kept hidden. A change to any other item moves nothing
//                   else. One scene with several modes (a title and its
//                   extras each) uses this, so a camera switch that acts in
//                   every scene shows the camera only in the mode showing.
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
#include "show-in-scenes.hpp"
#include "../common/timing.hpp"

#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

enum class LinkMode : int {
	OnlyOne = 0, // saved: append only
	Together = 1,
	Follow = 2, // the first listed leads
};

class VisibilityLink;

// Every Link Visibility there is, so Show in Scenes can ask which items a
// mode that is not showing holds hidden. Only pointers are kept here, under
// this lock; a filter takes itself out in its destructor.
std::mutex g_links_mutex;
std::vector<VisibilityLink *> g_links;

// What a "Follow the first" filter says, copied out under the locks.
struct FollowLook {
	std::vector<std::string> names; // the first is the leader
	bool acted_valid = false;       // it has looked at its leader since it last started
	bool acted_lead = false;        // the leader's state it has followed
};

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

class VisibilityLink {
	// An item that was shown or hidden: its source's name, which of the two,
	// and whether that was in this filter's own scene.
	struct Change {
		std::string name;
		bool visible = false;
		bool own = false;
	};

public:
	VisibilityLink(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		std::lock_guard lock(g_links_mutex);
		g_links.push_back(this);
	}

	~VisibilityLink()
	{
		{
			std::lock_guard lock(g_links_mutex);
			g_links.erase(std::remove(g_links.begin(), g_links.end(), this), g_links.end());
		}
		detach();
	}

	// For link_visibility_holds_hidden: called with g_links_mutex held.
	bool look_if_follows(obs_source_t *scene, FollowLook &look)
	{
		if (!scene || parent_ != scene || !obs_source_enabled(source_))
			return false;
		std::lock_guard lock(mutex_);
		if (mode_ != LinkMode::Follow || names_.size() < 2)
			return false;
		look.names = names_;
		look.acted_valid = acted_valid_;
		look.acted_lead = acted_lead_;
		return true;
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> names = names_in(settings);
		const std::vector<std::string> elsewhere = pick_list_read(settings, "elsewhere", false);
		const bool lock_hidden = obs_data_get_bool(settings, "lock_hidden");
		std::lock_guard lock(mutex_);
		// Switched on (or loaded with it on): the listed items that are hidden
		// now are locked, as soon as that may be done.
		if (lock_hidden && !lock_hidden_)
			lock_sweep_ = true;
		lock_hidden_ = lock_hidden;
		elsewhere_ = elsewhere;
		watch_timer_ = 0.0;
		const auto mode = static_cast<LinkMode>(std::clamp((int)obs_data_get_int(settings, "mode"), 0, 2));
		// Another leader perhaps, or back from another mode: look afresh.
		if (names != names_ || mode != mode_)
			acted_valid_ = false;
		mode_ = mode;
		transition_ = item_transition_kind(settings);
		transition_ms_ = item_transition_ms(settings);
		names_ = names;
	}

	void attach(obs_source_t *parent)
	{
		detach();
		{
			std::lock_guard lock(g_links_mutex);
			parent_ = parent;
		}
		signal_handler_connect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb, this);
	}

	void detach()
	{
		if (parent_)
			signal_handler_disconnect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb,
						  this);
		{
			std::lock_guard lock(g_links_mutex);
			parent_ = nullptr;
		}
		unwatch();
	}

	// Listens to every other scene (and group) while there are sources to
	// follow there; scenes made later are picked up within a couple of
	// seconds. Each is held weakly, so one that is gone is simply skipped.
	void watch_others()
	{
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *self = static_cast<VisibilityLink *>(param);
				if (scene == self->parent_)
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
	void tick(float seconds)
	{
		std::vector<Change> changes;
		LinkMode mode;
		std::vector<std::string> names, elsewhere;
		bool look_for_scenes = false;
		std::string transition;
		uint32_t transition_ms = 0;
		bool lock_hidden = false;
		{
			std::lock_guard lock(mutex_);
			lock_hidden = lock_hidden_;
			if (lock_sweep_)
				sweep_due_ = true;
			lock_sweep_ = false;
			changes.swap(pending_);
			mode = mode_;
			names = names_;
			elsewhere = elsewhere_;
			if ((watch_timer_ -= seconds) <= 0.0) {
				watch_timer_ = 2.0;
				look_for_scenes = true;
			}
			transition = transition_;
			transition_ms = transition_ms_;
		}
		if (look_for_scenes && parent_) {
			if (elsewhere.empty())
				unwatch();
			else
				watch_others();
		}
		if (!parent_ || !obs_source_enabled(source_)) {
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
		// with a cut part-way through) is ended, twice a second.
		if ((stale_timer_ -= seconds) <= 0.0) {
			stale_timer_ = 0.5;
			for (const std::string &name : names)
				if (end_stale_transition(find_item(parent_, name)))
					obs_log(LOG_INFO,
						"Link Visibility '%s': ended a transition left under way on '%s'",
						obs_source_get_name(source_), name.c_str());
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
			applying_ = std::this_thread::get_id();
			follow_first(names, elsewhere, changes, transition, transition_ms, lock_as_shown);
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
					set_item_transitions(item, transition, transition_ms);
					set_item_visible(item, want);
					lock_as_shown(name);
				}
			}
			// And the sources followed into the other scenes, groups included.
			if (!elsewhere.empty())
				follow_elsewhere(elsewhere, changed, transition, transition_ms,
						 mode == LinkMode::OnlyOne, visible);
		}
		applying_ = std::thread::id();
	}

	// Shows or hides the sources followed into the other scenes, groups
	// included: as `changed` was (Together, Follow the first), or, for
	// Show only one, all but the one shown.
	void follow_elsewhere(const std::vector<std::string> &elsewhere, const std::string &changed,
			      const std::string &transition, uint32_t transition_ms, bool only_one, bool visible)
	{
		struct Work {
			const std::vector<std::string> *sources;
			const std::string *changed, *transition;
			uint32_t transition_ms;
			bool only_one, visible;
		} work{&elsewhere, &changed, &transition, transition_ms, only_one, visible};
		static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *w = static_cast<Work *>(param);
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			const bool listed = name && std::find(w->sources->begin(), w->sources->end(), name) !=
							    w->sources->end();
			// Together: the same as what changed. Only one: the
			// others go; the one that was shown stays as it is.
			const bool act = listed && !(w->only_one && *w->changed == name);
			const bool want = w->only_one ? false : w->visible;
			if (act && obs_sceneitem_visible(item) != want) {
				set_item_transitions(item, *w->transition, w->transition_ms);
				set_item_visible(item, want);
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
				if (scene != s->own)
					obs_scene_enum_items(obs_scene_from_source(scene), each, s->work);
				return true;
			},
			&scenes);
	}

	// Follow the first: the leader's state is followed once each time it
	// changes, and while it is hidden none of the others may show.
	void follow_first(const std::vector<std::string> &names, const std::vector<std::string> &elsewhere,
			  const std::vector<Change> &changes, const std::string &transition, uint32_t transition_ms,
			  const std::function<void(const std::string &)> &lock_as_shown)
	{
		for (const Change &c : changes)
			if (c.own)
				lock_as_shown(c.name);
		obs_sceneitem_t *leader = names.size() >= 2 ? find_item(parent_, names[0]) : nullptr;
		// (Not while OBS loads its scenes: only some have their items yet.)
		if (!leader || !scenes_loaded()) {
			std::lock_guard lock(mutex_);
			acted_valid_ = false;
			return;
		}
		const bool lead_on = obs_sceneitem_visible(leader);
		bool valid, acted;
		{
			std::lock_guard lock(mutex_);
			valid = acted_valid_;
			acted = acted_lead_;
		}
		// The state found at the first look is taken as it is: the others
		// are as they were left (a camera hidden by hand stays hidden).
		const bool act = valid && acted != lead_on;
		for (size_t i = 1; i < names.size(); i++) {
			obs_sceneitem_t *item = find_item(parent_, names[i]);
			if (!item || item == leader)
				continue;
			const bool shown = obs_sceneitem_visible(item);
			bool want = shown;
			if (act && lead_on)
				// Shown with the leader, unless a switch holds it off.
				want = shown || !show_in_scenes_holds_off(parent_, obs_sceneitem_get_source(item));
			else if (!lead_on && shown)
				// Hidden with the leader, and hidden again if something
				// shows it meanwhile; unless another mode showing has it.
				want = !link_visibility_holds_hidden(parent_, item);
			if (want == shown)
				continue;
			set_item_transitions(item, transition, transition_ms);
			set_item_visible(item, want);
			lock_as_shown(names[i]);
		}
		if (act && !elsewhere.empty())
			follow_elsewhere(elsewhere, names[0], transition, transition_ms, false, lead_on);
		std::lock_guard lock(mutex_);
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

private:
	static void visible_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<VisibilityLink *>(data);
		if (self->applying_.load() == std::this_thread::get_id())
			return; // our own doing
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		auto *scene = static_cast<obs_scene_t *>(calldata_ptr(cd, "scene"));
		const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
		if (!name)
			return;
		// In its own scene the listed items count; in any other, the
		// sources followed there.
		const bool own = !scene || obs_scene_get_source(scene) == self->parent_;
		std::lock_guard lock(self->mutex_);
		const std::vector<std::string> &list = own ? self->names_ : self->elsewhere_;
		if (std::find(list.begin(), list.end(), name) != list.end())
			self->pending_.push_back({name, calldata_bool(cd, "visible"), own});
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	std::string transition_ = kTransitionFade;
	uint32_t transition_ms_ = 300;
	LinkMode mode_ = LinkMode::OnlyOne;
	std::vector<std::string> names_, elsewhere_;
	std::vector<obs_weak_source_t *> watched_; // the other scenes listened to
	double watch_timer_ = 0.0;
	std::vector<Change> pending_;
	bool acted_valid_ = false;                // Follow the first: the leader has been looked at
	bool acted_lead_ = false;                 // and this is the state of it last followed
	bool lock_hidden_ = false;                // lock the listed items while they are hidden
	bool lock_sweep_ = false;                 // the option has just been switched on
	bool sweep_due_ = false;                  // the hidden ones are still to be locked (the tick's own)
	bool was_on_ = false;                     // the filter was enabled at its last tick
	double stale_timer_ = 0.0;                // until the next look for transitions left under way
	std::atomic<std::thread::id> applying_{}; // the thread acting for the filter, while it does
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<VisibilityLink *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Link.Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.OnlyOne"), (int)LinkMode::OnlyOne);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.Together"), (int)LinkMode::Together);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.Follow"), (int)LinkMode::Follow);
	obs_property_set_long_description(mode, obs_module_text("Link.Mode.Tip"));

	PickList items;
	items.setting = "items";
	items.title = "Link.Items";
	items.add = "List.AddItem";
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

	add_item_transition_properties(props);
	obs_property_t *lock_hidden = obs_properties_add_bool(props, "lock_hidden", obs_module_text("Link.LockHidden"));
	obs_property_set_long_description(lock_hidden, obs_module_text("Link.LockHidden.Tip"));
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Link.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
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
	return name && std::find(look.names.begin() + 1, look.names.end(), name) != look.names.end();
}

} // namespace

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
		obs_sceneitem_t *leader = find_item(scene, look.names[0]);
		if (!leader)
			continue; // a mode without its leader leads nothing
		if (obs_sceneitem_visible(leader))
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
		obs_sceneitem_t *leader = find_item(scene, look.names[0]);
		if (leader && obs_sceneitem_visible(leader) != look.acted_lead)
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
		item_transition_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<VisibilityLink *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<VisibilityLink *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<VisibilityLink *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<VisibilityLink *>(data)->detach();
	};
	register_timed(&info);
	// Its "Follow the first source" modes hold the sources of the modes not
	// showing hidden, for every filter that shows things.
	holds_hidden_register(link_visibility_holds_hidden, link_visibility_settling);
}

} // namespace stagekit
