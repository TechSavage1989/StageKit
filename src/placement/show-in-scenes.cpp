/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Show in Scenes: one switch for showing and hiding sources across scenes.
//
// The filter goes on any source and is the switch: a Stream Deck Filter key,
// the eye beside it or a hotkey turns it on and off. Turned on, the listed
// sources are shown in the scenes chosen (or hidden, if set the other way);
// turned off, the opposite. With no source listed it acts on the source it
// is on; with "every scene", wherever those sources are.
//
// It replaces a Stream Deck multi-action holding one Source Visibility step
// for every scene. It acts when it is switched, not all the time, so an item
// shown or hidden by hand stays that way until the next press.
//
// "Follow what is shown" turns it round as well: the switch reads on or off
// by what is shown in its scenes, so a key's light stays true when something
// else (a Link Visibility, the eye) shows or hides its sources. Correcting
// the switch never shows or hides anything.
//
// An item that a Link Visibility set to "Follow the first" holds hidden (it
// belongs to a mode of its scene that is not showing) is passed over when
// the switch shows things, and does not count for "Follow what is shown".
//
// "Switch to scene" (`go_to`, empty as it comes): a press that turns the
// switch on, once its sources are shown, also changes OBS to that scene with
// OBS's current transition (the preview in Studio Mode), unless the scene on
// screen already has one of the sources. It goes by the scene's name, so it
// works with any collection. Not on a press turning it off, not when "Follow
// what is shown" corrects the switch, not for the state restored at load.

#include "show-in-scenes.hpp"
#include "../common/timing.hpp"

#include "place-item.hpp"
#include "visibility-link.hpp"

#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <callback/proc.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stagekit {

namespace {

enum class Where : int {
	Every = 0, // saved: append only
	Listed = 1,
};

// Goes through the items showing one of `sources` in the scenes covered:
// every scene, or the ones named. A group's own items are reached through the
// scene that holds the group, and `scene` is that scene for them too.
void each_item(const std::vector<std::string> &sources, const std::vector<std::string> &scenes, bool every,
	       const std::function<void(obs_sceneitem_t *item, const char *name, obs_source_t *scene)> &act)
{
	struct Work {
		const std::vector<std::string> *sources, *scenes;
		bool every;
		const std::function<void(obs_sceneitem_t *, const char *, obs_source_t *)> *act;
		obs_source_t *scene = nullptr; // the scene being gone through
		void look(obs_sceneitem_t *item) const
		{
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (name && std::find(sources->begin(), sources->end(), name) != sources->end())
				(*act)(item, name, scene);
		}
	} work{&sources, &scenes, every, &act};
	static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		static_cast<Work *>(param)->look(item);
		if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					static_cast<Work *>(p)->look(inner);
					return true;
				},
				param);
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			auto *w = static_cast<Work *>(param);
			// OBS lists each group as a scene too; its items are reached
			// through the scene that holds it.
			if (obs_source_is_group(scene))
				return true;
			const char *name = obs_source_get_name(scene);
			if (!w->every &&
			    (!name || std::find(w->scenes->begin(), w->scenes->end(), name) == w->scenes->end()))
				return true;
			w->scene = scene;
			obs_scene_enum_items(obs_scene_from_source(scene), each, param);
			return true;
		},
		&work);
}

