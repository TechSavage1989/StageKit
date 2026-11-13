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

#include "show-in-scenes.hpp"

#include "../common/pick-list.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

enum class Where : int {
	Every = 0, // saved: append only
	Listed = 1,
};

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
	}

	// The state OBS restores when it loads a collection is not a press.
	void tick() { ready_ = true; }

	// Shows or hides every item it is set to, and says how many.
	int apply(bool enabled)
	{
		std::vector<std::string> sources, scenes;
		Where where;
		bool show;
		{
			std::lock_guard lock(mutex_);
			sources = sources_;
			scenes = scenes_;
			where = where_;
			show = enabled != on_hides_;
		}
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (sources.empty() && parent && obs_source_get_name(parent))
			sources.push_back(obs_source_get_name(parent));
		struct Work {
			const std::vector<std::string> *sources, *scenes;
			bool every, show;
			int changed = 0;
		} work{&sources, &scenes, where == Where::Every, show};
		static const auto each = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *w = static_cast<Work *>(param);
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (name && std::find(w->sources->begin(), w->sources->end(), name) != w->sources->end() &&
			    obs_sceneitem_visible(item) != w->show) {
				obs_sceneitem_set_visible(item, w->show);
				w->changed++;
			}
			// A group's own items are in the scene that holds the group.
			if (obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(
					item,
					[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
						auto *w2 = static_cast<Work *>(p);
						const char *inner_name =
							obs_source_get_name(obs_sceneitem_get_source(inner));
						if (inner_name &&
						    std::find(w2->sources->begin(), w2->sources->end(), inner_name) !=
							    w2->sources->end() &&
						    obs_sceneitem_visible(inner) != w2->show) {
							obs_sceneitem_set_visible(inner, w2->show);
							w2->changed++;
						}
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
				if (!w->every && (!name || std::find(w->scenes->begin(), w->scenes->end(), name) ==
								   w->scenes->end()))
					return true;
				obs_scene_enum_items(obs_scene_from_source(scene), each, param);
				return true;
			},
			&work);
		obs_log(LOG_INFO, "Show in Scenes '%s': %s %d item(s)", obs_source_get_name(source_),
			show ? "showed" : "hid", work.changed);
		return work.changed;
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
		if (self->ready_)
			self->apply(calldata_bool(cd, "enabled"));
	}

	obs_source_t *source_;
	std::mutex mutex_;
	std::vector<std::string> sources_, scenes_;
	Where where_ = Where::Every;
	bool on_hides_ = false;
	std::atomic<bool> ready_{false};
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

	obs_properties_add_button2(
		props, "apply_now", obs_module_text("ShowIn.ApplyNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *filter = static_cast<ShowInScenes *>(d);
			if (filter)
				filter->apply(obs_source_enabled(filter->source()));
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
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float) {
		static_cast<ShowInScenes *>(data)->tick();
	};
	// A switch: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<ShowInScenes *>(data)->source());
	};
	obs_register_source(&info);
}

} // namespace stagekit
