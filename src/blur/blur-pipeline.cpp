/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "blur-pipeline.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>

namespace stagekit {

namespace {

// Up to here the Gaussian and the box blur are drawn tap by tap. A filter set
// stronger than this is drawn the Dual Kawase way for its whole ramp: the taps
// would stand so far apart that they show as stripes, and Kawase at the same
// number looks about as strong.
constexpr double kTapLimit = 250.0;

bool type_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	blur_look_visibility(props, settings);
	return true;
}

} // namespace

void blur_look_visibility(obs_properties_t *props, obs_data_t *settings)
{
	const auto type = static_cast<BlurType>(obs_data_get_int(settings, "blur_type"));
	const bool mosaic = type == BlurType::Pixelate || type == BlurType::Hexagon;
	obs_property_set_visible(obs_properties_get(props, "angle"), type == BlurType::Motion);
	obs_property_set_visible(obs_properties_get(props, "center_x"), type == BlurType::Zoom);
	obs_property_set_visible(obs_properties_get(props, "center_y"), type == BlurType::Zoom);
	for (const char *name :
	     {"pixel_shape", "pixel_smooth", "pixel_glass", "pixel_glass_style", "origin_x", "origin_y", "rotation"})
		obs_property_set_visible(obs_properties_get(props, name), mosaic);
	// (Scene Blur: only where each source is blurred by itself.)
	const bool scene_wide = obs_properties_get(props, "blur_area") && obs_data_get_int(settings, "blur_area") == 0;
	obs_property_set_visible(obs_properties_get(props, "pixel_edge"), mosaic && !scene_wide);
}

// ------------------------------------------------------------------ settings

BlurLook read_blur_look(obs_data_t *settings)
{
	BlurLook s;
	s.type = static_cast<BlurType>(obs_data_get_int(settings, "blur_type"));
	s.strength = std::clamp(obs_data_get_double(settings, "strength"), 0.0, kMaxBlurStrength);
	s.angle_deg = obs_data_get_double(settings, "angle");
	s.center_x = obs_data_get_double(settings, "center_x") / 100.0;
	s.center_y = obs_data_get_double(settings, "center_y") / 100.0;
	s.pixel_shape = static_cast<PixelShape>(std::clamp<long long>(obs_data_get_int(settings, "pixel_shape"), 0, 6));
	s.pixel_smooth = std::clamp(obs_data_get_double(settings, "pixel_smooth") / 100.0, 0.0, 1.0);
	s.pixel_glass = std::clamp(obs_data_get_double(settings, "pixel_glass") / 100.0, 0.0, 1.0);
	s.pixel_glass_style = (int)std::clamp<long long>(obs_data_get_int(settings, "pixel_glass_style"), 0, 1);
	s.origin_x = obs_data_get_double(settings, "origin_x") / 100.0;
	s.origin_y = obs_data_get_double(settings, "origin_y") / 100.0;
	s.rotation_deg = obs_data_get_double(settings, "rotation");
	s.whole_cells = obs_data_get_int(settings, "pixel_edge") == (int)MosaicEdge::Whole;
	// "Hexagons" was a blur type of its own before the mosaic had shapes.
	if (s.type == BlurType::Hexagon) {
		s.type = BlurType::Pixelate;
		s.pixel_shape = PixelShape::Hexagon;
		obs_data_set_int(settings, "blur_type", (int)BlurType::Pixelate);
		obs_data_set_int(settings, "pixel_shape", (int)PixelShape::Hexagon);
	}
	return s;
}

BlurRamp read_blur_ramp(obs_data_t *settings)
{
	BlurRamp r;
	r.style = static_cast<RampStyle>(obs_data_get_int(settings, "ramp_style"));
	r.in_s = std::max<long long>(0, obs_data_get_int(settings, "ramp_in_ms")) / 1000.0;
	r.out_s = std::max<long long>(0, obs_data_get_int(settings, "ramp_out_ms")) / 1000.0;
	r.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	r.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	return r;
}

