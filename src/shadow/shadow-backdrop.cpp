/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Shadow & Backdrop: a filter that gives any source a drop shadow, an
// optional panel behind it and rounded corners.
//
// A filter normally cannot draw outside its source, which is why shadows
// usually mean wrapping the source in an oversized scene. This filter grows
// the source by exactly the room the shadow and panel need, so no wrapper
// scene is required.

#include "shadow-backdrop.hpp"

#include "../common/kawase-blur.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>

namespace stagekit {

namespace {

struct Settings {
	bool shadow = true;
	uint32_t shadow_color = 0xFF000000;
	double shadow_opacity = 0.6;
	double offset_x = 12.0;
	double offset_y = 12.0;
	double softness = 20.0;
	double spread = 0.0;
	bool backdrop = false;
	uint32_t backdrop_color = 0xFF3A3A3A;
	double backdrop_opacity = 1.0;
	double backdrop_pad = 12.0;
	double radius = 0.0;
	bool hide_source = false;
};

struct Padding {
	uint32_t left = 0, top = 0, right = 0, bottom = 0;
};

// Room needed around the source for the panel and the shadow.
Padding padding_for(const Settings &s)
{
	const double panel = s.backdrop ? s.backdrop_pad : 0.0;
	const double soft = s.shadow && s.softness > 0.0 ? std::ceil(s.softness * 1.5) + 2.0 : 0.0;
	const double ox = s.shadow ? s.offset_x : 0.0;
	const double oy = s.shadow ? s.offset_y : 0.0;
	const double shadow = s.shadow ? 1.0 : 0.0;
	Padding p;
	p.left = (uint32_t)std::ceil(panel + shadow * (std::max(0.0, -ox) + soft));
	p.right = (uint32_t)std::ceil(panel + shadow * (std::max(0.0, ox) + soft));
	p.top = (uint32_t)std::ceil(panel + shadow * (std::max(0.0, -oy) + soft));
	p.bottom = (uint32_t)std::ceil(panel + shadow * (std::max(0.0, oy) + soft));
	return p;
}

class ShadowBackdrop {
public:
	ShadowBackdrop(obs_data_t *settings, obs_source_t *source);
	~ShadowBackdrop();

	void update(obs_data_t *settings);
	void render();
	uint32_t width();
	uint32_t height();

private:
	Settings settings();
	bool capture_input(uint32_t w, uint32_t h);

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_shadow_ = nullptr;
	gs_eparam_t *p_out_size_ = nullptr;
	gs_eparam_t *p_src_size_ = nullptr;
	gs_eparam_t *p_pad_ = nullptr;
	gs_eparam_t *p_radius_ = nullptr;
	gs_eparam_t *p_backdrop_on_ = nullptr;
	gs_eparam_t *p_backdrop_pad_ = nullptr;
	gs_eparam_t *p_backdrop_color_ = nullptr;
	gs_eparam_t *p_shadow_offset_ = nullptr;
	gs_eparam_t *p_shadow_color_ = nullptr;
	gs_eparam_t *p_shadow_gain_ = nullptr;
	gs_eparam_t *p_hide_source_ = nullptr;

	std::unique_ptr<KawaseBlur> blur_;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_card_ = nullptr;
	gs_texrender_t *tr_final_ = nullptr;
};

ShadowBackdrop::ShadowBackdrop(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	char *path = obs_module_file("effects/stagekit-shadow.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Shadow & Backdrop: failed to load effect: %s", errors ? errors : "not found");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_shadow_ = gs_effect_get_param_by_name(effect_, "shadow");
		p_out_size_ = gs_effect_get_param_by_name(effect_, "out_size");
		p_src_size_ = gs_effect_get_param_by_name(effect_, "src_size");
		p_pad_ = gs_effect_get_param_by_name(effect_, "pad");
		p_radius_ = gs_effect_get_param_by_name(effect_, "radius");
		p_backdrop_on_ = gs_effect_get_param_by_name(effect_, "backdrop_on");
		p_backdrop_pad_ = gs_effect_get_param_by_name(effect_, "backdrop_pad");
		p_backdrop_color_ = gs_effect_get_param_by_name(effect_, "backdrop_color");
		p_shadow_offset_ = gs_effect_get_param_by_name(effect_, "shadow_offset");
		p_shadow_color_ = gs_effect_get_param_by_name(effect_, "shadow_color");
		p_shadow_gain_ = gs_effect_get_param_by_name(effect_, "shadow_gain");
		p_hide_source_ = gs_effect_get_param_by_name(effect_, "hide_source");
	}
	blur_ = std::make_unique<KawaseBlur>();
	tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_card_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_final_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);

	update(settings);
}

ShadowBackdrop::~ShadowBackdrop()
{
	obs_enter_graphics();
	blur_.reset();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(tr_input_);
	gs_texrender_destroy(tr_card_);
	gs_texrender_destroy(tr_final_);
	obs_leave_graphics();
}

