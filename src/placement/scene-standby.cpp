/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Standby: one place to set the standby image for every capture of a
// scene. The owner's promise: "any hidden or empty capture in the scene shows
// the standby image, without a filter per capture."
//
// It draws nothing. It keeps a Standby Image filter on each source it covers
// (`ManagedSet`, src/common/managed-filter.*), which draws the image at the
// source's own size, before its shadow and corners and after its Auto Frame,
// as a filter made by hand does. The empty image is a matter of the source,
// so it follows the source into every scene; the image in a *hidden*
// source's place is a matter of the scene (a layout scene hides its stacked
// captures on purpose), so the Standby filter asks `scene_standby_hidden`
// scene by scene.

#include "scene-standby.hpp"

#include "../capture/standby.hpp"
#include "../common/filter-input.hpp"
#include "../common/easing.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Sources covered. Saved by number: append only. A filter left on the
// default has no number saved and reads as Every capture, the default it
// had before All sources was added and still has.
enum class Which : int { AllCaptures = 0, Chosen = 1, AllSources = 2 };

// The Standby Image settings the scene filter sets on each source's filter.
// The same names and numbers as the Standby Image filter's own.
const LookKey kLook[] = {
	{"picture", LookType::Int},     {"file", LookType::String},    {"source", LookType::String},
	{"own_shape", LookType::Bool},  {"style", LookType::Int},      {"fade_ms", LookType::Int},
	{"back_ms", LookType::Int},     {"ease_curve", LookType::Int}, {"ease_mode", LookType::Int},
	{"glide_size", LookType::Bool},
};

const ManagedSpec kSpec = {kSceneStandbyId,
			   kStandbyFilterId,
			   "SceneStandby.FilterName",
			   "Scene Standby",
			   kLook,
			   sizeof(kLook) / sizeof(kLook[0]),
			   /*mirror_enabled=*/false};

// What one enabled Scene Standby lists under "Show when hidden", for the
// Standby filters asking from their ticks. No lock is held while OBS is
// asked anything.
struct HiddenEntry {
	obs_weak_source_t *scene = nullptr;
	bool enabled = false;
	std::set<std::string> hidden;
	ItemChange change;
	std::map<std::string, ItemChange> per_source;
};
std::mutex g_hidden_mutex;
std::map<obs_source_t *, HiddenEntry> g_hidden;

// The settings, as read in `update`: swapped whole, read on the tick.
struct Config {
	Which which = Which::AllCaptures;
	std::vector<std::string> sources, skip, hidden;
	std::string image_source; // the source shown as the image, never covered itself
	bool screens = false, when_empty = true;
	std::string look_json;
	ItemChange change;
	std::map<std::string, ItemChange> per_source;
};

// A spread of the filters' looks over the second, so several scenes do not
// all look in the same tick. TODO(stagger): stagger_phase() once timing has it.
double phase_of(obs_source_t *source)
{
	const char *uuid = obs_source_get_uuid(source);
	unsigned hash = 2166136261u;
	for (const char *c = uuid ? uuid : ""; *c; c++)
		hash = (hash ^ (unsigned char)*c) * 16777619u;
	return (hash % 1000) / 1000.0;
}

class SceneStandby {
public:
	SceneStandby(obs_data_t *settings, obs_source_t *source) : source_(source), managed_(kSpec, source)
	{
		timer_ = phase_of(source);
		update(settings);
	}
	~SceneStandby() { withdraw(); }

	void update(obs_data_t *settings)
	{
		auto config = std::make_shared<Config>();
		config->which = static_cast<Which>(std::clamp((int)obs_data_get_int(settings, "which"), 0, 2));
		if (obs_data_get_int(settings, "picture") == 2)
			config->image_source = obs_data_get_string(settings, "source");
		config->sources = pick_list_read(settings, "sources", false);
		config->skip = pick_list_read(settings, "skip", false);
		config->hidden = pick_list_read(settings, "hidden", false);
		config->screens = obs_data_get_bool(settings, "screens");
		config->when_empty = obs_data_get_bool(settings, "when_empty");
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
		config->change = item_change_read(settings);
		for (const std::string &name : config->hidden)
			config->per_source[name] = item_change_read(settings, name.c_str());
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
		// Once a second on screen, every three off it, at once after a change.
		timer_ += seconds;
		const double every = obs_source_showing(parent) ? 1.0 : 3.0;
		if (!dirty && timer_ < every)
			return;
		// (Not while OBS loads its scenes: see loading.hpp.)
		if (!scenes_loaded())
			return;
		timer_ = 0.0;
		act(parent, enabled);
	}

