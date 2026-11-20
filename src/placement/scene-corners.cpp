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
//
// *Corners on: The whole scene* (`corners_area` 1): the scene's own image is
// cut to the shape at the edges of the screen, as one shape, by this filter
// (CornersPaint, what Corners & Edges draws with), once a frame. Its sources
// get no filter, and the ones it had added go after the usual grace. Then it
// draws: it keeps itself after every Scene Shadow and Scene Blur so that they
// still draw and the shape is cut last, and Morph carries the shape through a
// change of scene (scene_corners_now).
#include "scene-corners.hpp"

#include "../common/filter-input.hpp"
#include "../common/filter-order.hpp"
#include "../common/loading.hpp"
#include "../common/managed-filter.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/stand-in.hpp"
#include "../common/timing.hpp"
#include "../look/scene-blur.hpp"
#include "../look/scene-shadow.hpp"
#include "../shape/corners.hpp"
#include "../transition/morph.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Sources covered. Saved by number: append only. A filter left on the
// default has no number saved and reads as All but the background, the
// default it had before All sources was added and still has.
enum class Which : int { AllButBackground = 0, Chosen = 1, AllSources = 2 };

// Corners on. Saved by number: append only. 0 is what every filter saved
// before it did.
enum class Area : int { Each = 0, Whole = 1 };

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
	Area area = Area::Each;
	Which which = Which::AllSources;
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

class SceneCorners {
public:
	SceneCorners(obs_data_t *settings, obs_source_t *source) : source_(source), managed_(kSpec, source)
	{
		timer_ = phase_of(source);
		order_timer_ = timer_ / 2.0;
		update(settings);
	}
	~SceneCorners()
	{
		managed_.withdraw();
		paint_.reset();
		if (tr_input_) {
			obs_enter_graphics();
			gs_texrender_destroy(tr_input_);
			obs_leave_graphics();
		}
	}

	// The whole scene's shape, while this filter draws it.
	bool whole() const { return whole_.load(); }
	CornerShape shape()
	{
		std::lock_guard lock(mutex_);
		return shape_;
	}
	void render();