void ShadowBackdrop::update(obs_data_t *settings)
{
	Settings s;
	s.shadow = obs_data_get_bool(settings, "shadow");
	s.shadow_color = (uint32_t)obs_data_get_int(settings, "shadow_color");
	s.shadow_opacity = std::clamp(obs_data_get_double(settings, "shadow_opacity") / 100.0, 0.0, 1.0);
	s.offset_x = std::clamp(obs_data_get_double(settings, "offset_x"), -500.0, 500.0);
	s.offset_y = std::clamp(obs_data_get_double(settings, "offset_y"), -500.0, 500.0);
	s.softness = std::clamp(obs_data_get_double(settings, "softness"), 0.0, 200.0);
	s.spread = std::clamp(obs_data_get_double(settings, "spread") / 100.0, 0.0, 1.0);
	s.backdrop = obs_data_get_bool(settings, "backdrop");
	s.backdrop_color = (uint32_t)obs_data_get_int(settings, "backdrop_color");
	s.backdrop_opacity = std::clamp(obs_data_get_double(settings, "backdrop_opacity") / 100.0, 0.0, 1.0);
	s.backdrop_pad = std::clamp(obs_data_get_double(settings, "backdrop_pad"), 0.0, 500.0);
	s.radius = std::clamp(obs_data_get_double(settings, "radius"), 0.0, 2000.0);
	s.hide_source = obs_data_get_bool(settings, "hide_source");

	std::lock_guard lock(mutex_);
	settings_ = s;
}

Settings ShadowBackdrop::settings()
{
	std::lock_guard lock(mutex_);
	return settings_;
}

uint32_t ShadowBackdrop::width()
{
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	if (!w)
		return 0;
	const Padding p = padding_for(settings());
	return w + p.left + p.right;
}

uint32_t ShadowBackdrop::height()
{
	obs_source_t *target = obs_filter_get_target(source_);
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!h)
		return 0;
	const Padding p = padding_for(settings());
	return h + p.top + p.bottom;
}

bool ShadowBackdrop::capture_input(uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr_input_);
	if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return false;
	if (!gs_texrender_begin(tr_input_, w, h))
		return false;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr_input_);
	return true;
}

void ShadowBackdrop::render()
{
	obs_source_t *target = obs_filter_get_target(source_);
	obs_source_t *parent = obs_filter_get_parent(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!effect_ || !parent || !w || !h) {
		obs_source_skip_video_filter(source_);
		return;
	}

	const Settings s = settings();
	const Padding p = padding_for(s);
	const uint32_t ow = w + p.left + p.right;
	const uint32_t oh = h + p.top + p.bottom;

	if (!capture_input(w, h))
		return;
	gs_texture_t *input = gs_texrender_get_texture(tr_input_);
	if (!input)
		return;

	// The intermediate passes work on raw colour values.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	vec2 out_size, src_size, pad;
	vec2_set(&out_size, (float)ow, (float)oh);
	vec2_set(&src_size, (float)w, (float)h);
	vec2_set(&pad, (float)p.left, (float)p.top);

	auto draw = [&](gs_texrender_t *dst, gs_texture_t *image, const char *technique) {
		gs_texrender_reset(dst);
		if (!gs_texrender_begin(dst, ow, oh))
			return;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_ortho(0.0f, (float)ow, 0.0f, (float)oh, -100.0f, 100.0f);
		gs_effect_set_texture(p_image_, image);
		while (gs_effect_loop(effect_, technique))
			gs_draw_sprite(image, 0, ow, oh);
		gs_blend_state_pop();
		gs_texrender_end(dst);
	};

	// 1. The source on its panel, with rounded corners, inside the padding.
	vec4 backdrop_color;
	vec4_from_rgba(&backdrop_color, s.backdrop_color);
	backdrop_color.w = (float)s.backdrop_opacity;
	gs_effect_set_vec2(p_out_size_, &out_size);
	gs_effect_set_vec2(p_src_size_, &src_size);
	gs_effect_set_vec2(p_pad_, &pad);
	gs_effect_set_float(p_radius_, (float)s.radius);
	gs_effect_set_float(p_backdrop_on_, s.backdrop ? 1.0f : 0.0f);
	gs_effect_set_float(p_backdrop_pad_, (float)s.backdrop_pad);
	gs_effect_set_vec4(p_backdrop_color_, &backdrop_color);
	draw(tr_card_, input, "Card");
	gs_texture_t *card = gs_texrender_get_texture(tr_card_);

	// 2. Its silhouette, softened, becomes the shadow.
	gs_texture_t *silhouette = card;
	if (card && s.shadow && s.softness > 0.0)
		silhouette = blur_->blur(card, ow, oh, s.softness);

	// 3. Shadow underneath the card.
	if (card) {
		vec2 offset;
		vec2_set(&offset, (float)s.offset_x, (float)s.offset_y);
		vec4 shadow_color;
		vec4_from_rgba(&shadow_color, s.shadow_color);
		shadow_color.w = s.shadow ? (float)s.shadow_opacity : 0.0f;
		gs_effect_set_vec2(p_out_size_, &out_size);
		gs_effect_set_texture(p_shadow_, silhouette);
		gs_effect_set_vec2(p_shadow_offset_, &offset);
		gs_effect_set_vec4(p_shadow_color_, &shadow_color);
		gs_effect_set_float(p_shadow_gain_, (float)(1.0 + s.spread * 6.0));
		gs_effect_set_float(p_hide_source_, s.hide_source ? 1.0f : 0.0f);
		draw(tr_final_, card, "Compose");
	}

	gs_enable_framebuffer_srgb(previous_srgb);

	if (gs_texture_t *final_tex = card ? gs_texrender_get_texture(tr_final_) : nullptr) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, final_tex);
		else
			gs_effect_set_texture(image, final_tex);
		while (gs_effect_loop(def, "Draw"))
			gs_draw_sprite(final_tex, 0, ow, oh);
		gs_enable_framebuffer_srgb(previous);
	}
}