// Adds to `sources` the items their Send To filters bring along, and those
// Place Item filters that move them bring along.
void add_linked(std::vector<std::string> &sources)
{
	const std::vector<std::string> leaders = sources;
	auto add = [&sources](const char *name) {
		if (name && *name && std::find(sources.begin(), sources.end(), name) == sources.end())
			sources.push_back(name);
	};
	for (const std::string &leader : leaders) {
		obs_source_t *found = obs_get_source_by_name(leader.c_str());
		if (!found)
			continue;
		std::vector<std::string> linked;
		obs_source_enum_filters(
			found,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				const char *id = obs_source_get_unversioned_id(filter);
				if (!id || strcmp(id, kSendToId) != 0)
					return;
				obs_data_t *settings = obs_source_get_settings(filter);
				static_cast<std::vector<std::string> *>(param)->push_back(
					obs_data_get_string(settings, "linked"));
				obs_data_release(settings);
			},
			&linked);
		obs_source_release(found);
		for (const std::string &name : linked)
			add(name.c_str());
	}
	struct Scan {
		const std::vector<std::string> *leaders;
		std::vector<std::string> linked;
	} scan{&leaders, {}};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			obs_source_enum_filters(
				scene,
				[](obs_source_t *, obs_source_t *filter, void *p) {
					auto *sc = static_cast<Scan *>(p);
					const char *id = obs_source_get_unversioned_id(filter);
					if (!id || strcmp(id, kPlaceItemId) != 0)
						return;
					obs_data_t *settings = obs_source_get_settings(filter);
					const char *moved = obs_data_get_string(settings, "item");
					if (std::find(sc->leaders->begin(), sc->leaders->end(), moved) !=
					    sc->leaders->end())
						sc->linked.push_back(obs_data_get_string(settings, "linked"));
					obs_data_release(settings);
				},
				param);
			return true;
		},
		&scan);
	for (const std::string &name : scan.linked)
		add(name.c_str());
}

// --- "Switch to scene".

// For the tests, which have no OBS window: "stagekit_test_scene_switch"
// (string program) stands in for the scene on screen, and from then on a
// change is noted instead of made; "stagekit_test_scene_switched" (out
// string scenes) gives the scenes asked for since it was last called,
// separated by '|', and forgets them.
std::mutex g_test_mutex;
bool g_test_switching = false;
std::string g_test_program, g_test_switched;

// True when `scene` holds an item of one of `sources`, straight in it or in
// one of its groups.
bool scene_has_any(obs_source_t *scene, const std::vector<std::string> &sources)
{
	obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
	if (!s)
		return false;
	struct Look {
		const std::vector<std::string> *sources;
		bool found = false;
		void at(obs_sceneitem_t *item)
		{
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (name && std::find(sources->begin(), sources->end(), name) != sources->end())
				found = true;
		}
	} look{&sources};
	obs_scene_enum_items(
		s,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *l = static_cast<Look *>(param);
			l->at(item);
			if (!l->found && obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(
					item,
					[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
						static_cast<Look *>(p)->at(inner);
						return !static_cast<Look *>(p)->found;
					},
					param);
			return !l->found;
		},
		&look);
	return look.found;
}

// Whether to change from `current` to the scene named `target`: it must be a
// scene (not a group), not the one on screen, and the one on screen must not
// have any of `sources` already. Gives the target, with a reference, or null.
obs_source_t *scene_to_go_to(obs_source_t *current, const std::string &target, const std::vector<std::string> &sources)
{
	obs_source_t *scene = obs_get_source_by_name(target.c_str());
	if (!scene)
		return nullptr;
	if (!obs_scene_from_source(scene) || obs_source_is_group(scene) || scene == current ||
	    scene_has_any(current, sources)) {
		obs_source_release(scene);
		return nullptr;
	}
	return scene;
}

struct GoTo {
	std::string filter, target;
	std::vector<std::string> sources;
};

// Changes scene for a press, on the window's thread (OBS's frontend calls
// belong there), or notes it in the tests.
void go_to_scene(GoTo go)
{
	{
		std::lock_guard lock(g_test_mutex);
		if (g_test_switching) {
			obs_source_t *current = obs_get_source_by_name(g_test_program.c_str());
			obs_source_t *scene = scene_to_go_to(current, go.target, go.sources);
			if (scene) {
				if (!g_test_switched.empty())
					g_test_switched += "|";
				g_test_switched += go.target;
				obs_source_release(scene);
			}
			obs_source_release(current);
			return;
		}
	}
	if (!obs_frontend_get_main_window())
		return;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			GoTo *go = static_cast<GoTo *>(param);
			const bool studio = obs_frontend_preview_program_mode_active();
			obs_source_t *current = studio ? obs_frontend_get_current_preview_scene()
						       : obs_frontend_get_current_scene();
			obs_source_t *scene = scene_to_go_to(current, go->target, go->sources);
			if (scene) {
				if (studio)
					obs_frontend_set_current_preview_scene(scene);
				else
					obs_frontend_set_current_scene(scene);
				obs_log(LOG_INFO, "Show in Scenes '%s': switched to scene '%s'", go->filter.c_str(),
					go->target.c_str());
				obs_source_release(scene);
			}
			obs_source_release(current);
			delete go;
		},
		new GoTo(std::move(go)), false);
}

