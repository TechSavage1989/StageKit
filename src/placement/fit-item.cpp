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

#include "../common/scene-util.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

// Saved in settings: append only.
enum class BoxKind { WholeScene = 0, Custom = 1 };

struct Settings {
	std::string item;
	BoxKind box = BoxKind::WholeScene;
	double x = 0, y = 0, w = 1920, h = 1080;
	double margin = 80;
};

class FitItem {
public:
	FitItem(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }

	void update(obs_data_t *settings)
	{
		Settings s;
		s.item = obs_data_get_string(settings, "item");
		s.box = obs_data_get_int(settings, "box") == 1 ? BoxKind::Custom : BoxKind::WholeScene;
		s.x = obs_data_get_double(settings, "box_x");
		s.y = obs_data_get_double(settings, "box_y");
		s.w = std::max(2.0, obs_data_get_double(settings, "box_width"));
		s.h = std::max(2.0, obs_data_get_double(settings, "box_height"));
		s.margin = std::clamp(obs_data_get_double(settings, "margin"), 0.0, 2000.0);
		std::lock_guard lock(mutex_);
		settings_ = s;
	}

	void attach(obs_source_t *parent) { parent_ = parent; }
	void detach() { parent_ = nullptr; }
	obs_source_t *source() const { return source_; }
	obs_source_t *parent() const { return parent_; }

	void tick()
	{
		if (!parent_ || !obs_source_enabled(source_))
			return;
		Settings s;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
		}
		obs_sceneitem_t *item = find_item(parent_, s.item);
		obs_source_t *shown = item ? obs_sceneitem_get_source(item) : nullptr;
		const double sw = shown ? obs_source_get_width(shown) : 0.0,
			     sh = shown ? obs_source_get_height(shown) : 0.0;
		if (sw < 2.0 || sh < 2.0)
			return; // nothing to fit yet

		// What is fitted is the picture, without the room a shadow takes
		// around it and without what the item crops away.
		double room[4];
		shadow_room(shown, room);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		const double left = std::max(room[0], (double)crop.left), top = std::max(room[1], (double)crop.top);
		const double right = std::max(room[2], (double)crop.right),
			     bottom = std::max(room[3], (double)crop.bottom);
		const double pw = sw - left - right, ph = sh - top - bottom;
		if (pw < 2.0 || ph < 2.0)
			return;

		double bx = 0.0, by = 0.0, bw = obs_source_get_width(parent_), bh = obs_source_get_height(parent_);
		if (s.box == BoxKind::Custom) {
			bx = s.x;
			by = s.y;
			bw = s.w;
			bh = s.h;
		}
		if (bw < 2.0 || bh < 2.0)
			return;
		// The same shape as the box fills it; any other shape keeps clear of
		// its edges by the margin.
		const double picture = pw / ph, box = bw / bh;
		const bool fills = std::fabs(picture - box) / box < 0.01;
		const double margin = fills ? 0.0 : std::min({s.margin, bw / 2.0 - 1.0, bh / 2.0 - 1.0});
		const double aw = bw - 2.0 * margin, ah = bh - 2.0 * margin;
		const double k = std::min(aw / pw, ah / ph);
		// The item's corner, so that the picture sits centred. The item's own
		// crop is already taken off what it draws.
		const double x = bx + (bw - pw * k) / 2.0 - (left - crop.left) * k;
		const double y = by + (bh - ph * k) / 2.0 - (top - crop.top) * k;

		vec2 pos, scale;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_scale(item, &scale);
		const bool placed = obs_sceneitem_get_bounds_type(item) == OBS_BOUNDS_NONE &&
				    obs_sceneitem_get_alignment(item) == (OBS_ALIGN_LEFT | OBS_ALIGN_TOP) &&
				    std::fabs(pos.x - x) < 0.05 && std::fabs(pos.y - y) < 0.05 &&
				    std::fabs(scale.x - k) < 0.0002 && std::fabs(scale.y - k) < 0.0002 &&
				    obs_sceneitem_get_rot(item) == 0.0f;
		if (placed)
			return;
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_NONE);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_LEFT | OBS_ALIGN_TOP);
		obs_sceneitem_set_rot(item, 0.0f);
		vec2_set(&scale, (float)k, (float)k);
		obs_sceneitem_set_scale(item, &scale);
		vec2_set(&pos, (float)x, (float)y);
		obs_sceneitem_set_pos(item, &pos);
		obs_sceneitem_defer_update_end(item);
	}

private:
	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	Settings settings_;
};

bool box_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = obs_data_get_int(settings, "box") == 1;
	for (const char *name : {"box_x", "box_y", "box_width", "box_height"})
		obs_property_set_visible(obs_properties_get(props, name), custom);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<FitItem *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *item = obs_properties_add_list(props, "item", obs_module_text("Fit.Item"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(item, obs_module_text("Fit.Item.None"), "");
	add_scene_items_to_list(self ? self->parent() : nullptr, item);

	obs_property_t *box = obs_properties_add_list(props, "box", obs_module_text("Fit.Box"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Scene"), (int)BoxKind::WholeScene);
	obs_property_list_add_int(box, obs_module_text("Fit.Box.Custom"), (int)BoxKind::Custom);
	obs_property_set_modified_callback(box, box_modified);
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
}

} // namespace

void register_fit_item()
{
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
		static_cast<FitItem *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float) {
		static_cast<FitItem *>(data)->tick();
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<FitItem *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<FitItem *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<FitItem *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