	void withdraw()
	{
		managed_.withdraw();
		std::lock_guard lock(g_hidden_mutex);
		auto found = g_hidden.find(source_);
		if (found != g_hidden.end()) {
			obs_weak_source_release(found->second.scene);
			g_hidden.erase(found);
		}
	}

	// Taken off its scene (or put back, which comes to the same next tick).
	void detach()
	{
		withdraw();
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
		// Which sources: the captures of the scene (groups opened), every
		// source of it (groups opened too), or the ones chosen.
		std::set<std::string> covered;
		if (config->which == Which::Chosen) {
			covered.insert(config->sources.begin(), config->sources.end());
		} else {
			struct Walk {
				const Config *config;
				std::set<std::string> *covered;
			} walk{config.get(), &covered};
			static const auto look = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *w = static_cast<Walk *>(param);
				obs_source_t *source = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(source);
				if (!name || !*name || is_morph_layer(item) || is_stand_in(source) ||
				    std::find(w->config->skip.begin(), w->config->skip.end(), name) !=
					    w->config->skip.end())
					return true;
				if (w->config->which == Which::AllSources) {
					// Every source with an image of its own: not a group or a
					// nested scene (never empty: a scene is the canvas's
					// size), not a private one, not the standby image itself.
					if (obs_obj_is_private(source) || obs_scene_from_source(source) ||
					    obs_group_from_source(source) ||
					    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) ||
					    w->config->image_source == name)
						return true;
				} else if (!is_capture_source(source) ||
					   (!w->config->screens && is_screen_capture(source))) {
					return true;
				}
				w->covered->insert(name);
				return true;
			};
			obs_scene_enum_items(
				obs_scene_from_source(scene),
				[](obs_scene_t *s, obs_sceneitem_t *item, void *param) {
					look(s, item, param);
					if (obs_sceneitem_is_group(item))
						obs_sceneitem_group_enum_items(item, look, param);
					return true;
				},
				&walk);
		}
		// Shown in a hidden source's place: only items straight in the scene
		// (stand-ins are not made inside groups yet).
		std::set<std::string> hidden, grouped;
		for (const std::string &name : config->hidden) {
			obs_sceneitem_t *straight = obs_scene_find_source(obs_scene_from_source(scene), name.c_str());
			if (straight)
				hidden.insert(name);
			else
				grouped.insert(name);
		}
		std::set<std::string> kept = covered;
		kept.insert(hidden.begin(), hidden.end());

		obs_data_t *look = obs_data_create_from_json(config->look_json.c_str());
		managed_.publish(scene, enabled, kept, config->when_empty ? covered : std::set<std::string>(), look);
		obs_data_release(look);
		{
			HiddenEntry entry;
			entry.scene = obs_source_get_weak_source(scene);
			entry.enabled = enabled;
			entry.hidden = hidden;
			entry.change = config->change;
			for (const std::string &name : hidden) {
				auto found = config->per_source.find(name);
				entry.per_source[name] = found != config->per_source.end() ? found->second
											   : config->change;
			}
			std::lock_guard lock(g_hidden_mutex);
			auto &slot = g_hidden[source_];
			obs_weak_source_release(slot.scene);
			slot = std::move(entry);
		}
		managed_.sync(scene, os_gettime_ns() / 1e9);

		// What the window says.
		auto join = [](const std::set<std::string> &names) {
			std::string text;
			for (const std::string &name : names)
				text += (text.empty() ? "" : ", ") + name;
			return text;
		};
		std::set<std::string> made;
		for (const std::string &name : kept)
			if (!managed_.own_ones().count(name))
				made.insert(name);
		std::string text = made.empty() ? obs_module_text("SceneStandby.Status.None")
						: text_with("SceneStandby.Status.Covers", join(made));
		if (!managed_.own_ones().empty())
			text += " " + text_with("SceneStandby.Status.Own", join(managed_.own_ones()));
		if (!grouped.empty())
			text += " " + text_with("SceneStandby.Status.Grouped", join(grouped));
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
	const int which = (int)obs_data_get_int(settings, "which");
	const bool chosen = which == (int)Which::Chosen;
	obs_property_set_visible(obs_properties_get(props, "sources|alist"), chosen);
	obs_property_set_visible(obs_properties_get(props, "skip|alist"), !chosen);
	obs_property_set_visible(obs_properties_get(props, "screens"), which == (int)Which::AllCaptures);
	return true;
}

