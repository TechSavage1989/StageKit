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

bool box_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const long long kind = obs_data_get_int(settings, "box");
	for (const char *name : {"box_x", "box_y", "box_width", "box_height"})
		obs_property_set_visible(obs_properties_get(props, name), kind == 1);
	for (const char *name :
	     {"skip|alist", "glide_ms", "ease_curve", "ease_mode", "visible_pixels", "tolerance", "skip_fill_on"})
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, kind == 2);
	const bool rule = kind == 2 && obs_data_get_bool(settings, "skip_fill_on");
	for (const char *name : {"skip_use_pct", "skip_use_px"})
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, rule);
	if (obs_property_t *p = obs_properties_get(props, "skip_fill"))
		obs_property_set_visible(p, rule && obs_data_get_bool(settings, "skip_use_pct"));
	for (const char *name : {"skip_px_top", "skip_px_bottom", "skip_px_left", "skip_px_right"})
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, rule && obs_data_get_bool(settings, "skip_use_px"));
	obs_property_set_description(obs_properties_get(props, "margin"),
				     obs_module_text(kind == 2 ? "Fit.Gap" : "Fit.Margin"));
	return true;
}

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

	obs_property_t *box = obs_properties_add_list(props, "box", obs_module_text("Fit.Box"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Scene"), (int)BoxKind::WholeScene);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Custom"), (int)BoxKind::Custom);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Free"), (int)BoxKind::FreeSpace);
	obs_property_set_long_description(box, obs_module_text("Fit.Box.Tip"));
	auto number = [&](const char *name, const char *text) {
		obs_property_t *p =
			obs_properties_add_float(props, name, obs_module_text(text), -16384.0, 16384.0, 1.0);
		obs_property_float_set_suffix(p, " px");
	};
	number("box_x", "Fit.BoxX");
	number("box_y", "Fit.BoxY");
	number("box_width", "Fit.BoxWidth");
	number("box_height", "Fit.BoxHeight");

	obs_property_t *margin =
		obs_properties_add_float_slider(props, "margin", obs_module_text("Fit.Margin"), 0.0, 400.0, 1.0);
	obs_property_float_set_suffix(margin, " px");
	obs_property_set_long_description(margin, obs_module_text("Fit.Margin.Tip"));
	obs_property_t *resize = obs_properties_add_bool(props, "resize", obs_module_text("Fit.Resize"));
	obs_property_set_long_description(resize, obs_module_text("Fit.Resize.Tip"));
	obs_property_t *toward = obs_properties_add_list(props, "toward", obs_module_text("Fit.Toward"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	// Saved by number: new ones go at the end.
	int toward_value = 0;
	for (const char *name :
	     {"Fit.Toward.Middle", "Fit.Toward.Top", "Fit.Toward.Bottom", "Fit.Toward.Left", "Fit.Toward.Right",
	      "Fit.Toward.TopLeft", "Fit.Toward.TopRight", "Fit.Toward.BottomLeft", "Fit.Toward.BottomRight"})
		obs_property_list_add_int(toward, obs_module_text(name), toward_value++);
	obs_property_set_long_description(toward, obs_module_text("Fit.Toward.Tip"));
	obs_property_t *tolerance =
		obs_properties_add_float_slider(props, "tolerance", obs_module_text("Fit.Tolerance"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(tolerance, " px");
	obs_property_set_long_description(tolerance, obs_module_text("Fit.Tolerance.Tip"));
	obs_property_t *visible_pixels =
		obs_properties_add_bool(props, "visible_pixels", obs_module_text("Measure.Visible"));
	obs_property_set_long_description(visible_pixels, obs_module_text("Fit.Visible.Tip"));
	obs_property_t *no_shadows = obs_properties_add_bool(props, "no_shadows", obs_module_text("Measure.NoShadows"));
	obs_property_set_long_description(no_shadows, obs_module_text("Fit.NoShadows.Tip"));
	obs_property_t *glide = obs_properties_add_int(props, "glide_ms", obs_module_text("Fit.Glide"), 0, 10000, 50);
	obs_property_int_set_suffix(glide, " ms");
	obs_property_set_long_description(glide, obs_module_text("Fit.Glide.Tip"));
	add_easing_properties(props, "");

	obs_source_t *parent = self ? self->parent() : nullptr;
	PickList skip;
	skip.setting = "skip";
	skip.title = "Fit.Skip";
	skip.add = "List.AddItem";
	skip.tip = "Fit.Skip.Tip";
	skip.choices = [parent](std::vector<std::string> &names) {
		scene_item_names(parent, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, skip);
	obs_property_t *skip_fill_on =
		obs_properties_add_bool(props, "skip_fill_on", obs_module_text("Fit.SkipFillOn"));
	obs_property_set_long_description(skip_fill_on, obs_module_text("Fit.SkipFillOn.Tip"));
	obs_property_set_modified_callback(skip_fill_on, box_modified);
	obs_property_t *use_pct = obs_properties_add_bool(props, "skip_use_pct", obs_module_text("Fit.SkipUsePct"));
	obs_property_set_long_description(use_pct, obs_module_text("Fit.SkipUsePct.Tip"));
	obs_property_set_modified_callback(use_pct, box_modified);
	obs_property_t *skip_fill =
		obs_properties_add_float_slider(props, "skip_fill", obs_module_text("Fit.SkipFill"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(skip_fill, "%");
	obs_property_set_long_description(skip_fill, obs_module_text("Fit.SkipFill.Tip"));
	obs_property_t *use_px = obs_properties_add_bool(props, "skip_use_px", obs_module_text("Fit.SkipUsePx"));
	obs_property_set_long_description(use_px, obs_module_text("Fit.SkipUsePx.Tip"));
	obs_property_set_modified_callback(use_px, box_modified);
	static const char *const kEdges[][2] = {{"skip_px_top", "Fit.SkipPxTop"},
						{"skip_px_bottom", "Fit.SkipPxBottom"},
						{"skip_px_left", "Fit.SkipPxLeft"},
						{"skip_px_right", "Fit.SkipPxRight"}};
	for (const auto &edge : kEdges) {
		obs_property_t *px =
			obs_properties_add_float_slider(props, edge[0], obs_module_text(edge[1]), 0.0, 2000.0, 1.0);
		obs_property_float_set_suffix(px, " px");
		obs_property_set_long_description(px, obs_module_text("Fit.SkipPx.Tip"));
	}
	PickList share;
	share.setting = "share_scenes";
	share.title = "Fit.Share";
	share.add = "List.AddScene";
	share.tip = "Fit.Share.Tip";
	share.choices = [parent](std::vector<std::string> &names) {
		struct Other {
			std::vector<std::string> *names;
			obs_source_t *self;
		} other{&names, parent};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				auto *o = static_cast<Other *>(param);
				const char *name = obs_source_get_name(scene);
				if (name && *name && !obs_source_is_group(scene) && scene != o->self)
					o->names->push_back(name);
				return true;
			},
			&other);
	};
	pick_list_add(props, self ? self->source() : nullptr, share);
	// Last, so that everything it shows and hides is there.
	obs_property_set_modified_callback(box, box_modified);
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
		for (const char *key : {"item", "keep_item", "orig_item"}) {
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
		obs_source_skip_video_filter(static_cast<FitItem *>(data)->source());
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
