/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Corners: corners and edges on every source of a scene, set once.
//
// It draws nothing. Drawing the scene item by item to shape each one would
// mean doing again what OBS does for each item (crops, scale filters,
// blending, groups, item transitions to their last frame), and a mistake
// there breaks a whole scene. Instead it keeps a Corners & Edges filter on
// each source it covers (`ManagedSet`, src/common/managed-filter.*), with
// the look set here pushed to each. Corners & Edges never changes a source's
// size, so nothing around a source moves and Morph morphs as before.
//
// Switched off, its filters are switched off; on, on again: here the scene
// filter is the switch. A Corners & Edges made by hand is left alone.

#include "scene-corners.hpp"

#include "../common/loading.hpp"
#include "../common/managed-filter.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/timing.hpp"
#include "../shape/corners.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Sources covered. Saved by number: append only.
enum class Which : int { AllButBackground = 0, Chosen = 1 };

// The Corners & Edges settings set on each source's filter: the same names
// and numbers.
const LookKey kLook[] = {
	{"style", LookType::Int},        {"size", LookType::Double},           {"top_left", LookType::Bool},
	{"top_right", LookType::Bool},   {"bottom_right", LookType::Bool},     {"bottom_left", LookType::Bool},
	{"inset", LookType::Double},     {"soft", LookType::Double},           {"border", LookType::Double},
	{"border_color", LookType::Int}, {"border_opacity", LookType::Double}, {"screen_px", LookType::Bool},
};

const ManagedSpec kSpec = {kSceneCornersId,
			   kCornersFilterId,
			   "SceneCorners.FilterName",
			   "Scene Corners",
			   kLook,
			   sizeof(kLook) / sizeof(kLook[0]),
			   /*mirror_enabled=*/true};

// A source drawn over this much of the screen, under everything else, is
// taken for the background.
constexpr double kBackgroundShare = 0.9;

struct Config {
	Which which = Which::AllButBackground;
	std::vector<std::string> sources, skip;
	bool auto_background = true;
	std::string look_json;
};

// TODO(stagger): stagger_phase() once timing has it.
double phase_of(obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	unsigned hash = 2166136261u;
	for (const char *c = uuid ? uuid : ""; *c; c++)
		hash = (hash ^ (unsigned char)*c) * 16777619u;
	return (hash % 1000) / 1000.0;
}

// How much of the screen an item covers, from its size and scale (or its
// bounding box), without asking OBS to work out its box afresh.
double screen_share(obs_sceneitem_t *item)
{
	obs_video_info ovi = {};
	if (!obs_get_video_info(&ovi) || !ovi.base_width || !ovi.base_height)
		return 0.0;
	obs_source_t *source = obs_sceneitem_get_source(item);
	double w, h;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
		vec2 bounds;
		obs_sceneitem_get_bounds(item, &bounds);
		w = bounds.x;
		h = bounds.y;
	} else {
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		vec2 scale;
		obs_sceneitem_get_scale(item, &scale);
		w = std::max(0.0, (double)obs_source_get_width(source) - crop.left - crop.right) * std::fabs(scale.x);
		h = std::max(0.0, (double)obs_source_get_height(source) - crop.top - crop.bottom) * std::fabs(scale.y);
	}
	return std::min(w, (double)ovi.base_width) * std::min(h, (double)ovi.base_height) /
	       ((double)ovi.base_width * (double)ovi.base_height);
}

class SceneCorners {
public:
	SceneCorners(obs_data_t *settings, obs_source_t *source) : source_(source), managed_(kSpec, source)
	{
		timer_ = phase_of(source);
		update(settings);
	}
	~SceneCorners() { managed_.withdraw(); }

	void update(obs_data_t *settings)
	{
		auto config = std::make_shared<Config>();
		config->which = static_cast<Which>(std::clamp((int)obs_data_get_int(settings, "which"), 0, 1));
		config->sources = pick_list_read(settings, "sources", false);
		config->skip = pick_list_read(settings, "skip", false);
		config->auto_background = obs_data_get_bool(settings, "auto_background");
		obs_data_t *look = obs_data_create();
		for (const LookKey &key : kLook) {
			switch (key.type) {
			case LookType::Int:
				obs_data_set_int(look, key.name, obs_data_get_int(settings, key.name));
				break;
			case LookType::Double:
				obs_data_set_double(look, key.name, obs_data_get_double(settings, key.name));
				break;
			case LookType::Bool:
				obs_data_set_bool(look, key.name, obs_data_get_bool(settings, key.name));
				break;
			case LookType::String:
				obs_data_set_string(look, key.name, obs_data_get_string(settings, key.name));
				break;
			}
		}
		config->look_json = obs_data_get_json(look);
		obs_data_release(look);
		std::lock_guard lock(mutex_);
		config_ = config;
		dirty_ = true;
	}

	void tick(float seconds)
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent || obs_source_is_group(parent) || !obs_scene_from_source(parent))
			return;
		const bool enabled = obs_source_enabled(source_);
		bool dirty;
		{
			std::lock_guard lock(mutex_);
			dirty = dirty_ || enabled != was_enabled_;
		}
		timer_ += seconds;
		const double every = obs_source_showing(parent) ? 1.0 : 3.0;
		if (!dirty && timer_ < every)
			return;
		if (!scenes_loaded())
			return;
		timer_ = 0.0;
		act(parent, enabled);
	}

	void detach()
	{
		managed_.withdraw();
		std::lock_guard lock(mutex_);
		dirty_ = true;
	}

	std::string status()
	{
		std::lock_guard lock(mutex_);
		return status_;
	}

	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return obs_filter_get_parent(source_); }