	void update(obs_data_t *settings)
	{
		const int old_scope = (int)obs_data_get_int(settings, "which");
		if (!obs_data_get_bool(settings, "coverage_v2")) {
			// The old All sources choice included full-screen items even if the hidden box was ticked.
			if (obs_data_has_user_value(settings, "which") && old_scope == (int)Which::AllSources)
				obs_data_set_bool(settings, "auto_background", false);
			obs_data_set_bool(settings, "coverage_v2", true);
		}
		if (old_scope == (int)Which::AllButBackground)
			obs_data_set_int(settings, "which", (int)Which::AllSources);
		auto config = std::make_shared<Config>();
		config->area = static_cast<Area>(std::clamp((int)obs_data_get_int(settings, "corners_area"), 0, 1));
		config->which = static_cast<Which>(std::clamp((int)obs_data_get_int(settings, "which"), 0, 2));
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
		const CornerShape shape = read_corner_shape(settings);
		std::lock_guard lock(mutex_);
		config_ = config;
		shape_ = shape;
		whole_ = config->area == Area::Whole;
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
		order_timer_ += seconds;
		if (whole_ && enabled && order_timer_ >= 0.5 && scenes_loaded()) {
			order_timer_ = 0.0;
			keep_last(parent);
		}
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
		if (config->area == Area::Whole) {
			// The scene is shaped as a whole: no source keeps a filter.
		} else if (config->which == Which::Chosen) {
			covered.insert(config->sources.begin(), config->sources.end());
		} else {
			// Every item straight in the scene (a group counts as one
			// source), bottom first. All but the background also leaves
			// out what covers most of the screen under everything else;
			// All sources leaves out only the ignored ones.
			const bool find_background = config->auto_background;
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
				    obs_obj_is_private(source) ||
				    !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
					continue;
				if (under_all && find_background && screen_share(item) >= kBackgroundShare)
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
		std::string text = config->area == Area::Whole ? obs_module_text("SceneCorners.Status.Whole")
				   : made.empty()              ? obs_module_text("SceneCorners.Status.None")
							       : text_with("SceneCorners.Status.Covers", made);
		if (!own.empty())
			text += " " + text_with("SceneCorners.Status.Own", own);
		std::lock_guard lock(mutex_);
		status_ = text;
	}

	// The whole scene's shape goes on last: after every Scene Shadow and
	// Scene Blur, which stand aside when a filter that draws comes before
	// them (as Scene Blur keeps itself after a Scene Shadow).
	void keep_last(obs_source_t *parent)
	{
		struct Last {
			obs_source_t *found = nullptr;
		} last;
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *f, void *param) {
				if (is_scene_shadow(f) || is_scene_blur(f))
					static_cast<Last *>(param)->found = f;
			},
			&last);
		if (!last.found || applied_index(parent, source_) > applied_index(parent, last.found))
			return;
		for (int i = 0; i < 64; i++) {
			const int mine = applied_index(parent, source_);
			if (mine < 0 || mine > applied_index(parent, last.found))
				break;
			obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_DOWN);
			if (applied_index(parent, source_) == mine)
				break;
		}
		obs_log(LOG_INFO, "Scene Corners '%s': moved after the filter '%s', so that both draw",
			obs_source_get_name(source_), obs_source_get_name(last.found));
	}

	obs_source_t *source_;
	ManagedSet managed_;
	std::mutex mutex_;
	std::shared_ptr<const Config> config_;
	CornerShape shape_;
	std::atomic<bool> whole_{false};
	bool dirty_ = true;
	bool was_enabled_ = false;
	double timer_ = 0.0;
	double order_timer_ = 0.0;
	std::string status_;

	// On the drawing thread only.
	std::unique_ptr<CornersPaint> paint_;
	gs_texrender_t *tr_input_ = nullptr;
	bool have_out_ = false;
	uint64_t last_frame_ = 0;
	uint32_t last_cx_ = 0, last_cy_ = 0;
	bool last_linear_ = false;
};

void SceneCorners::render()
{
	obs_source_t *parent = obs_filter_get_parent(source_);
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t cx = target ? obs_source_get_base_width(target) : 0;
	const uint32_t cy = target ? obs_source_get_base_height(target) : 0;
	if (!whole_ || !parent || obs_source_is_group(parent) || !obs_scene_from_source(parent) || !cx || !cy) {
		have_out_ = false;
		skip_filter_input(source_);
		return;
	}
	if (!paint_) {
		paint_ = std::make_unique<CornersPaint>();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	}
	if (!paint_->ready()) {
		skip_filter_input(source_);
		return;
	}
	// Once a frame: the multiview, a projector and Studio Mode draw a scene
	// again in the same frame, as it was.
	const uint64_t frame = obs_get_video_frame_time();
	const bool linear = gs_get_linear_srgb();
	if (have_out_ && frame == last_frame_ && cx == last_cx_ && cy == last_cy_ && linear == last_linear_) {
		paint_->draw_out(cx, cy);
		return;
	}
	have_out_ = false;
	if (!draw_filter_input(source_, tr_input_, cx, cy)) {
		skip_filter_input(source_);
		return;
	}
	const CornerShape s = shape();
	// Sizes in screen pixels: the scene is the screen.
	if (!paint_->shape(gs_texrender_get_texture(tr_input_), cx, cy, 0.0, 0.0, cx, cy, 1.0, 1.0, s)) {
		skip_filter_input(source_);
		return;
	}
	paint_->draw_out(cx, cy);
	have_out_ = true;
	last_frame_ = frame;
	last_cx_ = cx;
	last_cy_ = cy;
	last_linear_ = linear;
}