class ShowInScenes;

// Every Show in Scenes there is, for show_in_scenes_holds_off. Only pointers
// are kept, under this lock; a filter takes itself out in its destructor.
std::mutex g_switches_mutex;
std::vector<ShowInScenes *> g_switches;

// What a switch covers and whether it now means hidden, copied out.
struct SwitchLook {
	std::vector<std::string> sources, scenes;
	bool every = true, with_linked = false, hides = false;
};

class ShowInScenes {
public:
	ShowInScenes(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
		std::lock_guard lock(g_switches_mutex);
		g_switches.push_back(this);
	}

	~ShowInScenes()
	{
		{
			std::lock_guard lock(g_switches_mutex);
			g_switches.erase(std::remove(g_switches.begin(), g_switches.end(), this), g_switches.end());
		}
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
	}

	// For show_in_scenes_holds_off: called with g_switches_mutex held. The
	// state is the switch's own: on or off, and what "on" means.
	SwitchLook look()
	{
		SwitchLook look;
		obs_source_t *parent = obs_filter_get_parent(source_);
		const char *own = parent ? obs_source_get_name(parent) : nullptr;
		const bool enabled = obs_source_enabled(source_);
		std::lock_guard lock(mutex_);
		look.sources = sources_;
		if (look.sources.empty() && own && *own)
			look.sources.push_back(own);
		look.scenes = scenes_;
		look.every = where_ == Where::Every;
		look.with_linked = with_linked_;
		look.hides = enabled == on_hides_; // the same rule as apply()
		return look;
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> sources = pick_list_read(settings, "sources", false);
		const std::vector<std::string> scenes = pick_list_read(settings, "scenes", false);
		std::lock_guard lock(mutex_);
		sources_ = sources;
		scenes_ = scenes;
		where_ = static_cast<Where>(std::clamp((int)obs_data_get_int(settings, "where"), 0, 1));
		on_hides_ = obs_data_get_int(settings, "on_means") == 1;
		with_linked_ = obs_data_get_bool(settings, "with_linked");
		follow_ = obs_data_get_bool(settings, "follow");
		go_to_ = obs_data_get_string(settings, "go_to");
		// How items change: the usual way, and each listed source's own.
		change_ = item_change_read(settings);
		own_change_.clear();
		for (const std::string &name : sources)
			own_change_[name] = item_change_read(settings, name.c_str());
	}

	// The state OBS restores when it loads a collection is not a press.
	// A press is acted on here, in the tick, not on the thread that made it
	// (a Stream Deck key's comes from obs-websocket's): a Link Visibility
	// acts on the same items from its tick, and two threads setting an
	// item's transitions at once could each let go of the other's. It also
	// keeps "Follow what is shown" from looking at a press half done.
	void tick(float seconds)
	{
		ready_ = true;
		const int press = press_.exchange(-1);
		if (press >= 0)
			apply(press != 0);
		// A press that turned it on also changes scene, when set to.
		if (press == 1 && scenes_loaded())
			switch_scene();
		if ((follow_timer_ -= seconds) > 0.0)
			return;
		follow_timer_ = 0.25;
		follow();
	}

