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
// "Switch to scene" (`go_to`, empty as it comes): a press that shows the
// sources (turns the switch on, or off when "On means" is hidden), once they
// are shown, also changes OBS to that scene with OBS's current transition
// (the preview in Studio Mode), unless the scene on screen already has one
// of the sources. It goes by the scene's name, so it works with any
// collection. Not on a press hiding them, not for "Do it now", not when
// "Follow what is shown" corrects the switch, not for the state restored at
// load.

#include "show-in-scenes.hpp"
#include "../common/timing.hpp"

#include "place-item.hpp"

#include "../common/go-to-scene.hpp"
#include "../common/holds-hidden.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-rev.hpp"
#include "../common/tooltip.hpp"

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

class ShowInScenes {
public:
	ShowInScenes(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
	}

	~ShowInScenes()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enabled_cb, this);
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
		look_again_ = true;
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
			apply((press & kPressOn) != 0);
		// A press that showed the sources also changes scene, when set to.
		if (press >= 0 && !(press & kPressStay) && scenes_loaded())
			switch_scene((press & kPressOn) != 0);
		if (press >= 0)
			look_again_ = true;
		if (!follow_staggered_) {
			// Each switch on a quarter second of its own, so a dozen of
			// them do not all look in the same frame.
			follow_staggered_ = true;
			follow_timer_ = stagger_phase(source_, 0.25);
		}
		if ((follow_timer_ -= seconds) > 0.0)
			return;
		follow_timer_ = 0.25;
		// Only when something it goes by may have changed: an item in any
		// scene shown, hidden, added, removed (`scene_revision`), a press,
		// its settings, loading, or a look that found a mode settling. And
		// every two seconds anyway, for what no signal tells of (a filter
		// that holds items hidden switched on or off).
		const uint64_t rev = scene_revision();
		const bool loaded = scenes_loaded();
		follow_safety_ -= 0.25;
		if (!look_again_.exchange(false) && rev == follow_rev_ && loaded == follow_loaded_ &&
		    follow_safety_ > 0.0)
			return;
		follow_rev_ = rev;
		follow_loaded_ = loaded;
		follow_safety_ = 2.0;
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
		count_look(Look::SwitchFollow);
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
				  if (mode_settling(scene, item))
					  settling = true;
				  if (mode_holds_hidden(scene, item))
					  return;
				  found++;
				  if (obs_sceneitem_visible(item))
					  shown++;
			  });
		if (settling)
			look_again_ = true; // looked at again in a quarter second
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
				  if (show && mode_holds_hidden(scene, item))
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

	// "Switch to scene", after a press that showed the sources (turned the
	// switch on, or off when "On means" is hidden).
	void switch_scene(bool enabled)
	{
		GoTo go;
		{
			std::lock_guard lock(mutex_);
			if (enabled == on_hides_)
				return; // that press hid them
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
		// The switch's state has changed: whatever goes by the scenes looks again.
		scene_revision_bump();
		// (Its own correction of the switch is not a press.)
		if (self->ready_ && self->correcting_.load() != std::this_thread::get_id())
			self->press(calldata_bool(cd, "enabled"));
	}

public:
	// Acted on at the next tick; of several presses in one frame, the last.
	// "Do it now" in the filter's window (`by_key` false) never changes scene.
	static constexpr int kPressOn = 1, kPressStay = 2;
	void press(bool enabled, bool by_key = true) { press_ = (enabled ? kPressOn : 0) | (by_key ? 0 : kPressStay); }

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
	bool follow_ = false;       // the switch goes by what is shown
	std::string go_to_;         // the scene a press turning it on goes to
	double follow_timer_ = 0.0; // until the next look, in seconds
	bool follow_staggered_ = false;
	std::atomic<bool> look_again_{true}; // the next look is not skipped
	uint64_t follow_rev_ = 0;            // scene_revision() at the last look
	bool follow_loaded_ = false;
	double follow_safety_ = 0.0;                // until a look comes whatever changed
	std::atomic<std::thread::id> correcting_{}; // the thread putting the switch right, if any
	std::atomic<int> press_{-1};                // a press (kPressOn, kPressStay), or -1 for none
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
	set_tip(where, obs_module_text("ShowIn.Where.Tip"));
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
	set_tip(means, obs_module_text("ShowIn.OnMeans.Tip"));

	obs_property_t *with_linked =
		obs_properties_add_bool(props, "with_linked", obs_module_text("ShowIn.WithLinked"));
	set_tip(with_linked, obs_module_text("ShowIn.WithLinked.Tip"));

	obs_property_t *follow = obs_properties_add_bool(props, "follow", obs_module_text("ShowIn.Follow"));
	set_tip(follow, obs_module_text("ShowIn.Follow.Tip"));

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
	set_tip(go_to, obs_module_text("ShowIn.GoTo.Tip"));

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

	go_to_scene_add_test_calls();
}

} // namespace stagekit
