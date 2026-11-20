/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Fade and Zoom, a transition.
//
// The owner, after Morph's "Fade and zoom" for items that are in one scene
// only: "also add fade and zoom as a transition". OBS hands a transition its
// two sides as finished pictures; each is drawn at a strength and a size
// that go with the time. An item shown or hidden has one side only: it
// fades as it grows or shrinks. With two (a change of scene) the old scene
// stays underneath and the new one fades in over it as it grows.

#include "fade-zoom.hpp"
#include "morph-paint.hpp"
#include "../common/easing.hpp"
#include "../common/timing.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <mutex>

namespace stagekit {

namespace {

class FadeZoom : public MorphPaint {
public:
	FadeZoom(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }
	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		curve_ = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		mode_ = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		small_ = std::clamp(obs_data_get_double(settings, "zoom"), 10.0, 100.0) / 100.0;
	}
	void render()
	{
		// Which sides have a source. OBS hands the callback a see-through
		// placeholder for a side without one, never nothing, so an item
		// being hidden looked like a change of scene into nothing: it stayed
		// whole and went at once at the end.
		obs_source_t *a = obs_transition_get_source(source_, OBS_TRANSITION_SOURCE_A);
		obs_source_t *b = obs_transition_get_source(source_, OBS_TRANSITION_SOURCE_B);
		has_a_ = a != nullptr;
		has_b_ = b != nullptr;
		obs_source_release(a);
		obs_source_release(b);
		obs_transition_video_render(source_, draw_cb);
	}
	obs_source_t *source() const { return source_; }

private:
	static void draw_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
	{
		auto *self = static_cast<FadeZoom *>(data);
		self->draw(self->has_a_ ? a : nullptr, self->has_b_ ? b : nullptr, t, cx, cy);
	}
	void draw(gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy);

	obs_source_t *source_;
	std::mutex mutex_;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	double small_ = 0.8;                 // the size of a picture when it is all but gone
	bool has_a_ = false, has_b_ = false; // drawing thread only
};

void FadeZoom::draw(gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	if (!effect_ || (!a && !b) || !cx || !cy)
		return;
	EaseCurve curve;
	EaseMode mode;
	double small;
	{
		std::lock_guard lock(mutex_);
		curve = curve_;
		mode = mode_;
		small = small_;
	}
	// How much of the new side there is. (A curve that overshoots would
	// fade past all or nothing: kept between.)
	const float in = (float)std::clamp(ease(curve, mode, std::clamp((double)t, 0.0, 1.0)), 0.0, 1.0);
	const float out = 1.0f - in;
	auto size_at = [small](float there) {
		return small + (1.0 - small) * (double)there;
	};
	if (!a || !b) {
		// One item coming or going.
		const float there = a ? out : in;
		draw_faded(a ? a : b, cx, cy, there, size_at(there));
		return;
	}
	// Two, a change of scene: the old one stays as it is underneath and the
	// new one fades in over it as it grows, so the picture is whole all the
	// way. (Both shrunk and faded, the screen's edges would be bare half-way.)
	draw_faded(a, cx, cy, 1.0f, 1.0);
	draw_faded(b, cx, cy, in, size_at(in));
}

float mix_out(void *, float t)
{
	return 1.0f - t;
}

float mix_in(void *, float t)
{
	return t;
}

} // namespace

void register_fade_zoom()
{
	obs_source_info info = {};
	info.id = kFadeZoomId;
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.get_name = [](void *) {
		return obs_module_text("FadeZoom.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new FadeZoom(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<FadeZoom *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<FadeZoom *>(data)->update(settings);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<FadeZoom *>(data)->render();
	};
	info.audio_render = [](void *data, uint64_t *ts_out, obs_source_audio_mix *audio, uint32_t mixers,
			       size_t channels, size_t sample_rate) {
		return obs_transition_audio_render(static_cast<FadeZoom *>(data)->source(), ts_out, audio, mixers,
						   channels, sample_rate, mix_out, mix_in);
	};
	info.get_properties = [](void *) {
		obs_properties_t *props = obs_properties_create();
		obs_property_t *zoom = obs_properties_add_float_slider(props, "zoom", obs_module_text("FadeZoom.Zoom"),
								       10.0, 100.0, 1.0);
		obs_property_float_set_suffix(zoom, "%");
		set_tip(zoom, obs_module_text("FadeZoom.Zoom.Tip"));
		add_easing_properties(props, "");
		return props;
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_double(settings, "zoom", 80.0);
		obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
		obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	};
	register_timed(&info);
}

} // namespace stagekit