	// "Follow what is shown": the switch is put the way its sources are in
	// the scenes it covers, shown only when every one of their items is. That
	// only corrects the switch: nothing is shown or hidden for it.
	void follow()
	{
		std::vector<std::string> sources, scenes;
		Where where;
		bool on_hides;
		{
			std::lock_guard lock(mutex_);
			if (!follow_)
				return;
			sources = sources_;
			scenes = scenes_;
			where = where_;
			on_hides = on_hides_;
		}
		// (Not while OBS loads its scenes: only some have their items yet.)
		if (!scenes_loaded())
			return;
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (sources.empty() && parent && obs_source_get_name(parent))
			sources.push_back(obs_source_get_name(parent));
		int found = 0, shown = 0;
		bool settling = false;
		each_item(sources, scenes, where == Where::Every,
			  [&](obs_sceneitem_t *item, const char *, obs_source_t *scene) {
				  // A mode on its way in or out is not read yet, and an item
				  // a mode not showing holds hidden is one a press would
				  // not show either.
				  if (link_visibility_settling(scene, item))
					  settling = true;
				  if (link_visibility_holds_hidden(scene, item))
					  return;
				  found++;
				  if (obs_sceneitem_visible(item))
					  shown++;
			  });
		if (settling || !found)
			return; // in none of its scenes: nothing to go by
		const bool on = (shown == found) != on_hides;
		if (obs_source_enabled(source_) == on)
			return;
		// OBS tells the filter of this change as of any other; it is known
		// by the thread it comes from, so a press made at the same moment
		// from elsewhere still counts.
		correcting_ = std::this_thread::get_id();
		obs_source_set_enabled(source_, on);
		correcting_ = std::thread::id();
		obs_log(LOG_INFO, "Show in Scenes '%s': now reads %s, as %d of %d source(s) are shown",
			obs_source_get_name(source_), on ? "on" : "off", shown, found);
	}

	// Shows or hides every item it is set to, and says how many.
	int apply(bool enabled)
	{
		std::vector<std::string> sources, scenes;
		Where where;
		bool show, with_linked;
		ItemChange change;
		std::map<std::string, ItemChange> own_change;
		{
			std::lock_guard lock(mutex_);
			change = change_;
			own_change = own_change_;
			sources = sources_;
			scenes = scenes_;
			where = where_;
			show = enabled != on_hides_;
			with_linked = with_linked_;
		}
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (sources.empty() && parent && obs_source_get_name(parent))
			sources.push_back(obs_source_get_name(parent));
		// "Bring-along items too": whatever the sources' Send To keys, and
		// Place Item filters that move them, are set to bring along.
		if (with_linked)
			add_linked(sources);
		int changed = 0;
		each_item(sources, scenes, where == Where::Every,
			  [&](obs_sceneitem_t *item, const char *name, obs_source_t *scene) {
				  if (obs_sceneitem_visible(item) == show)
					  return;
				  // Not shown in a mode of its scene that is not showing
				  // (Link Visibility's "Follow the first"); hiding is never
				  // passed over.
				  if (show && link_visibility_holds_hidden(scene, item))
					  return;
				  // A listed source's own change, or the usual one.
				  const auto own = own_change.find(name);
				  apply_item_change(item, own == own_change.end() ? change : own->second, scene);
				  set_item_visible(item, show);
				  changed++;
			  });
		obs_log(LOG_INFO, "Show in Scenes '%s': %s %d item(s)", obs_source_get_name(source_),
			show ? "showed" : "hid", changed);
		return changed;
	}

	// "Switch to scene", after a press turned the switch on.
	void switch_scene()
	{
		GoTo go;
		{
			std::lock_guard lock(mutex_);
			go.target = go_to_;
			go.sources = sources_;
		}
		if (go.target.empty())
			return;
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (go.sources.empty() && parent && obs_source_get_name(parent))
			go.sources.push_back(obs_source_get_name(parent));
		go.filter = obs_source_get_name(source_);
		go_to_scene(std::move(go));
	}

	obs_source_t *source() const { return source_; }
	bool hides_when_on()
	{
		std::lock_guard lock(mutex_);
		return on_hides_;
	}

private:
	static void enabled_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<ShowInScenes *>(data);
		// (Its own correction of the switch is not a press.)
		if (self->ready_ && self->correcting_.load() != std::this_thread::get_id())
			self->press(calldata_bool(cd, "enabled"));
	}