private:
	void act(obs_source_t *scene, bool enabled)
	{
		std::shared_ptr<const Config> config;
		{
			std::lock_guard lock(mutex_);
			config = config_;
			dirty_ = false;
			was_enabled_ = enabled;
		}
		std::set<std::string> covered;
		if (config->which == Which::Chosen) {
			covered.insert(config->sources.begin(), config->sources.end());
		} else {
			// Every item straight in the scene (a group counts as one
			// source), bottom first, leaving out the background: what
			// covers most of the screen under everything else.
			std::vector<obs_sceneitem_t *> items;
			obs_scene_enum_items(
				obs_scene_from_source(scene),
				[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
					obs_sceneitem_addref(item);
					static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
					return true;
				},
				&items);
			bool under_all = true;
			for (obs_sceneitem_t *item : items) {
				obs_source_t *source = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(source);
				if (!name || !*name || is_morph_layer(item) || is_stand_in(source) ||
				    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
					continue;
				if (under_all && config->auto_background && screen_share(item) >= kBackgroundShare)
					continue;
				under_all = false;
				if (std::find(config->skip.begin(), config->skip.end(), name) != config->skip.end())
					continue;
				covered.insert(name);
			}
			for (obs_sceneitem_t *item : items)
				obs_sceneitem_release(item);
		}
		obs_data_t *look = obs_data_create_from_json(config->look_json.c_str());
		managed_.publish(scene, enabled, covered, covered, look);
		obs_data_release(look);
		managed_.sync(scene, os_gettime_ns() / 1e9);

		std::string made, own;
		for (const std::string &name : covered) {
			std::string &into = managed_.own_ones().count(name) ? own : made;
			into += (into.empty() ? "" : ", ") + name;
		}
		std::string text = made.empty() ? obs_module_text("SceneCorners.Status.None")
						: text_with("SceneCorners.Status.Covers", made);
		if (!own.empty())
			text += " " + text_with("SceneCorners.Status.Own", own);
		std::lock_guard lock(mutex_);
		status_ = text;
	}

	obs_source_t *source_;
	ManagedSet managed_;
	std::mutex mutex_;
	std::shared_ptr<const Config> config_;
	bool dirty_ = true;
	bool was_enabled_ = false;
	double timer_ = 0.0;
	std::string status_;
};

bool which_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool chosen = obs_data_get_int(settings, "which") == (int)Which::Chosen;
	obs_property_set_visible(obs_properties_get(props, "sources|alist"), chosen);
	obs_property_set_visible(obs_properties_get(props, "skip|alist"), !chosen);
	obs_property_set_visible(obs_properties_get(props, "auto_background"), !chosen);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneCorners *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *scene = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();
	if (scene && obs_source_is_group(scene)) {
		obs_property_t *wrong = obs_properties_add_text(props, "on_group",
								obs_module_text("SceneCorners.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(wrong, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(wrong, true);
	}
	const std::string now = self ? self->status() : std::string();
	obs_property_t *status = obs_properties_add_text(
		props, "status", now.empty() ? obs_module_text("SceneCorners.Status.None") : now.c_str(),
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);

	obs_property_t *which = obs_properties_add_list(props, "which", obs_module_text("SceneCorners.Which"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(which, obs_module_text("SceneCorners.Which.All"), (int)Which::AllButBackground);
	obs_property_list_add_int(which, obs_module_text("SceneCorners.Which.Chosen"), (int)Which::Chosen);
	obs_property_set_long_description(which, obs_module_text("SceneCorners.Which.Tip"));
	obs_property_set_modified_callback(which, which_modified);

	auto items = [scene](std::vector<std::string> &names) {
		obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
		if (!s)
			return;
		obs_scene_enum_items(
			s,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *src = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(src);
				if (name && *name && !is_stand_in(src) && !is_morph_layer(item))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
	};
	PickList sources;
	sources.setting = "sources";
	sources.title = "SceneCorners.Sources";
	sources.add = "List.Choose";
	sources.tip = "SceneCorners.Sources.Tip";
	sources.choices = items;
	pick_list_add(props, owner, sources);
	PickList skip;
	skip.setting = "skip";
	skip.title = "SceneCorners.Skip";
	skip.add = "List.Choose";
	skip.tip = "SceneCorners.Skip.Tip";
	skip.choices = items;
	pick_list_add(props, owner, skip);
	obs_property_t *background =
		obs_properties_add_bool(props, "auto_background", obs_module_text("SceneCorners.AutoBackground"));
	obs_property_set_long_description(background, obs_module_text("SceneCorners.AutoBackground.Tip"));

	corners_add_look_properties(props);

	obs_property_t *help =
		obs_properties_add_text(props, "help", obs_module_text("SceneCorners.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_scene_corners()
{
	obs_source_info info = {};
	info.id = kSceneCornersId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneCorners.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneCorners(settings, source);
	};
	info.destroy = [](void *data) {
		// (Its filters on the sources stay: this also runs when OBS changes
		// scene collection or closes.)
		delete static_cast<SceneCorners *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneCorners *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "which", (int)Which::AllButBackground);
		obs_data_set_default_bool(settings, "auto_background", true);
		corners_look_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneCorners *>(data)->tick(seconds);
	};
	// It never changes the picture: its filters on the sources do.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<SceneCorners *>(data)->source());
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneCorners *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
