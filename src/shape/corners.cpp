/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Corners & Edges: cuts a source's picture to a shape (rounded, cut, notched
// or scooped corners, trimmed and feathered edges, an outline) without
// changing its size, so everything after it in the filter list, and every
// scene item showing it, carries on as before.

#include "corners.hpp"
#include "../common/timing.hpp"

#include "../common/shown-scale.hpp"
#include "../frame/auto-frame.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace stagekit {

namespace {

// Saved in settings: append only.
enum class Style { Round = 0, Cut = 1, Notch = 2, Scoop = 3 };

struct Settings {
	Style style = Style::Round;
	double size = 40.0;
	bool corner[4] = {true, true, true, true}; // top-left, top-right, bottom-right, bottom-left
	double inset = 0.0;
	double soft = 0.0;
	double border = 0.0;
	uint32_t border_color = 0xFFFFFFFF;
	double border_opacity = 1.0;
	bool screen_px = true;
};

// Where a filter comes in the order its source applies them, or -1.
int applied_index(obs_source_t *parent, obs_source_t *filter)
{
	struct Find {
		obs_source_t *wanted;
		int at, found;
	} find{filter, 0, -1};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->wanted)
				x->found = x->at;
			x->at++;
		},
		&find);
	return find.found;
}

// The first enabled filter of a kind on a source, in applied order. Not a new reference.
obs_source_t *first_filter(obs_source_t *parent, const char *id)
{
	struct Find {
		const char *id;
		obs_source_t *found;
	} find{id, nullptr};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			const char *fid = obs_source_get_unversioned_id(f);
			if (!x->found && fid && strcmp(fid, x->id) == 0 && obs_source_enabled(f))
				x->found = f;
		},
		&find);
	return find.found;
}

class Corners {
public:
	Corners(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		char *path = obs_module_file("effects/stagekit-corners.effect");
		obs_enter_graphics();
		char *errors = nullptr;
		effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
		if (!effect_)
			obs_log(LOG_ERROR, "Corners & Edges: failed to load effect: %s", errors ? errors : "not found");
		bfree(errors);
		if (effect_) {
			p_image_ = gs_effect_get_param_by_name(effect_, "image");
			p_size_ = gs_effect_get_param_by_name(effect_, "size");
			p_rect_ = gs_effect_get_param_by_name(effect_, "rect");
			p_radii_ = gs_effect_get_param_by_name(effect_, "radii");
			p_style_ = gs_effect_get_param_by_name(effect_, "style");
			p_soft_ = gs_effect_get_param_by_name(effect_, "soft");
			p_border_ = gs_effect_get_param_by_name(effect_, "border");
			p_border_color_ = gs_effect_get_param_by_name(effect_, "border_color");
		}
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		bfree(path);
		update(settings);
	}