void add_blur_look_properties(obs_properties_t *props)
{
	obs_properties_t *look = obs_properties_create();

	obs_property_t *type = obs_properties_add_list(look, "blur_type", obs_module_text("RampBlur.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Gaussian"), (int)BlurType::Gaussian);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Kawase"), (int)BlurType::Kawase);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Box"), (int)BlurType::Box);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Pixelate"), (int)BlurType::Pixelate);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Zoom"), (int)BlurType::Zoom);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Motion"), (int)BlurType::Motion);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Temporal"), (int)BlurType::Temporal);
	obs_property_set_long_description(type, obs_module_text("RampBlur.Type.Tip"));
	obs_property_set_modified_callback(type, type_modified);

	obs_properties_add_float_slider(look, "strength", obs_module_text("RampBlur.Strength"), 0.0, kMaxBlurStrength,
					0.5);
	obs_property_t *angle =
		obs_properties_add_float_slider(look, "angle", obs_module_text("RampBlur.Angle"), -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(angle, "°");
	obs_property_t *cx =
		obs_properties_add_float_slider(look, "center_x", obs_module_text("RampBlur.CenterX"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cx, "%");
	obs_property_t *cy =
		obs_properties_add_float_slider(look, "center_y", obs_module_text("RampBlur.CenterY"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cy, "%");
	obs_property_t *shape = obs_properties_add_list(look, "pixel_shape", obs_module_text("Mosaic.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	int shape_value = 0;
	for (const char *name : {"Mosaic.Square", "Mosaic.Hexagon", "Mosaic.Circle", "Mosaic.Triangle",
				 "Mosaic.Voronoi", "Mosaic.Rhomboid", "Mosaic.Triakis"})
		obs_property_list_add_int(shape, obs_module_text(name), shape_value++);
	obs_property_t *smooth = obs_properties_add_float_slider(look, "pixel_smooth", obs_module_text("Mosaic.Smooth"),
								 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(smooth, "%");
	obs_property_set_long_description(smooth, obs_module_text("Mosaic.Smooth.Tip"));
	obs_property_t *glass =
		obs_properties_add_float_slider(look, "pixel_glass", obs_module_text("Mosaic.Glass"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(glass, "%");
	obs_property_set_long_description(glass, obs_module_text("Mosaic.Glass.Tip"));
	obs_property_t *glass_style = obs_properties_add_list(look, "pixel_glass_style",
							      obs_module_text("Mosaic.GlassStyle"), OBS_COMBO_TYPE_LIST,
							      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(glass_style, obs_module_text("Mosaic.GlassStyle.Rounded"), 0);
	obs_property_list_add_int(glass_style, obs_module_text("Mosaic.GlassStyle.Edges"), 1);
	obs_property_set_long_description(glass_style, obs_module_text("Mosaic.GlassStyle.Tip"));
	obs_property_t *ox =
		obs_properties_add_float_slider(look, "origin_x", obs_module_text("Mosaic.OriginX"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(ox, "%");
	obs_property_set_long_description(ox, obs_module_text("Mosaic.Origin.Tip"));
	obs_property_t *oy =
		obs_properties_add_float_slider(look, "origin_y", obs_module_text("Mosaic.OriginY"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(oy, "%");
	obs_property_set_long_description(oy, obs_module_text("Mosaic.Origin.Tip"));
	obs_property_t *rotation = obs_properties_add_float_slider(look, "rotation", obs_module_text("Mosaic.Rotation"),
								   -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(rotation, "°");
	obs_property_t *edge = obs_properties_add_list(look, "pixel_edge", obs_module_text("Mosaic.Edge"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(edge, obs_module_text("Mosaic.Edge.Cut"), (int)MosaicEdge::Cut);
	obs_property_list_add_int(edge, obs_module_text("Mosaic.Edge.Whole"), (int)MosaicEdge::Whole);
	obs_property_set_long_description(edge, obs_module_text("Mosaic.Edge.Tip"));

	obs_properties_add_group(props, "look_group", obs_module_text("RampBlur.LookGroup"), OBS_GROUP_NORMAL, look);
}

void add_blur_ramp_properties(obs_properties_t *props)
{
	obs_properties_t *ramp = obs_properties_create();
	obs_property_t *style = obs_properties_add_list(ramp, "ramp_style", obs_module_text("RampBlur.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("RampBlur.Style.Grow"), (int)RampStyle::Grow);
	obs_property_list_add_int(style, obs_module_text("RampBlur.Style.Fade"), (int)RampStyle::Fade);
	obs_property_t *in =
		obs_properties_add_int_slider(ramp, "ramp_in_ms", obs_module_text("RampBlur.RampIn"), 0, 5000, 50);
	obs_property_int_set_suffix(in, " ms");
	obs_property_t *out =
		obs_properties_add_int_slider(ramp, "ramp_out_ms", obs_module_text("RampBlur.RampOut"), 0, 5000, 50);
	obs_property_int_set_suffix(out, " ms");
	add_easing_properties(ramp, "");
	obs_properties_add_group(props, "ramp_group", obs_module_text("RampBlur.RampGroup"), OBS_GROUP_NORMAL, ramp);
}

void blur_look_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "blur_type", (int)BlurType::Gaussian);
	obs_data_set_default_double(settings, "strength", 30.0);
	obs_data_set_default_double(settings, "angle", 0.0);
	obs_data_set_default_double(settings, "center_x", 50.0);
	obs_data_set_default_double(settings, "center_y", 50.0);
	obs_data_set_default_int(settings, "pixel_shape", (int)PixelShape::Square);
	obs_data_set_default_double(settings, "pixel_smooth", 0.0);
	obs_data_set_default_double(settings, "pixel_glass", 0.0);
	obs_data_set_default_int(settings, "pixel_glass_style", 0);
	obs_data_set_default_double(settings, "origin_x", 50.0);
	obs_data_set_default_double(settings, "origin_y", 50.0);
	obs_data_set_default_double(settings, "rotation", 0.0);
	obs_data_set_default_int(settings, "pixel_edge", (int)MosaicEdge::Cut);
	obs_data_set_default_int(settings, "ramp_style", (int)RampStyle::Grow);
	obs_data_set_default_int(settings, "ramp_in_ms", 400);
	obs_data_set_default_int(settings, "ramp_out_ms", 400);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

void blur_at_level(const BlurLook &look, RampStyle style, double level, double &strength, double &mix)
{
	strength = look.strength;
	mix = 1.0;
	if (style == RampStyle::Grow)
		strength *= level;
	else
		mix = std::min(level, 1.0);
	strength = std::min(strength, kMaxBlurStrength);
}

const char *blur_type_name(BlurType type, PixelShape shape)
{
	switch (type) {
	case BlurType::Gaussian:
		return "gaussian";
	case BlurType::Box:
		return "box";
	case BlurType::Kawase:
		return "kawase";
	case BlurType::Pixelate:
		// (Hexagons were a type of their own once, and are still named so.)
		return shape == PixelShape::Hexagon ? "hexagon" : "pixelate";
	case BlurType::Hexagon:
		return "hexagon";
	case BlurType::Temporal:
		return "temporal";
	case BlurType::Zoom:
		return "zoom";
	case BlurType::Motion:
		return "motion";
	}
	return "";
}

const char *blur_style_name(const BlurLook &look)
{
	switch (look.type) {
	case BlurType::Gaussian:
		return "gaussian";
	case BlurType::Box:
		return "box";
	case BlurType::Kawase:
		return "kawase";
	case BlurType::Zoom:
		return "zoom";
	case BlurType::Motion:
		return "motion";
	case BlurType::Temporal:
		return "trail";
	case BlurType::Pixelate:
	case BlurType::Hexagon:
		break;
	}
	if (look.pixel_glass > 0.0)
		return "glass";
	switch (look.type == BlurType::Hexagon ? PixelShape::Hexagon : look.pixel_shape) {
	case PixelShape::Square:
		return "pixelate";
	case PixelShape::Hexagon:
		return "hexagons";
	case PixelShape::Circle:
		return "mosaic_circle";
	case PixelShape::Triangle:
		return "mosaic_triangle";
	case PixelShape::Voronoi:
		return "mosaic_voronoi";
	case PixelShape::Rhomboid:
		return "mosaic_rhomboid";
	case PixelShape::Triakis:
		return "mosaic_triakis";
	}
	return "pixelate";
}

// ------------------------------------------------------------------ passes

BlurPipeline::BlurPipeline()
{
	char *path = obs_module_file("effects/stagekit-blur.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Ramp Blur: failed to load effect '%s': %s", path ? path : "(missing)",
			errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_image2_ = gs_effect_get_param_by_name(effect_, "image2");
		p_mix2_ = gs_effect_get_param_by_name(effect_, "mix2");
		p_original_ = gs_effect_get_param_by_name(effect_, "original");
		p_texel_ = gs_effect_get_param_by_name(effect_, "texel");
		p_size_ = gs_effect_get_param_by_name(effect_, "size");
		p_direction_ = gs_effect_get_param_by_name(effect_, "direction");
		p_radius_ = gs_effect_get_param_by_name(effect_, "radius");
		p_samples_ = gs_effect_get_param_by_name(effect_, "samples");
		p_offset_ = gs_effect_get_param_by_name(effect_, "offset");
		p_center_ = gs_effect_get_param_by_name(effect_, "center");
		p_strength_ = gs_effect_get_param_by_name(effect_, "strength");
		p_mix_ = gs_effect_get_param_by_name(effect_, "mix_amount");
	}
	char *looks_path = obs_module_file("effects/stagekit-looks.effect");
	char *looks_errors = nullptr;
	looks_ = looks_path ? gs_effect_create_from_file(looks_path, &looks_errors) : nullptr;
	if (!looks_)
		obs_log(LOG_ERROR,
			"Blur: failed to load effect '%s': %s. Cell shapes other than squares and hexagons, the Temporal type and Scene Blur are not available.",
			looks_path ? looks_path : "(missing)", looks_errors ? looks_errors : "unknown error");
	bfree(looks_errors);
	bfree(looks_path);
	if (looks_) {
		pl_image_ = gs_effect_get_param_by_name(looks_, "image");
		pl_image2_ = gs_effect_get_param_by_name(looks_, "image2");
		pl_mix2_ = gs_effect_get_param_by_name(looks_, "mix2");
		pl_texel_ = gs_effect_get_param_by_name(looks_, "texel");
		pl_size_ = gs_effect_get_param_by_name(looks_, "size");
		pl_strength_ = gs_effect_get_param_by_name(looks_, "strength");
		pl_shape_ = gs_effect_get_param_by_name(looks_, "shape");
		pl_smoothing_ = gs_effect_get_param_by_name(looks_, "smoothing");
		pl_glass_ = gs_effect_get_param_by_name(looks_, "glass");
		pl_glass_style_ = gs_effect_get_param_by_name(looks_, "glass_style");
		pl_whole_ = gs_effect_get_param_by_name(looks_, "whole");
		pl_origin_ = gs_effect_get_param_by_name(looks_, "origin");
		pl_turn_ = gs_effect_get_param_by_name(looks_, "turn");
		pl_original_ = gs_effect_get_param_by_name(looks_, "original");
		pl_mask_ = gs_effect_get_param_by_name(looks_, "mask");
		pl_mix_ = gs_effect_get_param_by_name(looks_, "mix_amount");
		pl_counted_ = gs_effect_get_param_by_name(looks_, "counted");
	}
	for (gs_texrender_t *&tr : tr_past_)
		tr = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	tr_a_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_b_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (LevelTargets *targets : {&tr_down_, &tr_up_, &tr_down2_, &tr_up2_})
		for (auto &tr : *targets)
			tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);
}

BlurPipeline::~BlurPipeline()
{
	obs_enter_graphics();
	gs_effect_destroy(effect_);
	gs_effect_destroy(looks_);
	gs_texrender_destroy(tr_a_);
	gs_texrender_destroy(tr_b_);
	for (gs_texrender_t *tr : tr_past_)
		gs_texrender_destroy(tr);
	for (LevelTargets *targets : {&tr_down_, &tr_up_, &tr_down2_, &tr_up2_})
		for (auto *tr : *targets)
			gs_texrender_destroy(tr);
	obs_leave_graphics();
}

void BlurPipeline::frame_passed(float seconds)
{
	frame_seconds_ = std::min(frame_seconds_.load() + seconds, 1.0f); // since the trail last moved on
	new_frame_ = true;
}

void BlurPipeline::pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique,
			bool looks)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin_with_color_space(dst, w, h, GS_CS_SRGB))
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	// Sample and write through sRGB views so the blur happens in linear
	// light; bright highlights spread naturally instead of going muddy.
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(looks ? pl_image_ : p_image_, src);
	while (gs_effect_loop(looks ? looks_ : effect_, technique))
		gs_draw_sprite(src, 0, w, h);
	gs_enable_framebuffer_srgb(previous);

	gs_blend_state_pop();
	gs_texrender_end(dst);
}

gs_texture_t *BlurPipeline::kawase_chain(gs_texture_t *input, uint32_t w, uint32_t h, int levels, float offset,
					 LevelTargets &down, LevelTargets &up, gs_texrender_t *out)
{
	std::array<uint32_t, kMaxKawaseLevels + 1> lw{}, lh{};
	lw[0] = w;
	lh[0] = h;
	for (int i = 1; i <= levels; i++) {
		lw[i] = std::max(1u, lw[i - 1] / 2);
		lh[i] = std::max(1u, lh[i - 1] / 2);
	}

	gs_texture_t *cur = input;
	for (int i = 0; i < levels; i++) {
		vec2 t;
		vec2_set(&t, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &t);
		gs_effect_set_float(p_offset_, offset);
		pass(down[i], cur, lw[i + 1], lh[i + 1], "KawaseDown");
		cur = gs_texrender_get_texture(down[i]);
	}
	for (int i = levels; i >= 1; i--) {
		vec2 t;
		vec2_set(&t, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &t);
		gs_effect_set_float(p_offset_, offset);
		gs_texrender_t *dst = i == 1 ? out : up[i - 1];
		pass(dst, cur, lw[i - 1], lh[i - 1], "KawaseUp");
		cur = gs_texrender_get_texture(dst);
	}
	return cur;
}

BlurPipeline::Result BlurPipeline::run(const BlurLook &s, gs_texture_t *input, uint32_t w, uint32_t h, double strength,
				       double &mix, BlurTrail *trail)
{
	if (!effect_)
		return {};
	vec2 texel, size;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);
	vec2_set(&size, (float)w, (float)h);
	gs_effect_set_vec2(p_texel_, &texel);
	gs_effect_set_vec2(p_size_, &size);

	// Gaussian weighting hides wide sample spacing; a flat box kernel does
	// not, so it gets twice the taps before spacing starts to stripe.
	const double max_taps = s.type == BlurType::Box ? 64.0 : 32.0;
	auto taps = [max_taps](double px) {
		return (float)std::clamp(std::ceil(px), 1.0, max_taps);
	};

	BlurType type = s.type;
	if ((type == BlurType::Gaussian || type == BlurType::Box) && s.strength > kTapLimit)
		type = BlurType::Kawase;
	switch (type) {
	case BlurType::Gaussian:
	case BlurType::Box: {
		const char *tech = s.type == BlurType::Gaussian ? "Gaussian" : "Box";
		vec2 dir;
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, taps(strength));
		vec2_set(&dir, texel.x, 0.0f);
		gs_effect_set_vec2(p_direction_, &dir);
		pass(tr_a_, input, w, h, tech);

		gs_effect_set_vec2(p_texel_, &texel);
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, taps(strength));
		vec2_set(&dir, 0.0f, texel.y);
		gs_effect_set_vec2(p_direction_, &dir);
		pass(tr_b_, gs_texrender_get_texture(tr_a_), w, h, tech);
		return {gs_texrender_get_texture(tr_b_)};
	}

	case BlurType::Motion: {
		const double rad = s.angle_deg * 3.14159265358979323846 / 180.0;
		vec2 dir;
		vec2_set(&dir, (float)std::cos(rad) * texel.x, (float)std::sin(rad) * texel.y);
		gs_effect_set_vec2(p_direction_, &dir);
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, (float)std::clamp(std::ceil(strength), 1.0, 64.0));
		pass(tr_a_, input, w, h, "Motion");
		return {gs_texrender_get_texture(tr_a_)};
	}

	case BlurType::Pixelate:
	case BlurType::Hexagon: {
		const double block = 1.0 + strength;
		if (!looks_) {
			// Without the newer shader file: squares and hexagons, as before it.
			gs_effect_set_float(p_strength_, (float)block);
			pass(tr_a_, input, w, h, s.pixel_shape == PixelShape::Hexagon ? "Hexagon" : "Pixelate");
			return {gs_texrender_get_texture(tr_a_)};
		}
		const double rad = s.rotation_deg * 3.14159265358979323846 / 180.0;
		vec2 origin, turn;
		vec2_set(&origin, (float)(s.origin_x * w), (float)(s.origin_y * h));
		vec2_set(&turn, (float)std::cos(rad), (float)std::sin(rad));
		gs_effect_set_vec2(pl_texel_, &texel);
		gs_effect_set_vec2(pl_size_, &size);
		gs_effect_set_float(pl_strength_, (float)block);
		gs_effect_set_float(pl_shape_, (float)(int)s.pixel_shape);
		gs_effect_set_float(pl_smoothing_, (float)(s.pixel_smooth * block * 0.2));
		if (pl_glass_)
			gs_effect_set_float(pl_glass_, (float)s.pixel_glass);
		if (pl_glass_style_)
			gs_effect_set_float(pl_glass_style_, (float)s.pixel_glass_style);
		if (pl_whole_)
			gs_effect_set_float(pl_whole_, s.whole_cells ? 1.0f : 0.0f);
		gs_effect_set_vec2(pl_origin_, &origin);
		gs_effect_set_vec2(pl_turn_, &turn);
		pass(tr_a_, input, w, h, "Mosaic", true);
		return {gs_texrender_get_texture(tr_a_)};
	}

	case BlurType::Temporal: {
		// A trail: each frame is the picture mixed with what was drawn the
		// frame before. The strength is how long a thing stays, as a
		// hundredth of a second for each point, whatever the frame rate.
		if (!looks_)
			return {};
		if (trail)
			return own_trail(*trail, input, w, h, strength);
		if (w != past_w_ || h != past_h_) {
			past_w_ = w;
			past_h_ = h;
			past_valid_ = false;
		}
		gs_texture_t *past = past_valid_ ? gs_texrender_get_texture(tr_past_[past_]) : nullptr;
		if (past && !new_frame_.exchange(false))
			return {past}; // drawn again in the same frame
		new_frame_ = false;
		const double lasts = std::max(strength / 100.0, 0.001);
		const double keep = past ? std::exp(-(double)frame_seconds_.exchange(0.0f) / lasts) : 0.0;
		gs_effect_set_texture_srgb(pl_image2_, past ? past : input);
		gs_effect_set_float(pl_mix2_, (float)std::clamp(keep, 0.0, 0.999));
		pass(tr_past_[1 - past_], input, w, h, "Temporal", true);
		past_ = 1 - past_;
		past_valid_ = true;
		return {gs_texrender_get_texture(tr_past_[past_])};
	}

	case BlurType::Zoom: {
		// Strength 250 pulls samples 60% of the way to the centre.
		// (Never all the way: at the centre itself there is nothing to pull.)
		const double amount = std::min(strength / 250.0 * 0.6, 0.95);
		const double reach_px = amount * 0.75 * std::max(w, h);
		vec2 center;
		vec2_set(&center, (float)s.center_x, (float)s.center_y);
		gs_effect_set_vec2(p_center_, &center);
		gs_effect_set_float(p_strength_, (float)amount);
		gs_effect_set_float(p_samples_, (float)std::clamp(std::ceil(reach_px), 1.0, 64.0));
		pass(tr_a_, input, w, h, "Zoom");
		return {gs_texrender_get_texture(tr_a_)};
	}

	case BlurType::Kawase: {
		// Each level halves the resolution. The level count and per-level
		// offset are chosen so the blur grows continuously with strength.
		// kKawaseScale makes a given strength look about as strong as the
		// Gaussian at the same number.
		constexpr double kKawaseScale = 0.14;
		const double scaled = strength * kKawaseScale;
		const double k = std::max(scaled, 1.0);
		if (scaled < 1.0)
			mix *= scaled; // fade in the first, smallest step
		auto fits = [w, h](int levels) {
			return (w >> levels) >= 2 && (h >> levels) >= 2;
		};
		int levels = std::clamp((int)std::floor(std::log2(k)) + 1, 1, kMaxKawaseLevels);
		while (levels > 1 && !fits(levels))
			levels--;
		const double offset = k / std::pow(2.0, levels - 1); // [1, 2) unless capped

		// Changing the level count abruptly shows as a visible pop while
		// ramping. Across each octave, cross-fade towards the result with
		// one more level at half the offset; at the end of the octave that
		// is exactly what the next octave starts with.
		Result out;
		out.a = kawase_chain(input, w, h, levels, (float)offset, tr_down_, tr_up_, tr_a_);
		if (levels < kMaxKawaseLevels && fits(levels + 1)) {
			out.b_mix = std::clamp(offset - 1.0, 0.0, 1.0);
			if (out.b_mix > 0.001)
				out.b = kawase_chain(input, w, h, levels + 1, (float)(offset * 0.5), tr_down2_, tr_up2_,
						     tr_b_);
		}
		return out;
	}
	}
	return {};
}

BlurPipeline::Result BlurPipeline::own_trail(BlurTrail &t, gs_texture_t *input, uint32_t w, uint32_t h, double strength)
{
	for (gs_texrender_t *&tr : t.past)
		if (!tr)
			tr = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	if (w != t.w || h != t.h) {
		t.w = w;
		t.h = h;
		t.valid = false;
	}
	const uint64_t now = obs_get_video_frame_time();
	gs_texture_t *past = t.valid ? gs_texrender_get_texture(t.past[t.index]) : nullptr;
	if (past && now == t.frame)
		return {past}; // drawn again in the same frame
	// (The frame's own time: never subtracted the wrong way round.)
	const double seconds = past && now > t.frame ? std::min((double)(now - t.frame) / 1e9, 1.0) : 0.0;
	t.frame = now;
	const double lasts = std::max(strength / 100.0, 0.001);
	const double keep = past ? std::exp(-seconds / lasts) : 0.0;
	gs_effect_set_texture_srgb(pl_image2_, past ? past : input);
	gs_effect_set_float(pl_mix2_, (float)std::clamp(keep, 0.0, 0.999));
	pass(t.past[1 - t.index], input, w, h, "Temporal", true);
	t.index = 1 - t.index;
	t.valid = true;
	return {gs_texrender_get_texture(t.past[t.index])};
}

gs_texture_t *BlurPipeline::blur_into(gs_texrender_t *into, const BlurLook &look, gs_texture_t *input, uint32_t w,
				      uint32_t h, double strength, double mix, BlurTrail *trail)
{
	if (!into || !input || !w || !h || !effect_)
		return nullptr;
	const Result blurred = run(look, input, w, h, strength, mix, trail);
	gs_texrender_reset(into);
	if (!gs_texrender_begin_with_color_space(into, w, h, GS_CS_SRGB))
		return nullptr;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	composite(input, blurred, w, h, mix);
	gs_blend_state_pop();
	gs_texrender_end(into);
	return gs_texrender_get_texture(into);
}

void forget_trail_pictures(BlurTrail &trail)
{
	for (gs_texrender_t *&tr : trail.past) {
		gs_texrender_destroy(tr);
		tr = nullptr;
	}
	trail.valid = false;
}

void BlurPipeline::composite(gs_texture_t *input, const Result &blurred, uint32_t w, uint32_t h, double mix)
{
	if (!effect_)
		return;
	gs_texture_t *a = blurred.a ? blurred.a : input;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(p_original_, input);
	gs_effect_set_texture_srgb(p_image_, a);
	gs_effect_set_texture_srgb(p_image2_, blurred.b ? blurred.b : a);
	gs_effect_set_float(p_mix2_, blurred.b ? (float)blurred.b_mix : 0.0f);
	gs_effect_set_float(p_mix_, blurred.a ? (float)std::clamp(mix, 0.0, 1.0) : 0.0f);
	while (gs_effect_loop(effect_, "Composite"))
		gs_draw_sprite(input, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
}

bool BlurPipeline::composite_masked(gs_texture_t *original, const Result &blurred, gs_texture_t *mask, uint32_t w,
				    uint32_t h, double mix)
{
	if (!looks_ || !original || !mask)
		return false;
	gs_texture_t *a = blurred.a ? blurred.a : original;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(pl_original_, original);
	gs_effect_set_texture_srgb(pl_image_, a);
	gs_effect_set_texture_srgb(pl_image2_, blurred.b ? blurred.b : a);
	// (The mask is a share, not a colour: read as it is.)
	gs_effect_set_texture(pl_mask_, mask);
	gs_effect_set_float(pl_mix2_, blurred.b ? (float)blurred.b_mix : 0.0f);
	gs_effect_set_float(pl_mix_, blurred.a ? (float)std::clamp(mix, 0.0, 1.0) : 0.0f);
	while (gs_effect_loop(looks_, "MaskedComposite"))
		gs_draw_sprite(original, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
	return true;
}

void BlurPipeline::draw_mask_piece(gs_texture_t *texture, uint32_t w, uint32_t h, bool counted)
{
	if (!looks_ || !texture)
		return;
	gs_effect_set_texture(pl_image_, texture);
	gs_effect_set_float(pl_counted_, counted ? 1.0f : 0.0f);
	while (gs_effect_loop(looks_, "MaskPiece"))
		gs_draw_sprite(texture, 0, w, h);
}

} // namespace stagekit
