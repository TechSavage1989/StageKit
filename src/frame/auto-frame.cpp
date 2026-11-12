/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Auto Frame: gives a capture a fixed-size frame and fits whatever the
// capture currently outputs inside it.
//
// A game or emulator capture changes size whenever the program changes
// resolution, which normally means one hand-built scene per picture shape
// plus automation to switch between them. With this filter the source's
// size never changes, so its place in the scene, its shadow and anything
// linked to it stay put, while the picture inside glides to its new fit.

#include "auto-frame.hpp"

#include "../common/easing.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace stagekit {

namespace {

enum class FitMode : int {
	Fit = 0,     // whole picture visible, bars where the shapes differ
	Fill = 1,    // frame covered, picture cropped where the shapes differ
	Whole = 2,   // largest whole-number scale that fits (crisp pixel art)
	Stretch = 3, // picture stretched to the frame
};

enum class Shape : int {
	Source = 0, // use the capture's own width : height
	R4x3 = 1,
	R16x9 = 2,
	R8x7 = 3,
	R3x2 = 4,
	R1x1 = 5,
	R5x4 = 6,
	R16x10 = 7,
	R10x9 = 8,
	Custom = 9,
};

struct Settings {
	uint32_t frame_w = 1920;
	uint32_t frame_h = 1080;
	FitMode fit = FitMode::Fit;
	Shape shape = Shape::Source;
	double custom_w = 4.0;
	double custom_h = 3.0;
	int align_x = 1; // 0 left, 1 centre, 2 right
	int align_y = 1; // 0 top, 1 centre, 2 bottom
	bool sharp = false;
	double glide_s = 0.35;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

struct Rect {
	double x = 0, y = 0, w = 0, h = 0;
};

double shape_ratio(const Settings &s, uint32_t w, uint32_t h)
{
	switch (s.shape) {
	case Shape::R4x3:
		return 4.0 / 3.0;
	case Shape::R16x9:
		return 16.0 / 9.0;
	case Shape::R8x7:
		return 8.0 / 7.0;
	case Shape::R3x2:
		return 3.0 / 2.0;
	case Shape::R1x1:
		return 1.0;
	case Shape::R5x4:
		return 5.0 / 4.0;
	case Shape::R16x10:
		return 16.0 / 10.0;
	case Shape::R10x9:
		return 10.0 / 9.0;
	case Shape::Custom:
		if (s.custom_w > 0.0 && s.custom_h > 0.0)
			return s.custom_w / s.custom_h;
		break;
	case Shape::Source:
		break;
	}
	return (double)w / (double)h;
}

// Where the picture goes inside the frame.
Rect target_rect(const Settings &s, uint32_t w, uint32_t h)
{
	const double fw = s.frame_w, fh = s.frame_h;
	const double ratio = shape_ratio(s, w, h);
	// The picture's size in display pixels at 1x: its height, and the
	// width its shape calls for.
	const double base_h = h;
	const double base_w = h * ratio;

	Rect r;
	switch (s.fit) {
	case FitMode::Stretch:
		r.w = fw;
		r.h = fh;
		break;
	case FitMode::Fill: {
		const double k = std::max(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Whole: {
		const double k = std::max(1.0, std::floor(std::min(fw / base_w, fh / base_h)));
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Fit:
	default: {
		const double k = std::min(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	}
	r.x = (fw - r.w) * (s.align_x * 0.5);
	r.y = (fh - r.h) * (s.align_y * 0.5);
	return r;
}

bool same(const Rect &a, const Rect &b)
{
	return std::fabs(a.x - b.x) < 0.01 && std::fabs(a.y - b.y) < 0.01 && std::fabs(a.w - b.w) < 0.01 &&
	       std::fabs(a.h - b.h) < 0.01;
}

class AutoFrame {
public:
	AutoFrame(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_frame_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_sampler_info point = {};
		point.filter = GS_FILTER_POINT;
		point.address_u = GS_ADDRESS_CLAMP;
		point.address_v = GS_ADDRESS_CLAMP;
		point.address_w = GS_ADDRESS_CLAMP;
		point.max_anisotropy = 1;
		point_sampler_ = gs_samplerstate_create(&point);
		obs_leave_graphics();
		update(settings);

		proc_handler_add(
			obs_source_get_proc_handler(source_),
			"void get_picture(out float x, out float y, out float width, out float height)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoFrame *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_float(cd, "x", self->current_.x);
				calldata_set_float(cd, "y", self->current_.y);
				calldata_set_float(cd, "width", self->current_.w);
				calldata_set_float(cd, "height", self->current_.h);
			},
			this);
	}

	~AutoFrame()
	{
		obs_enter_graphics();
		gs_texrender_destroy(tr_input_);
		gs_texrender_destroy(tr_frame_);
		gs_samplerstate_destroy(point_sampler_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.frame_w = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_width"), 2, 16384);
		s.frame_h = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_height"), 2, 16384);
		s.fit = static_cast<FitMode>(obs_data_get_int(settings, "fit"));
		s.shape = static_cast<Shape>(obs_data_get_int(settings, "shape"));
		s.custom_w = obs_data_get_double(settings, "custom_w");
		s.custom_h = obs_data_get_double(settings, "custom_h");
		s.align_x = (int)std::clamp<long long>(obs_data_get_int(settings, "align_x"), 0, 2);
		s.align_y = (int)std::clamp<long long>(obs_data_get_int(settings, "align_y"), 0, 2);
		s.sharp = obs_data_get_bool(settings, "sharp");
		s.glide_s = std::max<long long>(0, obs_data_get_int(settings, "glide_ms")) / 1000.0;
		s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		std::lock_guard lock(mutex_);
		settings_ = s;
	}

	void tick(float seconds)
	{
		std::lock_guard lock(mutex_);
		pending_time_ += seconds;
	}

	uint32_t width()
	{
		std::lock_guard lock(mutex_);
		return settings_.frame_w;
	}
	uint32_t height()
	{
		std::lock_guard lock(mutex_);
		return settings_.frame_h;
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (!w || !h || !obs_filter_get_parent(source_))
			return; // nothing to show yet: the frame stays empty

		Settings s;
		Rect rect;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
			const Rect goal = target_rect(s, w, h);
			if (!have_rect_) {
				// First picture: no glide, just be in the right place.
				current_ = from_ = goal_ = goal;
				have_rect_ = true;
				elapsed_ = duration_ = 0.0;
			} else if (!same(goal, goal_)) {
				from_ = current_;
				goal_ = goal;
				elapsed_ = 0.0;
				duration_ = s.glide_s;
			}
			if (elapsed_ < duration_) {
				elapsed_ = std::min(duration_, elapsed_ + pending_time_);
				const double t = ease(s.curve, s.mode, elapsed_ / duration_);
				current_.x = from_.x + (goal_.x - from_.x) * t;
				current_.y = from_.y + (goal_.y - from_.y) * t;
				current_.w = from_.w + (goal_.w - from_.w) * t;
				current_.h = from_.h + (goal_.h - from_.h) * t;
			} else {
				current_ = goal_;
			}
			pending_time_ = 0.0;
			rect = current_;
		}

		// 1. The capture as it is.
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

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

		// 2. Placed inside the frame. Drawing into a frame-sized target also
		// crops anything a "fill" fit pushes past the edges.
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_texrender_reset(tr_frame_);
		if (gs_texrender_begin(tr_frame_, s.frame_w, s.frame_h)) {
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)s.frame_w, 0.0f, (float)s.frame_h, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(image, input);
			if (s.sharp)
				gs_effect_set_next_sampler(image, point_sampler_);
			gs_matrix_push();
			gs_matrix_translate3f((float)rect.x, (float)rect.y, 0.0f);
			gs_matrix_scale3f((float)(rect.w / w), (float)(rect.h / h), 1.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(input, 0, w, h);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(tr_frame_);
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		// 3. The finished frame.
		if (gs_texture_t *frame = gs_texrender_get_texture(tr_frame_)) {
			const bool linear = gs_get_linear_srgb();
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(linear);
			if (linear)
				gs_effect_set_texture_srgb(image, frame);
			else
				gs_effect_set_texture(image, frame);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(frame, 0, s.frame_w, s.frame_h);
			gs_enable_framebuffer_srgb(previous);
		}
	}

private:
	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	bool have_rect_ = false;
	Rect current_, from_, goal_;
	double elapsed_ = 0.0, duration_ = 0.0, pending_time_ = 0.0;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_frame_ = nullptr;
	gs_samplerstate_t *point_sampler_ = nullptr;
};

bool shape_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = static_cast<Shape>(obs_data_get_int(settings, "shape")) == Shape::Custom;
	obs_property_set_visible(obs_properties_get(props, "custom_w"), custom);
	obs_property_set_visible(obs_properties_get(props, "custom_h"), custom);
	return true;
}

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "frame_width", obs_module_text("Frame.Width"), 2, 16384, 1);
	obs_properties_add_int(props, "frame_height", obs_module_text("Frame.Height"), 2, 16384, 1);

	obs_property_t *fit = obs_properties_add_list(props, "fit", obs_module_text("Frame.Fit"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fit"), (int)FitMode::Fit);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Whole"), (int)FitMode::Whole);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fill"), (int)FitMode::Fill);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Stretch"), (int)FitMode::Stretch);

	obs_property_t *shape = obs_properties_add_list(props, "shape", obs_module_text("Frame.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Source"), (int)Shape::Source);
	struct {
		Shape value;
		const char *text;
	} shapes[] = {{Shape::R4x3, "4:3"},   {Shape::R16x9, "16:9"},   {Shape::R8x7, "8:7"}, {Shape::R3x2, "3:2"},
		      {Shape::R10x9, "10:9"}, {Shape::R16x10, "16:10"}, {Shape::R5x4, "5:4"}, {Shape::R1x1, "1:1"}};
	for (const auto &entry : shapes)
		obs_property_list_add_int(shape, entry.text, (int)entry.value);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Custom"), (int)Shape::Custom);
	obs_property_set_modified_callback(shape, shape_modified);
	obs_properties_add_float(props, "custom_w", obs_module_text("Frame.CustomW"), 0.01, 1000.0, 0.01);
	obs_properties_add_float(props, "custom_h", obs_module_text("Frame.CustomH"), 0.01, 1000.0, 0.01);

	obs_property_t *ax = obs_properties_add_list(props, "align_x", obs_module_text("Frame.AlignX"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ax, obs_module_text("Frame.Left"), 0);
	obs_property_list_add_int(ax, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ax, obs_module_text("Frame.Right"), 2);
	obs_property_t *ay = obs_properties_add_list(props, "align_y", obs_module_text("Frame.AlignY"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ay, obs_module_text("Frame.Top"), 0);
	obs_property_list_add_int(ay, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ay, obs_module_text("Frame.Bottom"), 2);

	obs_properties_add_bool(props, "sharp", obs_module_text("Frame.Sharp"));
	obs_property_t *glide =
		obs_properties_add_int_slider(props, "glide_ms", obs_module_text("Frame.Glide"), 0, 5000, 50);
	obs_property_int_set_suffix(glide, " ms");
	add_easing_properties(props, "");

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Frame.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "frame_width", have ? ovi.base_width : 1920);
	obs_data_set_default_int(settings, "frame_height", have ? ovi.base_height : 1080);
	obs_data_set_default_int(settings, "fit", (int)FitMode::Fit);
	obs_data_set_default_int(settings, "shape", (int)Shape::Source);
	obs_data_set_default_double(settings, "custom_w", 4.0);
	obs_data_set_default_double(settings, "custom_h", 3.0);
	obs_data_set_default_int(settings, "align_x", 1);
	obs_data_set_default_int(settings, "align_y", 1);
	obs_data_set_default_bool(settings, "sharp", false);
	obs_data_set_default_int(settings, "glide_ms", 350);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

} // namespace

void register_auto_frame()
{
	obs_source_info info = {};
	info.id = kAutoFrameId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Frame.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AutoFrame(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AutoFrame *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AutoFrame *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<AutoFrame *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<AutoFrame *>(data)->height();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AutoFrame *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AutoFrame *>(data)->render();
	};
	obs_register_source(&info);
}

} // namespace stagekit