	~Corners()
	{
		obs_enter_graphics();
		gs_effect_destroy(effect_);
		gs_texrender_destroy(tr_input_);
		gs_texrender_destroy(tr_out_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.style = static_cast<Style>(std::clamp((int)obs_data_get_int(settings, "style"), 0, 3));
		s.size = std::clamp(obs_data_get_double(settings, "size"), 0.0, 2000.0);
		s.corner[0] = obs_data_get_bool(settings, "top_left");
		s.corner[1] = obs_data_get_bool(settings, "top_right");
		s.corner[2] = obs_data_get_bool(settings, "bottom_right");
		s.corner[3] = obs_data_get_bool(settings, "bottom_left");
		s.inset = std::clamp(obs_data_get_double(settings, "inset"), 0.0, 2000.0);
		s.soft = std::clamp(obs_data_get_double(settings, "soft"), 0.0, 500.0);
		s.border = std::clamp(obs_data_get_double(settings, "border"), 0.0, 500.0);
		s.border_color = (uint32_t)obs_data_get_int(settings, "border_color");
		s.border_opacity = std::clamp(obs_data_get_double(settings, "border_opacity") / 100.0, 0.0, 1.0);
		s.screen_px = obs_data_get_bool(settings, "screen_px");
		std::lock_guard lock(mutex_);
		settings_ = s;
		placed_ = obs_data_get_bool(settings, "placed");
	}

	void tick()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return;
		bool placed;
		{
			std::lock_guard lock(mutex_);
			placed = placed_;
		}
		// Added after a Shadow & Backdrop, it would cut the shadow off. The
		// first time only, it moves itself above the shadow; after that the
		// order is the user's to choose.
		if (!placed) {
			obs_source_t *shadow = first_filter(parent, kShadowBackdropId);
			for (int i = 0; shadow && i < 32; i++) {
				const int mine = applied_index(parent, source_), theirs = applied_index(parent, shadow);
				if (mine < 0 || theirs < 0 || mine < theirs)
					break;
				obs_source_filter_set_order(parent, source_, OBS_ORDER_MOVE_UP);
				if (applied_index(parent, source_) == mine)
					break;
			}
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_bool(settings, "placed", true);
			obs_data_release(settings);
			std::lock_guard lock(mutex_);
			placed_ = true;
		}
		// Now and then, see how big the source is drawn where it is on screen.
		if (--scale_countdown_ <= 0) {
			scale_countdown_ = 20;
			double sx = 0.0, sy = 0.0;
			const bool found = shown_scale(parent, sx, sy);
			std::lock_guard lock(mutex_);
			shown_sx_ = found ? std::max(0.05, sx) : 1.0;
			shown_sy_ = found ? std::max(0.05, sy) : 1.0;
		}
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		obs_source_t *parent = obs_filter_get_parent(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (!effect_ || !parent || !w || !h) {
			obs_source_skip_video_filter(source_);
			return;
		}
		Settings s;
		double kx, ky;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
			kx = s.screen_px ? shown_sx_ : 1.0;
			ky = s.screen_px ? shown_sy_ : 1.0;
		}

		// The part to shape: the whole picture, or the game picture inside an
		// Auto Frame's frame when one comes before this filter.
		double rx = 0.0, ry = 0.0, rw = w, rh = h;
		if (obs_source_t *frame = first_filter(parent, kAutoFrameId)) {
			if (applied_index(parent, frame) < applied_index(parent, source_)) {
				calldata_t cd = {};
				if (proc_handler_call(obs_source_get_proc_handler(frame), "get_picture", &cd)) {
					const double px = calldata_float(&cd, "x"), py = calldata_float(&cd, "y");
					const double pw = calldata_float(&cd, "width"),
						     ph = calldata_float(&cd, "height");
					if (pw >= 1.0 && ph >= 1.0) {
						rx = std::clamp(px, 0.0, (double)w);
						ry = std::clamp(py, 0.0, (double)h);
						rw = std::clamp(pw, 0.0, w - rx);
						rh = std::clamp(ph, 0.0, h - ry);
					}
				}
				calldata_free(&cd);
			}
		}

		gs_texrender_reset(tr_input_);
		if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			return;
		if (!gs_texrender_begin(tr_input_, w, h))
			return;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		gs_blend_state_pop();
		gs_texrender_end(tr_input_);
		gs_texture_t *input = gs_texrender_get_texture(tr_input_);
		if (!input)
			return;

		// Sizes are in pixels as they look on screen, so the shape is worked
		// out in a space scaled like the item is drawn.
		vec2 size;
		vec2_set(&size, (float)(w * kx), (float)(h * ky));
		const double inset = std::min({s.inset, rw * kx / 2.0 - 0.5, rh * ky / 2.0 - 0.5});
		vec4 rect;
		vec4_set(&rect, (float)(rx * kx + inset), (float)(ry * ky + inset), (float)(rw * kx - 2.0 * inset),
			 (float)(rh * ky - 2.0 * inset));
		vec4 radii;
		vec4_set(&radii, s.corner[0] ? (float)s.size : 0.0f, s.corner[1] ? (float)s.size : 0.0f,
			 s.corner[2] ? (float)s.size : 0.0f, s.corner[3] ? (float)s.size : 0.0f);
		vec4 border_color;
		vec4_from_rgba(&border_color, s.border_color);
		border_color.w = (float)s.border_opacity;

		// The shaping pass works on raw colour values.
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_texrender_reset(tr_out_);
		bool drawn = false;
		if (gs_texrender_begin(tr_out_, w, h)) {
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
			gs_effect_set_texture(p_image_, input);
			gs_effect_set_vec2(p_size_, &size);
			gs_effect_set_vec4(p_rect_, &rect);
			gs_effect_set_vec4(p_radii_, &radii);
			gs_effect_set_float(p_style_, (float)(int)s.style);
			gs_effect_set_float(p_soft_, (float)s.soft);
			gs_effect_set_float(p_border_, (float)s.border);
			gs_effect_set_vec4(p_border_color_, &border_color);
			while (gs_effect_loop(effect_, "Shape"))
				gs_draw_sprite(input, 0, w, h);
			gs_blend_state_pop();
			gs_texrender_end(tr_out_);
			drawn = true;
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		if (gs_texture_t *out = drawn ? gs_texrender_get_texture(tr_out_) : nullptr) {
			gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
			const bool linear = gs_get_linear_srgb();
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(linear);
			if (linear)
				gs_effect_set_texture_srgb(image, out);
			else
				gs_effect_set_texture(image, out);
			while (gs_effect_loop(def, "Draw"))
				gs_draw_sprite(out, 0, w, h);
			gs_enable_framebuffer_srgb(previous);
		}
	}

private:
	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	bool placed_ = false;
	double shown_sx_ = 1.0, shown_sy_ = 1.0;
	int scale_countdown_ = 0;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_size_ = nullptr;
	gs_eparam_t *p_rect_ = nullptr;
	gs_eparam_t *p_radii_ = nullptr;
	gs_eparam_t *p_style_ = nullptr;
	gs_eparam_t *p_soft_ = nullptr;
	gs_eparam_t *p_border_ = nullptr;
	gs_eparam_t *p_border_color_ = nullptr;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_out_ = nullptr;
};

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *style = obs_properties_add_list(props, "style", obs_module_text("Corners.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Round"), (int)Style::Round);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Cut"), (int)Style::Cut);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Notch"), (int)Style::Notch);
	obs_property_list_add_int(style, obs_module_text("Corners.Style.Scoop"), (int)Style::Scoop);
	obs_property_t *size =
		obs_properties_add_float_slider(props, "size", obs_module_text("Corners.Size"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(size, " px");

	obs_properties_t *which = obs_properties_create();
	obs_properties_add_bool(which, "top_left", obs_module_text("Corners.TopLeft"));
	obs_properties_add_bool(which, "top_right", obs_module_text("Corners.TopRight"));
	obs_properties_add_bool(which, "bottom_left", obs_module_text("Corners.BottomLeft"));
	obs_properties_add_bool(which, "bottom_right", obs_module_text("Corners.BottomRight"));
	obs_properties_add_group(props, "which", obs_module_text("Corners.Which"), OBS_GROUP_NORMAL, which);

	obs_properties_t *edges = obs_properties_create();
	obs_property_t *inset =
		obs_properties_add_float_slider(edges, "inset", obs_module_text("Corners.Inset"), 0.0, 500.0, 1.0);
	obs_property_float_set_suffix(inset, " px");
	obs_property_set_long_description(inset, obs_module_text("Corners.Inset.Tip"));
	obs_property_t *soft =
		obs_properties_add_float_slider(edges, "soft", obs_module_text("Corners.Soft"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(soft, " px");
	obs_property_set_long_description(soft, obs_module_text("Corners.Soft.Tip"));
	obs_properties_add_group(props, "edges", obs_module_text("Corners.Edges"), OBS_GROUP_NORMAL, edges);

	obs_properties_t *line = obs_properties_create();
	obs_property_t *border =
		obs_properties_add_float_slider(line, "border", obs_module_text("Corners.Border"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(border, " px");
	obs_properties_add_color(line, "border_color", obs_module_text("Corners.BorderColor"));
	obs_property_t *opacity = obs_properties_add_float_slider(
		line, "border_opacity", obs_module_text("Corners.BorderOpacity"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(opacity, " %");
	obs_properties_add_group(props, "line", obs_module_text("Corners.Line"), OBS_GROUP_NORMAL, line);

	obs_property_t *screen = obs_properties_add_bool(props, "screen_px", obs_module_text("Corners.ScreenPx"));
	obs_property_set_long_description(screen, obs_module_text("Corners.ScreenPx.Tip"));
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "style", (int)Style::Round);
	obs_data_set_default_double(settings, "size", 40.0);
	obs_data_set_default_bool(settings, "top_left", true);
	obs_data_set_default_bool(settings, "top_right", true);
	obs_data_set_default_bool(settings, "bottom_left", true);
	obs_data_set_default_bool(settings, "bottom_right", true);
	obs_data_set_default_double(settings, "inset", 0.0);
	obs_data_set_default_double(settings, "soft", 0.0);
	obs_data_set_default_double(settings, "border", 0.0);
	obs_data_set_default_int(settings, "border_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "border_opacity", 100.0);
	obs_data_set_default_bool(settings, "screen_px", true);
	obs_data_set_default_bool(settings, "placed", false);
}

} // namespace

void register_corners()
{
	obs_source_info info = {};
	info.id = kCornersId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Corners.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Corners(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<Corners *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Corners *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float) {
		static_cast<Corners *>(data)->tick();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<Corners *>(data)->render();
	};
	register_timed(&info);
}

} // namespace stagekit