// --------------------------------------------------------------- properties

bool visibility_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool shadow = obs_data_get_bool(settings, "shadow");
	const bool backdrop = obs_data_get_bool(settings, "backdrop");
	for (const char *name :
	     {"shadow_color", "shadow_opacity", "offset_x", "offset_y", "softness", "spread", "hide_source"})
		obs_property_set_visible(obs_properties_get(props, name), shadow);
	for (const char *name : {"backdrop_color", "backdrop_opacity", "backdrop_pad"})
		obs_property_set_visible(obs_properties_get(props, name), backdrop);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *name, const char *text, double min, double max,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(text), min, max, 1.0);
	obs_property_float_set_suffix(p, suffix);
	return p;
}

obs_properties_t *get_properties(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *shadow = obs_properties_add_bool(props, "shadow", obs_module_text("Shadow.Shadow"));
	obs_property_set_modified_callback(shadow, visibility_modified);
	obs_properties_add_color(props, "shadow_color", obs_module_text("Shadow.Color"));
	slider(props, "shadow_opacity", "Shadow.Opacity", 0.0, 100.0, "%");
	slider(props, "offset_x", "Shadow.OffsetX", -200.0, 200.0, " px");
	slider(props, "offset_y", "Shadow.OffsetY", -200.0, 200.0, " px");
	slider(props, "softness", "Shadow.Softness", 0.0, 200.0, " px");
	slider(props, "spread", "Shadow.Spread", 0.0, 100.0, "%");
	obs_properties_add_bool(props, "hide_source", obs_module_text("Shadow.HideSource"));

	obs_property_t *backdrop = obs_properties_add_bool(props, "backdrop", obs_module_text("Shadow.Backdrop"));
	obs_property_set_modified_callback(backdrop, visibility_modified);
	obs_properties_add_color(props, "backdrop_color", obs_module_text("Shadow.BackdropColor"));
	slider(props, "backdrop_opacity", "Shadow.BackdropOpacity", 0.0, 100.0, "%");
	slider(props, "backdrop_pad", "Shadow.BackdropPad", 0.0, 200.0, " px");

	slider(props, "radius", "Shadow.Radius", 0.0, 500.0, " px");

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Shadow.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "shadow", true);
	obs_data_set_default_int(settings, "shadow_color", 0xFF000000);
	obs_data_set_default_double(settings, "shadow_opacity", 60.0);
	obs_data_set_default_double(settings, "offset_x", 12.0);
	obs_data_set_default_double(settings, "offset_y", 12.0);
	obs_data_set_default_double(settings, "softness", 20.0);
	obs_data_set_default_double(settings, "spread", 0.0);
	obs_data_set_default_bool(settings, "hide_source", false);
	obs_data_set_default_bool(settings, "backdrop", false);
	obs_data_set_default_int(settings, "backdrop_color", 0xFF3A3A3A);
	obs_data_set_default_double(settings, "backdrop_opacity", 100.0);
	obs_data_set_default_double(settings, "backdrop_pad", 12.0);
	obs_data_set_default_double(settings, "radius", 0.0);
}

} // namespace

void register_shadow_backdrop()
{
	obs_source_info info = {};
	info.id = kShadowBackdropId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Shadow.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ShadowBackdrop(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ShadowBackdrop *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ShadowBackdrop *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<ShadowBackdrop *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<ShadowBackdrop *>(data)->height();
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<ShadowBackdrop *>(data)->render();
	};
	obs_register_source(&info);
}

} // namespace stagekit