public:
	// Acted on at the next tick; of several presses in one frame, the last.
	// "Do it now" in the filter's window (`by_key` false) never changes scene.
	void press(bool enabled, bool by_key = true) { press_ = enabled ? (by_key ? 1 : 2) : 0; }

private:
	obs_source_t *source_;
	std::mutex mutex_;
	std::vector<std::string> sources_, scenes_;
	Where where_ = Where::Every;
	bool on_hides_ = false;
	bool with_linked_ = false;
	ItemChange change_;                            // how items change, usually
	std::map<std::string, ItemChange> own_change_; // and for each listed source
	std::atomic<bool> ready_{false};
	bool follow_ = false;                       // the switch goes by what is shown
	std::string go_to_;                         // the scene a press turning it on goes to
	double follow_timer_ = 0.0;                 // until the next look, in seconds
	std::atomic<std::thread::id> correcting_{}; // the thread putting the switch right, if any
	std::atomic<int> press_{-1};                // a press: 1 on, 2 on (no scene change), 0 off, -1 none
};

bool where_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *scenes = obs_properties_get(props, "scenes|alist");
	if (scenes)
		obs_property_set_visible(scenes, obs_data_get_int(settings, "where") == (int)Where::Listed);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<ShowInScenes *>(data);
	obs_source_t *parent = self ? obs_filter_get_parent(self->source()) : nullptr;
	obs_properties_t *props = obs_properties_create();

	const bool on = self && obs_source_enabled(self->source());
	const bool shown = self ? on != self->hides_when_on() : true;
	obs_properties_add_text(props, "status",
				obs_module_text(shown ? "ShowIn.Status.Shown" : "ShowIn.Status.Hidden"), OBS_TEXT_INFO);

	PickList sources;
	sources.setting = "sources";
	sources.title = "ShowIn.Sources";
	sources.add = "ShowIn.Sources.Add";
	sources.tip = "ShowIn.Sources.Tip";
	sources.choices = [parent](std::vector<std::string> &names) {
		struct Fill {
			std::vector<std::string> *names;
		} fill{&names};
		const auto add = [](void *param, obs_source_t *source) {
			auto *f = static_cast<Fill *>(param);
			const char *name = obs_source_get_name(source);
			if (name && *name && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) &&
			    std::find(f->names->begin(), f->names->end(), name) == f->names->end())
				f->names->push_back(name);
			return true;
		};
		// This source first, then the rest, then scenes and groups (which
		// can be items in other scenes).
		if (parent && obs_source_get_name(parent))
			names.push_back(obs_source_get_name(parent));
		obs_enum_sources(add, &fill);
		obs_enum_scenes(add, &fill);
	};
	pick_list_add(props, self ? self->source() : nullptr, sources);

	obs_property_t *where = obs_properties_add_list(props, "where", obs_module_text("ShowIn.Where"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(where, obs_module_text("ShowIn.Where.Every"), (int)Where::Every);
	obs_property_list_add_int(where, obs_module_text("ShowIn.Where.Listed"), (int)Where::Listed);
	obs_property_set_long_description(where, obs_module_text("ShowIn.Where.Tip"));
	obs_property_set_modified_callback(where, where_modified);

	PickList scenes;
	scenes.setting = "scenes";
	scenes.title = "ShowIn.Scenes";
	scenes.add = "List.AddScene";
	scenes.tip = "ShowIn.Scenes.Tip";
	scenes.choices = [](std::vector<std::string> &names) {
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
	};
	pick_list_add(props, self ? self->source() : nullptr, scenes);

	obs_property_t *means = obs_properties_add_list(props, "on_means", obs_module_text("ShowIn.OnMeans"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(means, obs_module_text("ShowIn.OnMeans.Shown"), 0);
	obs_property_list_add_int(means, obs_module_text("ShowIn.OnMeans.Hidden"), 1);
	obs_property_set_long_description(means, obs_module_text("ShowIn.OnMeans.Tip"));

	obs_property_t *with_linked =
		obs_properties_add_bool(props, "with_linked", obs_module_text("ShowIn.WithLinked"));
	obs_property_set_long_description(with_linked, obs_module_text("ShowIn.WithLinked.Tip"));

	obs_property_t *follow = obs_properties_add_bool(props, "follow", obs_module_text("ShowIn.Follow"));
	obs_property_set_long_description(follow, obs_module_text("ShowIn.Follow.Tip"));

	obs_property_t *go_to = obs_properties_add_list(props, "go_to", obs_module_text("ShowIn.GoTo"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(go_to, obs_module_text("ShowIn.GoTo.None"), "");
	{
		std::vector<std::string> names;
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
		// A scene chosen before and gone since stays chosen, by name.
		if (self) {
			obs_data_t *settings = obs_source_get_settings(self->source());
			const std::string chosen = obs_data_get_string(settings, "go_to");
			obs_data_release(settings);
			if (!chosen.empty() && std::find(names.begin(), names.end(), chosen) == names.end())
				names.push_back(chosen);
		}
		for (const std::string &name : names)
			obs_property_list_add_string(go_to, name.c_str(), name.c_str());
	}
	obs_property_set_long_description(go_to, obs_module_text("ShowIn.GoTo.Tip"));

	add_item_change_properties(props, self ? self->source() : nullptr, "sources");

	obs_properties_add_button2(
		props, "apply_now", obs_module_text("ShowIn.ApplyNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *filter = static_cast<ShowInScenes *>(d);
			if (filter)
				filter->press(obs_source_enabled(filter->source()), false);
			return false;
		},
		self);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("ShowIn.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool show_in_scenes_holds_off(obs_source_t *scene, obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	const char *scene_name = scene ? obs_source_get_name(scene) : nullptr;
	if (!name || !*name || !scene_name)
		return false;
	std::vector<SwitchLook> looks;
	{
		std::lock_guard lock(g_switches_mutex);
		for (ShowInScenes *sw : g_switches)
			looks.push_back(sw->look());
	}
	// (Bring-along items are looked up with no lock held.)
	for (SwitchLook &look : looks) {
		if (!look.hides)
			continue;
		if (!look.every && std::find(look.scenes.begin(), look.scenes.end(), scene_name) == look.scenes.end())
			continue;
		if (look.with_linked)
			add_linked(look.sources);
		if (std::find(look.sources.begin(), look.sources.end(), name) != look.sources.end())
			return true;
	}
	return false;
}

void register_show_in_scenes()
{
	obs_source_info info = {};
	info.id = kShowInScenesId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("ShowIn.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ShowInScenes(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ShowInScenes *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ShowInScenes *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "where", (int)Where::Every);
		obs_data_set_default_int(settings, "on_means", 0);
		obs_data_set_default_bool(settings, "follow", false);
		obs_data_set_default_string(settings, "go_to", "");
		item_change_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<ShowInScenes *>(data)->tick(seconds);
	};
	// A switch: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<ShowInScenes *>(data)->source());
	};
	register_timed(&info);

	static bool test_calls = false;
	if (!test_calls) {
		test_calls = true;
		proc_handler_add(
			obs_get_proc_handler(), "void stagekit_test_scene_switch(in string program)",
			[](void *, calldata_t *cd) {
				const char *program = calldata_string(cd, "program");
				std::lock_guard lock(g_test_mutex);
				g_test_switching = true;
				g_test_program = program ? program : "";
			},
			nullptr);
		proc_handler_add(
			obs_get_proc_handler(), "void stagekit_test_scene_switched(out string scenes)",
			[](void *, calldata_t *cd) {
				std::lock_guard lock(g_test_mutex);
				calldata_set_string(cd, "scenes", g_test_switched.c_str());
				g_test_switched.clear();
			},
			nullptr);
	}
}

} // namespace stagekit