bool area_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool whole = obs_data_get_int(settings, "corners_area") == (int)Area::Whole;
	const int which = (int)obs_data_get_int(settings, "which");
	const bool chosen = which == (int)Which::Chosen;
	obs_property_set_visible(obs_properties_get(props, "which"), !whole);
	obs_property_set_visible(obs_properties_get(props, "sources|alist"), !whole && chosen);
	obs_property_set_visible(obs_properties_get(props, "skip|alist"), !whole && !chosen);
	obs_property_set_visible(obs_properties_get(props, "auto_background"),
				 !whole && !chosen);
	obs_property_set_visible(obs_properties_get(props, "screen_px"), !whole);
	return true;
}

bool which_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	return area_modified(props, p, settings);
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<SceneCorners *>(data);
	obs_source_t *owner = self ? self->source() : nullptr;
	obs_source_t *scene = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();
	if (scene && obs_source_is_group(scene)) {
		add_on_group_warning(props, obs_module_text("SceneCorners.OnGroup"));
	}
	const std::string now = self ? self->status() : std::string();
	obs_property_t *status = obs_properties_add_text(
		props, "status", now.empty() ? obs_module_text("SceneCorners.Status.None") : now.c_str(),
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(status, true);

	obs_property_t *area = obs_properties_add_list(props, "corners_area", obs_module_text("SceneCorners.Area"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(area, obs_module_text("SceneCorners.Area.Each"), (int)Area::Each);
	obs_property_list_add_int(area, obs_module_text("SceneCorners.Area.Whole"), (int)Area::Whole);
	set_tip(area, obs_module_text("SceneCorners.Area.Tip"));
	obs_property_set_modified_callback(area, area_modified);

	obs_property_t *which = obs_properties_add_list(props, "which", obs_module_text("SceneCorners.Which"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// All sources first, as in every scene filter; the numbers stay as saved.
	obs_property_list_add_int(which, obs_module_text("SceneCorners.Which.Every"), (int)Which::AllSources);
	obs_property_list_add_int(which, obs_module_text("SceneCorners.Which.Chosen"), (int)Which::Chosen);
	set_tip(which, obs_module_text("SceneCorners.Which.Tip"));
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
	set_tip(background, obs_module_text("SceneCorners.AutoBackground.Tip"));

	corners_add_look_properties(props);

	obs_property_t *help =
		obs_properties_add_text(props, "help", obs_module_text("SceneCorners.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

bool scene_corners_draws(obs_source_t *filter)
{
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	if (!id || std::strcmp(id, kSceneCornersId) != 0)
		return false;
	auto *self = static_cast<SceneCorners *>(obs_obj_get_data(filter));
	return self && self->whole();
}

bool scene_corners_now(obs_source_t *scene, CornerShape &out)
{
	struct Find {
		CornerShape *out;
		bool found = false;
	} find{&out};
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (x->found || !obs_source_enabled(f) || !scene_corners_draws(f))
				return;
			*x->out = static_cast<SceneCorners *>(obs_obj_get_data(f))->shape();
			x->found = true;
		},
		&find);
	return find.found;
}

void register_scene_corners()
{
	obs_source_info info = {};
	info.id = kSceneCornersId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// (Colour-aware, like every StageKit filter that draws: it does with
	// *The whole scene*.)
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
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
		// (Not All sources: it would round a full-screen background's
		// corners, and change every filter saved with the default.)
		obs_data_set_default_int(settings, "which", (int)Which::AllSources);
		obs_data_set_default_bool(settings, "auto_background", true);
		obs_data_set_default_int(settings, "corners_area", (int)Area::Each);
		corners_look_defaults(settings);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<SceneCorners *>(data)->tick(seconds);
	};
	// With Each source it never changes the picture: its filters on the
	// sources do. With The whole scene it cuts the scene's image.
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<SceneCorners *>(data)->render();
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<SceneCorners *>(data)->detach();
	};
	register_timed(&info);
	// A scene shaped as a whole is still morphed item by item; Morph cuts
	// the change to the shape (scene_corners_now).
	morph_accept_drawing_filter(scene_corners_draws);
}

} // namespace stagekit
