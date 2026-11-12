/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Ramp Zoom: a filter that smoothly zooms into part of a source. Switching
// the filter on zooms in; switching it off zooms back out.

#include "ramp-zoom.hpp"

#include "../common/switch-ramp.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

struct Settings {
	double zoom = 2.0;
	double center_x = 0.5;
	double center_y = 0.5;
};

class RampZoom {
public:
	RampZoom(obs_data_t *settings, obs_source_t *source) : source_(source), ramp_(source)
	{
		obs_enter_graphics();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		obs_leave_graphics();
		update(settings);
	}

	~RampZoom()
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		obs_enter_graphics();
		gs_texrender_destroy(tr_input_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.zoom = std::clamp(obs_data_get_double(settings, "zoom") / 100.0, 1.0, 20.0);
		s.center_x = std::clamp(obs_data_get_double(settings, "center_x") / 100.0, 0.0, 1.0);
		s.center_y = std::clamp(obs_data_get_double(settings, "center_y") / 100.0, 0.0, 1.0);
		ramp_.configure(obs_data_get_int(settings, "ramp_in_ms") / 1000.0,
				obs_data_get_int(settings, "ramp_out_ms") / 1000.0,
				static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve")),
				static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode")));
		std::lock_guard lock(mutex_);
		settings_ = s;
	}

	void tick(float seconds) { ramp_.tick(seconds); }

	void attach(obs_source_t *parent)
	{
		ramp_.attached();
		const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		std::string text = obs_module_text("Hotkey.ZoomToggle");
		const size_t at = text.find("%1");
		if (at != std::string::npos)
			text.replace(at, 2, name);
		hotkey_ = obs_hotkey_register_source(
			parent, ("stagekit.zoom.toggle." + name).c_str(), text.c_str(),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				auto *self = static_cast<RampZoom *>(data);
				if (pressed)
					self->ramp_.set_active(!self->ramp_.active());
			},
			this);
	}

	void detach()
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		hotkey_ = OBS_INVALID_HOTKEY_ID;
	}

	void render()
	{
		Settings s;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
		}
		const double level = ramp_.level();
		// Multiplicative zoom feels even: each moment magnifies by the same ratio.
		const double zoom = std::max(1.0, std::pow(s.zoom, level));

		obs_source_t *target = obs_filter_get_target(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (zoom <= 1.0005 || !w || !h || !obs_filter_get_parent(source_)) {
			obs_source_skip_video_filter(source_);
			return;
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

		// The visible region, kept inside the picture.
		const double rw = w / zoom, rh = h / zoom;
		const double rx = std::clamp(s.center_x * w - rw / 2.0, 0.0, w - rw);
		const double ry = std::clamp(s.center_y * h - rh / 2.0, 0.0, h - rh);

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, input);
		else
			gs_effect_set_texture(image, input);
		gs_matrix_push();
		gs_matrix_scale3f((float)zoom, (float)zoom, 1.0f);
		gs_matrix_translate3f((float)-rx, (float)-ry, 0.0f);
		// Whole pixels outward, so the edges are never short by a fraction.
		const uint32_t sx = (uint32_t)std::floor(rx), sy = (uint32_t)std::floor(ry);
		const uint32_t sw = std::min(w - sx, (uint32_t)std::ceil(rx + rw) - sx);
		const uint32_t sh = std::min(h - sy, (uint32_t)std::ceil(ry + rh) - sy);
		gs_matrix_translate3f((float)sx, (float)sy, 0.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite_subregion(input, 0, sx, sy, sw, sh);
		gs_matrix_pop();
		gs_enable_framebuffer_srgb(previous);
	}

	SwitchRamp &ramp() { return ramp_; }

private:
	obs_source_t *source_;
	SwitchRamp ramp_;
	std::mutex mutex_;
	Settings settings_;
	gs_texrender_t *tr_input_ = nullptr;
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *zoom =
		obs_properties_add_float_slider(props, "zoom", obs_module_text("Zoom.Amount"), 100.0, 800.0, 5.0);
	obs_property_float_set_suffix(zoom, "%");
	obs_property_t *cx =
		obs_properties_add_float_slider(props, "center_x", obs_module_text("Zoom.CenterX"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cx, "%");
	obs_property_t *cy =
		obs_properties_add_float_slider(props, "center_y", obs_module_text("Zoom.CenterY"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cy, "%");
	obs_property_t *in =
		obs_properties_add_int_slider(props, "ramp_in_ms", obs_module_text("Zoom.RampIn"), 0, 5000, 50);
	obs_property_int_set_suffix(in, " ms");
	obs_property_t *out =
		obs_properties_add_int_slider(props, "ramp_out_ms", obs_module_text("Zoom.RampOut"), 0, 5000, 50);
	obs_property_int_set_suffix(out, " ms");
	add_easing_properties(props, "");
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Zoom.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "zoom", 200.0);
	obs_data_set_default_double(settings, "center_x", 50.0);
	obs_data_set_default_double(settings, "center_y", 50.0);
	obs_data_set_default_int(settings, "ramp_in_ms", 500);
	obs_data_set_default_int(settings, "ramp_out_ms", 500);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

void register_ramp_zoom()
{
	obs_source_info info = {};
	info.id = kRampZoomId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("Zoom.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new RampZoom(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<RampZoom *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<RampZoom *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<RampZoom *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<RampZoom *>(data)->render();
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<RampZoom *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<RampZoom *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