bool picture_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const int kind = (int)obs_data_get_int(settings, "picture");
	obs_property_set_visible(obs_properties_get(props, "file"), kind == 1);
	obs_property_set_visible(obs_properties_get(props, "source"), kind == 2);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneStandby *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *scene = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();
	if (scene && obs_source_is_group(scene)) {
		add_on_group_warning(props, obs_module_text("SceneStandby.OnGroup"));
	}
	const std::string now = self ? self->status() : std::string();
	obs_property_t *status = obs_properties_add_text(
		props, "status", now.empty() ? obs_module_text("SceneStandby.Status.None") : now.c_str(),
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);
	set_tip(status, obs_module_text("SceneStandby.Status.Tip"));

	obs_property_t *which = obs_properties_add_list(props, "which", obs_module_text("SceneStandby.Which"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// All sources first, as in every scene filter; the numbers stay as saved.
	obs_property_list_add_int(which, obs_module_text("SceneStandby.Which.Every"), (int)Which::AllSources);
	obs_property_list_add_int(which, obs_module_text("SceneStandby.Which.All"), (int)Which::AllCaptures);
	obs_property_list_add_int(which, obs_module_text("SceneStandby.Which.Chosen"), (int)Which::Chosen);
	set_tip(which, obs_module_text("SceneStandby.Which.Tip"));
	obs_property_set_modified_callback(which, which_modified);

	auto video_sources = [scene](std::vector<std::string> &names) {
		struct Fill {
			std::vector<std::string> *names;
			obs_source_t *scene;
		} fill{&names, scene};
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto *f = static_cast<Fill *>(param);
				const char *name = obs_source_get_name(source);
				if (name && *name && source != f->scene && !is_stand_in(source) &&
				    (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
					f->names->push_back(name);
				return true;
			},
			&fill);
	};
	PickList sources;
	sources.setting = "sources";
	sources.title = "SceneStandby.Sources";
	sources.add = "List.Choose";
	sources.tip = "SceneStandby.Sources.Tip";
	sources.choices = [scene](std::vector<std::string> &names) {
		scene_item_names(scene, names);
	};
	pick_list_add(props, owner, sources);

	PickList skip;
	skip.setting = "skip";
	skip.title = "SceneStandby.Skip";
	skip.add = "List.Choose";
	skip.tip = "SceneStandby.Skip.Tip";
	skip.choices = [scene](std::vector<std::string> &names) {
		scene_item_names(scene, names);
	};
	pick_list_add(props, owner, skip);
	obs_property_t *screens = obs_properties_add_bool(props, "screens", obs_module_text("SceneStandby.Screens"));
	set_tip(screens, obs_module_text("SceneStandby.Screens.Tip"));

	// The image: the Standby Image filter's own controls, set here for every
	// source. It is the same image in both cases below.
	obs_property_t *picture = obs_properties_add_list(props, "picture", obs_module_text("Standby.Picture"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Start"), 0);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.File"), 1);
	obs_property_list_add_int(picture, obs_module_text("Standby.Picture.Source"), 2);
	set_tip(picture, obs_module_text("Standby.Picture.Tip"));
	obs_property_set_modified_callback(picture, picture_modified);
	obs_properties_add_path(props, "file", obs_module_text("Standby.File"), OBS_PATH_FILE,
				obs_module_text("Standby.File.Filter"), nullptr);
	obs_property_t *source = obs_properties_add_list(props, "source", obs_module_text("Standby.Source"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(source, obs_module_text("Standby.Source.None"), "");
	{
		std::vector<std::string> names;
		video_sources(names);
		for (const std::string &name : names)
			obs_property_list_add_string(source, name.c_str(), name.c_str());
	}
	obs_property_t *own_shape = obs_properties_add_bool(props, "own_shape", obs_module_text("Standby.OwnShape"));
	set_tip(own_shape, obs_module_text("Standby.OwnShape.Tip"));
	// When a source has no image: the group's own tick box is "when_empty",
	// the switch for all of it.
	obs_properties_t *no_image = obs_properties_create();
	obs_property_t *style = obs_properties_add_list(no_image, "style", obs_module_text("Standby.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// The Standby Image's own transitions, by its numbers, in the one order
	// of every such list: fade, morph, fade and zoom, cut, slides, wipes.
	static const struct {
		int value;
		const char *text;
	} styles[] = {{0, "Standby.Style.Fade"},      {11, "Standby.Style.Morph"},    {10, "Standby.Style.Zoom"},
		      {1, "Standby.Style.Cut"},       {2, "Standby.Style.SlideLeft"}, {3, "Standby.Style.SlideRight"},
		      {4, "Standby.Style.SlideUp"},   {5, "Standby.Style.SlideDown"}, {6, "Standby.Style.WipeLeft"},
		      {7, "Standby.Style.WipeRight"}, {8, "Standby.Style.WipeUp"},    {9, "Standby.Style.WipeDown"}};
	for (const auto &one : styles)
		obs_property_list_add_int(style, obs_module_text(one.text), one.value);
	set_tip(style, obs_module_text("Standby.Style.Tip"));
	obs_property_t *fade =
		obs_properties_add_int_slider(no_image, "fade_ms", obs_module_text("Standby.Fade"), 0, 5000, 50);
	obs_property_int_set_suffix(fade, " ms");
	set_tip(fade, obs_module_text("Standby.Fade.Tip"));
	obs_property_t *back =
		obs_properties_add_int_slider(no_image, "back_ms", obs_module_text("Standby.Back"), 0, 5000, 50);
	obs_property_int_set_suffix(back, " ms");
	set_tip(back, obs_module_text("Standby.Back.Tip"));
	add_easing_properties(no_image, "");
	obs_property_t *glide = obs_properties_add_bool(no_image, "glide_size", obs_module_text("Standby.GlideSize"));
	set_tip(glide, obs_module_text("Standby.GlideSize.Tip"));
	obs_property_t *empty = obs_properties_add_group(props, "when_empty", obs_module_text("SceneStandby.WhenEmpty"),
							 OBS_GROUP_CHECKABLE, no_image);
	set_tip(empty, obs_module_text("SceneStandby.WhenEmpty.Tip"));

	// When a source is hidden: in its place, in this scene.
	obs_properties_t *when_hidden = obs_properties_create();
	PickList hidden;
	hidden.setting = "hidden";
	hidden.title = "SceneStandby.Hidden";
	hidden.add = "List.Choose";
	hidden.tip = "SceneStandby.Hidden.Tip";
	hidden.choices = [scene](std::vector<std::string> &names) {
		obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
		if (!s)
			return;
		obs_scene_enum_items(
			s,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *src = obs_sceneitem_get_source(item);
				const char *name = obs_source_get_name(src);
				if (name && *name && !is_stand_in(src) && !is_morph_layer(item) &&
				    !obs_sceneitem_is_group(item))
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
	};
	pick_list_add(when_hidden, owner, hidden);
	add_item_change_properties(when_hidden, owner, "hidden");
	obs_properties_add_group(props, "hidden_group", obs_module_text("SceneStandby.HiddenGroup"), OBS_GROUP_NORMAL,
				 when_hidden);

	obs_property_t *help =
		obs_properties_add_text(props, "help", obs_module_text("SceneStandby.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool scene_standby_hidden(obs_source_t *scene, obs_source_t *source, ItemChange *change)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!scene || !name)
		return false;
	std::lock_guard lock(g_hidden_mutex);
	for (const auto &[self, entry] : g_hidden) {
		if (!entry.enabled || !entry.hidden.count(name) ||
		    !obs_weak_source_references_source(entry.scene, scene))
			continue;
		if (change) {
			auto found = entry.per_source.find(name);
			*change = found != entry.per_source.end() ? found->second : entry.change;
		}
		return true;
	}
	return false;
}

bool scene_standby_any_hidden(obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!name)
		return false;
	std::lock_guard lock(g_hidden_mutex);
	for (const auto &[self, entry] : g_hidden)
		if (entry.enabled && entry.hidden.count(name))
			return true;
	return false;
}

ManagedState scene_standby_state(obs_source_t *source, const char *managed_by)
{
	return managed_state(kSceneStandbyId, source, managed_by);
}

void register_scene_standby()
{
	obs_source_info info = {};
	info.id = kSceneStandbyId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneStandby.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new SceneStandby(settings, source);
	};
	info.destroy = [](void *data) {
		// (Its filters on the sources stay: this also runs when OBS changes
		// scene collection or closes. They go idle, and another Scene
		// Standby's tick takes them off.)
		delete static_cast<SceneStandby *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<SceneStandby *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		// (Not All sources: a text with no words or a stopped video would
		// show the image, and every filter saved with the default would
		// change.)
		obs_data_set_default_int(settings, "which", (int)Which::AllCaptures);
		obs_data_set_default_bool(settings, "screens", false);
		obs_data_set_default_bool(settings, "when_empty", true);
		obs_data_set_default_int(settings, "picture", 0);
		obs_data_set_default_int(settings, "fade_ms", 800);
		obs_data_set_default_int(settings, "back_ms", 800);
		obs_data_set_default_int(settings, "style", 0);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
		obs_data_set_default_bool(settings, "glide_size", true);
		obs_data_set_default_bool(settings, "own_shape", true);
		item_change_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneStandby *>(data)->tick(seconds);
	};
	// It never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		skip_filter_input(static_cast<SceneStandby *>(data)->source());
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneStandby *>(data)->detach();
	};
	register_timed(&info);
}

} // namespace stagekit
