/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Fit Item: one capture, any shape, placed by the scene. A widescreen game
// fills the box; an emulator's squarer picture floats in the middle of it
// with a margin, with nothing to switch by hand between the two.

#include "fit-item.hpp"
#include "fit-engine.hpp"
#include "scene-fit.hpp"
#include "../common/filter-input.hpp"
#include "../common/timing.hpp"

#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../dock/arrange-tools.hpp"
#include "../dock/arrange.hpp"
#include "../shadow/shadow-backdrop.hpp"
#include "../capture/app-capture.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

using namespace fit;

namespace {

// A Fit to Space: one engine, and its own look at what is in the way.
struct FitItem {
	FitItem(obs_data_t *settings, obs_source_t *source) : engine(settings, source) {}
	FitEngine engine;
	FitScan scan;
	obs_source_t *parent() const { return engine.parent(); }
	obs_source_t *source() const { return engine.source(); }
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<FitItem *>(data);
	obs_properties_t *props = obs_properties_create();
	// On a group it has nothing to go by (a group is only as large as what
	// is in it) and does nothing; said here, where it will be looked for.
	if (self && self->parent() && obs_source_is_group(self->parent())) {
		obs_property_t *wrong =
			obs_properties_add_text(props, "on_group", obs_module_text("Fit.OnGroup"), OBS_TEXT_INFO);
		obs_property_text_set_info_type(wrong, OBS_TEXT_INFO_WARNING);
		obs_property_text_set_info_word_wrap(wrong, true);
	}
	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Fit.Item"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(item, obs_module_text("Fit.Item.None"), "");
	add_scene_items_to_list(self ? self->parent() : nullptr, item);
	fit_add_controls(props, "", "", self ? self->source() : nullptr, self ? self->parent() : nullptr, false);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "box", (int)BoxKind::WholeScene);
	obs_data_set_default_double(settings, "box_x", 0.0);
	obs_data_set_default_double(settings, "box_y", 0.0);
	obs_data_set_default_double(settings, "box_width", have ? ovi.base_width : 1920);
	obs_data_set_default_double(settings, "box_height", have ? ovi.base_height : 1080);
	obs_data_set_default_double(settings, "margin", 80.0);
	obs_data_set_default_double(settings, "tolerance", 8.0);
	obs_data_set_default_bool(settings, "skip_fill_on", false);
	obs_data_set_default_double(settings, "skip_fill", 95.0);
	obs_data_set_default_bool(settings, "skip_use_pct", true);
	obs_data_set_default_bool(settings, "skip_use_px", false);
	for (const char *name : {"skip_px_top", "skip_px_bottom", "skip_px_left", "skip_px_right"})
		obs_data_set_default_double(settings, name, 50.0);
	obs_data_set_default_bool(settings, "resize", true);
	obs_data_set_default_int(settings, "toward", 0);
	// Sharing, not by list order, old and new alike (see the handoff).
	obs_data_set_default_int(settings, "space_priority", (int)SpacePriority::Shares);
	obs_data_set_default_bool(settings, "list_priority", false);
	obs_data_set_default_bool(settings, "visible_pixels", true);
	obs_data_set_default_bool(settings, "no_shadows", true);
	obs_data_set_default_int(settings, "glide_ms", 300);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

void fit_item_placed(obs_source_t *scene, const char *item_name)
{
	if (!item_name)
		return;
	engines_placed(scene, item_name);
}

void fit_item_shared(obs_source_t *scene, const char *item_name, const double *home)
{
	if (!item_name || !scene)
		return;
	engines_shared(scene, item_name, home);
}

namespace {
// A source is renamed: the filters that name it go on naming it. Before, a
// filter kept the old name, found no such item and did nothing from then on;
// the owner's camera, renamed, was left unfitted in two scenes of three.
void on_source_rename(void *, calldata_t *cd)
{
	const char *was = calldata_string(cd, "prev_name");
	const char *now = calldata_string(cd, "new_name");
	if (!was || !now || !*was || !*now)
		return;
	std::vector<obs_source_t *> filters = engine_owners();
	for (obs_source_t *filter : filters) {
		obs_data_t *settings = obs_source_get_settings(filter);
		bool changed = false;
		for (const char *key : {"item", "keep_item", "orig_item", "home_item"}) {
			if (strcmp(obs_data_get_string(settings, key), was) == 0) {
				obs_data_set_string(settings, key, now);
				changed = true;
			}
		}
		if (changed)
			obs_source_update(filter, settings);
		obs_data_release(settings);
		obs_source_release(filter);
	}
}
} // namespace

void fit_item_stop()
{
	scene_fit_stop();
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
}

void register_fit_item()
{
	// For the tests: how many full ticks a fit filter's engines have had
	// (off screen most are skipped), and how many glides they set off.
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_fit_counts(in string filter, in string item, out int ticks, out int glides)",
		[](void *, calldata_t *cd) {
			obs_source_t *owner = nullptr;
			const char *name = calldata_string(cd, "filter");
			// A filter is not found by name: it is looked for on every scene.
			struct Find {
				const char *name;
				obs_source_t *found;
			} find{name, nullptr};
			obs_enum_scenes(
				[](void *param, obs_source_t *scene) {
					auto *f = static_cast<Find *>(param);
					if (!f->found)
						f->found = obs_source_get_filter_by_name(scene, f->name);
					return !f->found;
				},
				&find);
			owner = find.found;
			uint64_t ticks = 0, glides = 0;
			if (owner)
				engine_counts(owner, calldata_string(cd, "item"), ticks, glides);
			obs_source_release(owner);
			calldata_set_int(cd, "ticks", (long long)ticks);
			calldata_set_int(cd, "glides", (long long)glides);
		},
		nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", on_source_rename, nullptr);
	obs_source_info info = {};
	info.id = kFitItemId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Fit.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new FitItem(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<FitItem *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<FitItem *>(data)->engine.update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		auto *self = static_cast<FitItem *>(data);
		self->engine.tick(seconds, self->scan);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		skip_filter_input(static_cast<FitItem *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<FitItem *>(data)->engine.attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<FitItem *>(data)->engine.detach();
	};
	register_timed(&info);
}

} // namespace stagekit
