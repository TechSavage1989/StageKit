/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "look-paint.hpp"
#include "outline-pattern.hpp"

#include "../common/scene-util.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace stagekit {

ShadowLook read_shadow_look(obs_data_t *settings)
{
	ShadowLook s;
	s.shadow = obs_data_get_bool(settings, "shadow");
	s.glow = obs_data_get_int(settings, "shadow_type") == 1;
	s.picture = obs_data_get_int(settings, "shadow_fill") == 1;
	s.color = (uint32_t)obs_data_get_int(settings, "shadow_color");
	s.opacity = std::clamp(obs_data_get_double(settings, "shadow_opacity") / 100.0, 0.0, 1.0);
	s.offset_x = std::clamp(obs_data_get_double(settings, "offset_x"), -500.0, 500.0);
	s.offset_y = std::clamp(obs_data_get_double(settings, "offset_y"), -500.0, 500.0);
	s.softness = std::clamp(obs_data_get_double(settings, "softness"), 0.0, 200.0);
	s.spread = std::clamp(obs_data_get_double(settings, "spread") / 100.0, 0.0, 1.0);
	s.boost = std::clamp(obs_data_get_double(settings, "shadow_boost") / 100.0, 0.0, 1.0);
	s.outline = obs_data_get_bool(settings, "outline");
	s.outline_width = std::clamp(obs_data_get_double(settings, "outline_width"), 0.0, 200.0);
	s.outline_color = (uint32_t)obs_data_get_int(settings, "outline_color");
	s.outline_opacity = std::clamp(obs_data_get_double(settings, "outline_opacity") / 100.0, 0.0, 1.0);
	s.outline_corners = (int)std::clamp<long long>(obs_data_get_int(settings, "outline_corners"), 0, 5);
	s.outline_corner_size = std::clamp(obs_data_get_double(settings, "outline_corner_size"), 0.0, 500.0);
	s.intensity = std::clamp(obs_data_get_double(settings, "glow_intensity") / 100.0, 0.25, 4.0);
	s.design = read_outline_design(settings);
	// A glow sits evenly all round.
	if (s.glow)
		s.offset_x = s.offset_y = 0.0;
	if (s.outline_width < 0.5)
		s.outline = false;
	return s;
}

OutlineDesign read_outline_design(obs_data_t *settings)
{
	OutlineDesign d;
	d.kind = (int)std::clamp<long long>(obs_data_get_int(settings, "outline_design"), 0, OutlineDesign::kLast);
	d.color2 = (uint32_t)obs_data_get_int(settings, "outline_color2");
	d.dash = std::clamp(obs_data_get_double(settings, "outline_dash"), 1.0, 400.0);
	d.gap = std::clamp(obs_data_get_double(settings, "outline_gap"), 1.0, 400.0);
	d.speed = std::clamp(obs_data_get_double(settings, "outline_speed"), -1000.0, 1000.0);
	d.glow = std::clamp(obs_data_get_double(settings, "outline_neon") / 100.0, 0.1, 0.9);
	d.color3 = (uint32_t)obs_data_get_int(settings, "outline_color3");
	d.pattern_scale = std::clamp(obs_data_get_double(settings, "outline_pattern_scale") / 100.0, 0.1, 4.0);
	// (Only the tests set this: another file in the patterns' place.)
	if (obs_data_has_user_value(settings, "test_pattern_effect"))
		outline_pattern_test_file(obs_data_get_string(settings, "test_pattern_effect"));
	return d;
}

void DesignUniforms::find(gs_effect_t *effect)
{
	kind_ = gs_effect_get_param_by_name(effect, "design_kind");
	color2_ = gs_effect_get_param_by_name(effect, "design_color2");
	dash_ = gs_effect_get_param_by_name(effect, "design_dash");
	gap_ = gs_effect_get_param_by_name(effect, "design_gap");
	phase_ = gs_effect_get_param_by_name(effect, "design_phase");
	turn_ = gs_effect_get_param_by_name(effect, "design_turn");
	pulse_ = gs_effect_get_param_by_name(effect, "design_pulse");
	glow_ = gs_effect_get_param_by_name(effect, "design_glow");
	pattern_tex_ = gs_effect_get_param_by_name(effect, "pattern_tex");
	pattern_on_ = gs_effect_get_param_by_name(effect, "pattern_on");
}

void DesignUniforms::set(const OutlineDesign &d, gs_texture_t *pattern, gs_texture_t *stand_in) const
{
	if (!kind_)
		return;
	// (Seconds within a long round, in double precision; every design's
	// cycle is wrapped before it reaches the shader.)
	const double seconds = (double)(obs_get_video_frame_time() % 3600000000000ULL) * 1e-9;
	auto wrapped = [](double x) {
		return x - std::floor(x);
	};
	const bool moves = d.kind >= OutlineDesign::Marching && d.speed != 0.0;
	const double travelled = moves ? d.speed * seconds : 0.0;
	vec4 two;
	vec4_from_rgba(&two, d.color2);
	gs_effect_set_float(kind_, (float)d.kind);
	gs_effect_set_vec4(color2_, &two);
	gs_effect_set_float(dash_, (float)d.dash);
	gs_effect_set_float(gap_, (float)d.gap);
	gs_effect_set_float(phase_, (float)wrapped(travelled / std::max(d.dash + d.gap, 1.0)));
	gs_effect_set_float(turn_, (float)wrapped(travelled / 600.0));
	gs_effect_set_float(
		pulse_, d.kind == OutlineDesign::Pulse
				? (float)(0.3 + 0.7 * (0.5 + 0.5 * std::cos(travelled * (6.283185307179586 / 100.0))))
				: 1.0f);
	gs_effect_set_float(glow_, (float)d.glow);
	if (stand_in && pattern_tex_ && pattern_on_) {
		gs_effect_set_texture(pattern_tex_, pattern ? pattern : stand_in);
		gs_effect_set_float(pattern_on_, pattern ? 1.0f : 0.0f);
	}
}

namespace {

bool design_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const long long kind = obs_data_get_int(settings, "outline_design");
	auto show = [&](const char *name, bool on) {
		if (obs_property_t *p = obs_properties_get(props, name))
			obs_property_set_visible(p, on);
	};
	const bool dashes = kind == OutlineDesign::Dashed || kind == OutlineDesign::Marching;
	const bool pattern = kind >= OutlineDesign::Aurora && kind <= OutlineDesign::Embers;
	show("outline_color2", pattern || kind == OutlineDesign::Gradient || kind == OutlineDesign::Travelling ||
				       kind == OutlineDesign::Shimmer);
	show("outline_color3", pattern);
	show("outline_pattern_scale", pattern);
	show("outline_dash", dashes);
	show("outline_gap", dashes || kind == OutlineDesign::Dotted);
	show("outline_speed", kind >= OutlineDesign::Marching);
	show("outline_neon", kind == OutlineDesign::Neon);
	return true;
}

bool corners_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_set_visible(obs_properties_get(props, "outline_corner_size"),
				 obs_data_get_int(settings, "outline_corners") > 0 &&
					 obs_data_get_int(settings, "outline_corners") != 5);
	return true;
}

} // namespace

void add_outline_corner_properties(obs_properties_t *outline)
{
	obs_property_t *corners = obs_properties_add_list(outline, "outline_corners",
							  obs_module_text("Shadow.OutlineCorners"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Follow"), 0);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Round"), 1);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Cut"), 2);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Notch"), 3);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Scoop"), 4);
	obs_property_list_add_int(corners, obs_module_text("Shadow.OutlineCorners.Square"), 5);
	set_tip(corners, obs_module_text("Shadow.OutlineCorners.Tip"));
	obs_property_set_modified_callback(corners, corners_modified);
	obs_property_t *size = obs_properties_add_float_slider(
		outline, "outline_corner_size", obs_module_text("Shadow.OutlineCornerSize"), 0.0, 200.0, 1.0);
	obs_property_float_set_suffix(size, " px");
	set_tip(size, obs_module_text("Shadow.OutlineCornerSize.Tip"));
}

void add_outline_design_properties(obs_properties_t *outline)
{
	obs_property_t *design = obs_properties_add_list(outline, "outline_design",
							 obs_module_text("Shadow.OutlineDesign"), OBS_COMBO_TYPE_LIST,
							 OBS_COMBO_FORMAT_INT);
	static const char *const names[] = {"Solid",  "Double",   "Dashed",     "Dotted",  "Gradient", "Neon",
					    "Inner",  "Marching", "Travelling", "Pulse",   "Shimmer",  "Rainbow",
					    "Aurora", "Plasma",   "Lava",       "Waves",   "Nebula",   "Flow",
					    "Stars",  "Bokeh",    "Rays",       "Circuit", "Hex",      "Rain",
					    "Tunnel", "Ripples",  "Embers"};
	for (int i = 0; i <= OutlineDesign::kLast; i++)
		obs_property_list_add_int(
			design, obs_module_text((std::string("Shadow.OutlineDesign.") + names[i]).c_str()), i);
	set_tip(design, obs_module_text("Shadow.OutlineDesign.Tip"));
	obs_property_set_modified_callback(design, design_modified);
	obs_property_t *two =
		obs_properties_add_color(outline, "outline_color2", obs_module_text("Shadow.OutlineColor2"));
	set_tip(two, obs_module_text("Shadow.OutlineColor2.Tip"));
	obs_property_t *three =
		obs_properties_add_color(outline, "outline_color3", obs_module_text("Shadow.OutlineColor3"));
	set_tip(three, obs_module_text("Shadow.OutlineColor3.Tip"));
	auto slider = [&](const char *name, const char *text, double min, double max, const char *suffix) {
		obs_property_t *p =
			obs_properties_add_float_slider(outline, name, obs_module_text(text), min, max, 1.0);
		if (suffix)
			obs_property_float_set_suffix(p, suffix);
		return p;
	};
	slider("outline_dash", "Shadow.OutlineDash", 1.0, 200.0, " px");
	slider("outline_gap", "Shadow.OutlineGap", 1.0, 200.0, " px");
	set_tip(slider("outline_speed", "Shadow.OutlineSpeed", -600.0, 600.0, nullptr),
		obs_module_text("Shadow.OutlineSpeed.Tip"));
	set_tip(slider("outline_neon", "Shadow.OutlineNeon", 10.0, 90.0, "%"),
		obs_module_text("Shadow.OutlineNeon.Tip"));
	set_tip(slider("outline_pattern_scale", "Shadow.OutlinePatternScale", 10.0, 400.0, "%"),
		obs_module_text("Shadow.OutlinePatternScale.Tip"));
}

void add_intensity_property(obs_properties_t *shadow)
{
	obs_property_t *p = obs_properties_add_float_slider(shadow, "glow_intensity",
							    obs_module_text("Shadow.Intensity"), 25.0, 400.0, 1.0);
	obs_property_float_set_suffix(p, "%");
	set_tip(p, obs_module_text("Shadow.Intensity.Tip"));
}

void outline_glow_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "outline_corners", 0);
	obs_data_set_default_double(settings, "outline_corner_size", 20.0);
	obs_data_set_default_double(settings, "glow_intensity", 100.0);
	const OutlineDesign d;
	obs_data_set_default_int(settings, "outline_design", d.kind);
	obs_data_set_default_int(settings, "outline_color2", d.color2);
	obs_data_set_default_double(settings, "outline_dash", d.dash);
	obs_data_set_default_double(settings, "outline_gap", d.gap);
	obs_data_set_default_double(settings, "outline_speed", d.speed);
	obs_data_set_default_double(settings, "outline_neon", d.glow * 100.0);
	obs_data_set_default_int(settings, "outline_color3", d.color3);
	obs_data_set_default_double(settings, "outline_pattern_scale", d.pattern_scale * 100.0);
}

ShadowLook scaled_look(ShadowLook look, double level)
{
	level = std::clamp(level, 0.0, 1.0);
	look.opacity *= level;
	look.outline_opacity *= level;
	return look;
}

namespace {

uint32_t mix_color(uint32_t a, uint32_t b, double t)
{
	uint32_t out = 0;
	for (int shift = 0; shift < 32; shift += 8) {
		const double x = (double)((a >> shift) & 0xFF), y = (double)((b >> shift) & 0xFF);
		out |= (uint32_t)std::lround(std::clamp(x + (y - x) * t, 0.0, 255.0)) << shift;
	}
	return out;
}

double mix(double x, double y, double t)
{
	return x + (y - x) * t;
}

} // namespace

OutlineDesign lerp_design(const OutlineDesign &a, const OutlineDesign &b, double t)
{
	OutlineDesign out = t < 0.5 ? a : b;
	// (Only between two of one kind do the sizes glide: a dash length means
	// nothing to a solid line.)
	if (a.kind == b.kind) {
		out.color2 = mix_color(a.color2, b.color2, t);
		out.dash = mix(a.dash, b.dash, t);
		out.gap = mix(a.gap, b.gap, t);
		out.glow = mix(a.glow, b.glow, t);
		out.color3 = mix_color(a.color3, b.color3, t);
		out.pattern_scale = mix(a.pattern_scale, b.pattern_scale, t);
	}
	return out;
}

ShadowLook lerp_look(const ShadowLook *a, const ShadowLook *b, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	if (!a && !b) {
		ShadowLook none;
		none.shadow = none.outline = false;
		return none;
	}
	// A missing side is the other one, unseen.
	ShadowLook from = a ? *a : *b, to = b ? *b : *a;
	if (!a)
		from.opacity = from.outline_opacity = 0.0;
	if (!b)
		to.opacity = to.outline_opacity = 0.0;
	// A part switched off on one side: the other side's, unseen.
	auto part = [](ShadowLook &x, const ShadowLook &y) {
		if (!x.shadow && y.shadow) {
			const double outline_width = x.outline_width, outline_opacity = x.outline_opacity;
			const uint32_t outline_color = x.outline_color;
			const bool outline = x.outline;
			const int corners = x.outline_corners;
			const double corner_size = x.outline_corner_size;
			const OutlineDesign design = x.design;
			x = ShadowLook(y);
			x.design = design;
			x.opacity = 0.0;
			x.outline = outline;
			x.outline_width = outline_width;
			x.outline_color = outline_color;
			x.outline_opacity = outline_opacity;
			x.outline_corners = corners;
			x.outline_corner_size = corner_size;
		}
		if (!x.outline && y.outline) {
			x.outline = true;
			x.outline_width = y.outline_width;
			x.outline_color = y.outline_color;
			x.outline_opacity = 0.0;
			x.outline_corners = y.outline_corners;
			x.outline_corner_size = y.outline_corner_size;
			x.design = y.design;
		}
	};
	part(from, to);
	part(to, from);
	ShadowLook out;
	out.shadow = from.shadow || to.shadow;
	out.glow = t < 0.5 ? from.glow : to.glow;
	out.picture = t < 0.5 ? from.picture : to.picture;
	out.color = mix_color(from.color, to.color, t);
	out.opacity = mix(from.opacity, to.opacity, t);
	out.offset_x = mix(from.offset_x, to.offset_x, t);
	out.offset_y = mix(from.offset_y, to.offset_y, t);
	out.softness = mix(from.softness, to.softness, t);
	out.spread = mix(from.spread, to.spread, t);
	out.boost = mix(from.boost, to.boost, t);
	out.outline = from.outline || to.outline;
	out.outline_width = mix(from.outline_width, to.outline_width, t);
	out.outline_color = mix_color(from.outline_color, to.outline_color, t);
	out.outline_opacity = mix(from.outline_opacity, to.outline_opacity, t);
	// (The kind of corners changes half-way, as Corners & Edges' style does.)
	out.outline_corners = t < 0.5 ? from.outline_corners : to.outline_corners;
	out.outline_corner_size = mix(from.outline_corner_size, to.outline_corner_size, t);
	out.intensity = mix(from.intensity, to.intensity, t);
	out.design = lerp_design(from.design, to.design, t);
	return out;
}

// OBS starts an item's show transition with nothing on its first side and
// the item's source on its second, and a hide transition the other way
// round; when one ends, the second side moves to the first. So while a show
// runs the source is its second side, and while a hide runs its first.
bool item_transition_running(obs_sceneitem_t *item, bool show)
{
	obs_source_t *transition = item ? obs_sceneitem_get_transition(item, show) : nullptr;
	if (!transition || obs_sceneitem_visible(item) != show)
		return false;
	obs_source_t *side =
		obs_transition_get_source(transition, show ? OBS_TRANSITION_SOURCE_B : OBS_TRANSITION_SOURCE_A);
	const bool running = side && side == obs_sceneitem_get_source(item);
	obs_source_release(side);
	return running;
}

LookPaint::LookPaint()
{
	char *path = obs_module_file("effects/stagekit-scene-shadow.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Scene Shadow: failed to load effect '%s': %s. Scenes are drawn without it.",
			path ? path : "(missing)", errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_shadow_ = gs_effect_get_param_by_name(effect_, "shadow");
		p_cast_ = gs_effect_get_param_by_name(effect_, "silhouette");
		p_mask_ = gs_effect_get_param_by_name(effect_, "mask");
		p_out_size_ = gs_effect_get_param_by_name(effect_, "out_size");
		p_offset_ = gs_effect_get_param_by_name(effect_, "shadow_offset");
		p_color_ = gs_effect_get_param_by_name(effect_, "shadow_color");
		p_gain_ = gs_effect_get_param_by_name(effect_, "shadow_gain");
		p_picture_ = gs_effect_get_param_by_name(effect_, "shadow_picture");
		p_boost_ = gs_effect_get_param_by_name(effect_, "shadow_boost");
		p_outline_width_ = gs_effect_get_param_by_name(effect_, "outline_width");
		p_outline_color_ = gs_effect_get_param_by_name(effect_, "outline_color");
		p_straight_ = gs_effect_get_param_by_name(effect_, "image_straight");
		p_ring_shape_ = gs_effect_get_param_by_name(effect_, "ring_shape");
		p_ring_box_ = gs_effect_get_param_by_name(effect_, "ring_box");
		p_intensity_ = gs_effect_get_param_by_name(effect_, "glow_intensity");
		p_box_size_ = gs_effect_get_param_by_name(effect_, "box_size");
		p_box_rect_ = gs_effect_get_param_by_name(effect_, "box_rect");
		p_corner_size_ = gs_effect_get_param_by_name(effect_, "corner_size");
		p_corner_style_ = gs_effect_get_param_by_name(effect_, "corner_style");
		p_ring_strength_ = gs_effect_get_param_by_name(effect_, "ring_strength");
		p_rect_tex_ = gs_effect_get_param_by_name(effect_, "rect_tex");
		p_rect_known_ = gs_effect_get_param_by_name(effect_, "rect_known");
		p_rect_pic_ = gs_effect_get_param_by_name(effect_, "rect_pic");
		p_area_tex_ = gs_effect_get_param_by_name(effect_, "area_tex");
		p_force_follow_ = gs_effect_get_param_by_name(effect_, "force_follow");
		p_follow_amount_ = gs_effect_get_param_by_name(effect_, "follow_amount");
		p_follow_tex_ = gs_effect_get_param_by_name(effect_, "follow_tex");
		p_presence_ = gs_effect_get_param_by_name(effect_, "presence");
		design_.find(effect_);
		has_presence_ = gs_effect_get_technique(effect_, "Presence") != nullptr;
		has_mix_ = gs_effect_get_technique(effect_, "MixSides") != nullptr;
	}
	// (Smooth: sixteen-bit pictures and one more level, so a soft shadow's
	// tail has no steps.)
	blur_ = std::make_unique<KawaseBlur>(true);
	rects_ = std::make_unique<VisibleRect>();
	{
		// (A stand-in for rect_tex when no rectangle is known: every
		// texture an effect reads is given one.)
		const float none[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		const uint8_t *data = reinterpret_cast<const uint8_t *>(none);
		no_rect_ = gs_texture_create(1, 1, GS_RGBA32F, 1, &data, 0);
	}
	out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	outlined_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);
}

LookPaint::~LookPaint()
{
	obs_enter_graphics();
	blur_.reset();
	rects_.reset();
	gs_texture_destroy(no_rect_);
	gs_effect_destroy(effect_);
	gs_texrender_destroy(out_);
	gs_texrender_destroy(outlined_);
	for (gs_texrender_t *tr : between_)
		gs_texrender_destroy(tr);
	for (gs_texrender_t *tr : spares_)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
}

gs_texrender_t *LookPaint::spare()
{
	if (spare_ >= spares_.size())
		spares_.push_back(gs_texrender_create(GS_RGBA, GS_ZS_NONE));
	return spares_[spare_++];
}

// What an item draws now: its source, or the transition it is showing or
// hiding with. Nothing for a hidden item with none, or one with no size.
struct ItemDraw {
	obs_source_t *source = nullptr;
	obs_source_t *transition = nullptr;
	uint32_t sw = 0, sh = 0;
	obs_sceneitem_crop crop = {};
	int cw = 0, ch = 0;
};

namespace {

bool item_draw(obs_sceneitem_t *item, ItemDraw &out)
{
	out.source = item ? obs_sceneitem_get_source(item) : nullptr;
	if (!out.source)
		return false;
	const bool visible = obs_sceneitem_visible(item);
	if (item_transition_running(item, visible))
		out.transition = obs_sceneitem_get_transition(item, visible);
	else if (!visible)
		return false;
	out.sw = obs_source_get_width(out.source);
	out.sh = obs_source_get_height(out.source);
	if (!out.sw || !out.sh)
		return false;
	obs_sceneitem_get_crop(item, &out.crop);
	out.cw = (int)out.sw - out.crop.left - out.crop.right;
	out.ch = (int)out.sh - out.crop.top - out.crop.bottom;
	return out.cw >= 1 && out.ch >= 1;
}

void draw_source(const ItemDraw &d)
{
	if (d.transition) {
		obs_transition_set_size(d.transition, d.sw, d.sh);
		obs_source_video_render(d.transition);
	} else {
		obs_source_video_render(d.source);
	}
}

} // namespace

bool item_drawn_box(obs_sceneitem_t *item, LayerBox &out)
{
	out = LayerBox();
	ItemDraw d;
	if (!item_draw(item, d))
		return false;
	matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
	for (int i = 0; i < 4; i++) {
		vec3 corner, at;
		vec3_set(&corner, (i & 1) ? (float)d.cw : 0.0f, (i & 2) ? (float)d.ch : 0.0f, 0.0f);
		vec3_transform(&at, &corner, &m);
		x0 = std::min(x0, (double)at.x);
		y0 = std::min(y0, (double)at.y);
		x1 = std::max(x1, (double)at.x);
		y1 = std::max(y1, (double)at.y);
	}
	out.present = true;
	out.x0 = x0;
	out.y0 = y0;
	out.x1 = x1;
	out.y1 = y1;
	return true;
}

double shadow_reach(const ShadowLook &look)
{
	double reach = 2.0;
	if (look.shadow)
		reach += std::max(std::abs(look.offset_x), std::abs(look.offset_y)) + 2.0 * look.softness;
	if (look.outline)
		reach = std::max(reach, look.outline_width + 2.0);
	return reach;
}

std::vector<size_t> shadow_layers(const std::vector<LayerBox> &boxes, double reach, size_t most)
{
	std::vector<size_t> starts;
	std::vector<const LayerBox *> run; // the present boxes of the run being made
	for (size_t i = 0; i < boxes.size(); i++) {
		const LayerBox &b = boxes[i];
		if (!b.present)
			continue;
		bool meets = false;
		for (const LayerBox *o : run)
			meets = meets || (b.x0 - reach < o->x1 + reach && o->x0 - reach < b.x1 + reach &&
					  b.y0 - reach < o->y1 + reach && o->y0 - reach < b.y1 + reach);
		if (starts.empty() || (meets && starts.size() < std::max<size_t>(most, 1))) {
			starts.push_back(starts.empty() ? 0 : i);
			run.clear();
		}
		run.push_back(&b);
	}
	return starts;
}

gs_texture_t *LookPaint::picture_of(const ItemDraw &d)
{
	gs_texrender_t *into = spare();
	gs_texrender_reset(into);
	if (!gs_texrender_begin(into, (uint32_t)d.cw, (uint32_t)d.ch))
		return nullptr;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho((float)d.crop.left, (float)(d.crop.left + d.cw), (float)d.crop.top, (float)(d.crop.top + d.ch),
		 -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	draw_source(d);
	gs_blend_state_pop();
	gs_texrender_end(into);
	return gs_texrender_get_texture(into);
}

gs_texture_t *LookPaint::item_picture(obs_sceneitem_t *item, matrix4 &m, uint32_t &w, uint32_t &h)
{
	ItemDraw d;
	if (!item_draw(item, d))
		return nullptr;
	gs_texture_t *texture = picture_of(d);
	if (!texture)
		return nullptr;
	obs_sceneitem_get_draw_transform(item, &m);
	w = (uint32_t)d.cw;
	h = (uint32_t)d.ch;
	return texture;
}

void LookPaint::draw_item(obs_sceneitem_t *item)
{
	ItemDraw d;
	if (!item_draw(item, d))
		return;
	matrix4 m;
	obs_sceneitem_get_draw_transform(item, &m);
	if (!d.crop.left && !d.crop.top && !d.crop.right && !d.crop.bottom) {
		gs_matrix_push();
		gs_matrix_mul(&m);
		draw_source(d);
		gs_matrix_pop();
		return;
	}
	// Cropped: by way of a picture of what is left, as OBS draws it.
	gs_texture_t *texture = picture_of(d);
	if (!texture)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA); // the picture is premultiplied
	gs_matrix_push();
	gs_matrix_mul(&m);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, (uint32_t)d.cw, (uint32_t)d.ch);
	gs_matrix_pop();
	gs_blend_state_pop();
}

void LookPaint::draw_picture(gs_texture_t *texture, const matrix4 &m, uint32_t w, uint32_t h)
{
	if (!texture || !w || !h)
		return;
	// (Raw values in, raw values out, as the canvases hold them.)
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_matrix_push();
	gs_matrix_mul(&m);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, w, h);
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(previous);
}

bool LookPaint::begin_canvas(gs_texrender_t *into, uint32_t cx, uint32_t cy)
{
	gs_texrender_reset(into);
	if (!gs_texrender_begin(into, cx, cy))
		return false;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
	gs_blend_state_push();
	// As OBS fills a filter's picture (draw_filter_input).
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	return true;
}

void LookPaint::end_canvas(gs_texrender_t *into)
{
	gs_blend_state_pop();
	gs_texrender_end(into);
}

void LookPaint::copy_onto(gs_texture_t *texture, uint32_t cx, uint32_t cy, bool over)
{
	if (!texture)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, over ? GS_BLEND_INVSRCALPHA : GS_BLEND_ZERO);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);
	gs_blend_state_pop();
}

bool LookPaint::compose(gs_texture_t *scene, gs_texture_t *cast, gs_texture_t *mask, const ShadowLook &look,
			uint32_t cx, uint32_t cy, gs_texrender_t *into, bool scene_straight, gs_texture_t *ring,
			gs_texture_t *presence)
{
	gs_texrender_t *out = into ? into : out_;
	if (!effect_ || !scene || !cast || !cx || !cy)
		return false;
	// The passes work on raw colour values, as Shadow, Glow & Outline's do.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	vec2 size, offset;
	vec2_set(&size, (float)cx, (float)cy);
	vec2_set(&offset, look.glow ? 0.0f : (float)look.offset_x, look.glow ? 0.0f : (float)look.offset_y);
	vec4 color, line;
	vec4_from_rgba(&color, look.color);
	color.w = look.shadow ? (float)look.opacity : 0.0f;
	vec4_from_rgba(&line, look.outline_color);
	line.w = look.outline ? (float)look.outline_opacity : 0.0f;
	const bool has_ring = look.outline && look.outline_width >= 0.5 && look.outline_opacity > 0.001;
	const bool box = has_ring && look.box_line() && ring;
	// (Every value an effect reads, at every pass: OBS empties them after
	// each technique, and Direct3D draws nothing while one has none.)
	// A pattern fill: the pattern over the whole canvas, which the line is a
	// window onto (drawn before any of this file's passes begin).
	gs_texture_t *pattern = has_ring ? outline_pattern_texture(look.design, look.outline_color, cx, cy) : nullptr;
	auto line_values = [&] {
		gs_effect_set_texture(p_cast_, cast);
		gs_effect_set_texture(p_ring_shape_, box ? ring : cast);
		gs_effect_set_texture(p_follow_tex_, box ? ring : cast);
		gs_effect_set_float(p_ring_box_, box ? 1.0f : 0.0f);
		gs_effect_set_vec2(p_out_size_, &size);
		gs_effect_set_float(p_outline_width_, (float)look.outline_width);
		gs_effect_set_vec4(p_outline_color_, &line);
		design_.set(look.design, pattern, cast);
	};
	// The shadow is thrown by the sources with their line round them, as the
	// per-source filter's is (its shadow comes from the outlined picture):
	// from the bare sources, the shadow of an outlined source started under
	// its line and showed square corners behind a rounded one.
	gs_texture_t *thrower = cast;
	if (has_ring && look.shadow && look.opacity > 0.001) {
		gs_texrender_reset(outlined_);
		if (gs_texrender_begin(outlined_, cx, cy)) {
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			line_values();
			gs_effect_set_texture(p_image_, cast);
			while (gs_effect_loop(effect_, "Outlined"))
				gs_draw_sprite(cast, 0, cx, cy);
			gs_blend_state_pop();
			gs_texrender_end(outlined_);
			if (gs_texture_t *grown = gs_texrender_get_texture(outlined_))
				thrower = grown;
		}
	}
	gs_texture_t *softened = thrower;
	if (look.shadow && look.softness > 0.0)
		softened = blur_->blur(thrower, cx, cy, look.softness);
	gs_texrender_reset(out);
	bool drawn = false;
	if (gs_texrender_begin(out, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_effect_set_texture(p_image_, scene);
		gs_effect_set_texture(p_shadow_, softened);
		gs_effect_set_texture(p_mask_, mask ? mask : cast);
		gs_effect_set_texture(p_presence_, presence ? presence : mask ? mask : cast);
		gs_effect_set_vec2(p_offset_, &offset);
		gs_effect_set_vec4(p_color_, &color);
		gs_effect_set_float(p_gain_, (float)(1.0 + look.spread * 6.0));
		gs_effect_set_float(p_picture_, look.picture ? 1.0f : 0.0f);
		gs_effect_set_float(p_boost_, (float)look.boost);
		gs_effect_set_float(p_straight_, scene_straight ? 1.0f : 0.0f);
		gs_effect_set_float(p_intensity_, (float)look.intensity);
		line_values();
		while (gs_effect_loop(effect_, has_ring ? "Ring" : "MaskedShadow"))
			gs_draw_sprite(scene, 0, cx, cy);
		gs_blend_state_pop();
		gs_texrender_end(out);
		drawn = true;
	}
	gs_enable_framebuffer_srgb(previous_srgb);
	return drawn;
}

VisibleRect::VisibleRect()
{
	char *path = obs_module_file("effects/stagekit-looks.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_WARNING, "outline: could not load '%s' (%s); lines go round the whole box",
			path ? path : "(missing)", errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_in_size_ = gs_effect_get_param_by_name(effect_, "rect_in_size");
		// (A file of the others' may not have the techniques.)
		for (const char *t : {"RectFirst", "RectReduce", "AreaFirst", "AreaReduce"})
			if (!gs_effect_get_technique(effect_, t))
				effect_ = nullptr;
		has_top_ = effect_ && gs_effect_get_technique(effect_, "TopFirst") &&
			   gs_effect_get_technique(effect_, "TopReduce");
	}
	obs_leave_graphics();
	bfree(path);
}

VisibleRect::~VisibleRect()
{
	obs_enter_graphics();
	for (auto &set : sets_)
		for (gs_texrender_t *tr : set)
			gs_texrender_destroy(tr);
	gs_effect_destroy(effect_);
	obs_leave_graphics();
}

// Blocks of 8 by 8 at a time, down to one pixel, in a chain of its own for
// this frame (each result is kept until begin_frame).
gs_texture_t *VisibleRect::reduce(gs_texture_t *picture, uint32_t w, uint32_t h, const char *first_pass,
				  const char *rest)
{
	if (next_ >= sets_.size())
		sets_.emplace_back();
	std::vector<gs_texrender_t *> &levels = sets_[next_++];
	gs_texture_t *cur = picture;
	uint32_t cw = w, ch = h;
	size_t level = 0;
	bool first = true;
	while (cur && (first || cw > 1 || ch > 1)) {
		const uint32_t ow = (cw + 7) / 8, oh = (ch + 7) / 8;
		if (level >= levels.size())
			levels.push_back(gs_texrender_create(GS_RGBA32F, GS_ZS_NONE));
		gs_texrender_t *into = levels[level++];
		gs_texrender_reset(into);
		if (!gs_texrender_begin(into, ow, oh))
			return nullptr;
		gs_ortho(0.0f, (float)ow, 0.0f, (float)oh, -100.0f, 100.0f);
		// (No blending: float pictures need not be blendable.)
		gs_blend_state_push();
		gs_enable_blending(false);
		vec2 in_size;
		vec2_set(&in_size, (float)cw, (float)ch);
		gs_effect_set_texture(p_image_, cur);
		gs_effect_set_vec2(p_in_size_, &in_size);
		while (gs_effect_loop(effect_, first ? first_pass : rest))
			gs_draw_sprite(nullptr, 0, ow, oh);
		gs_blend_state_pop();
		gs_texrender_end(into);
		cur = gs_texrender_get_texture(into);
		cw = ow;
		ch = oh;
		first = false;
	}
	return cur;
}

Seen VisibleRect::measure(gs_texture_t *picture, uint32_t w, uint32_t h)
{
	Seen out;
	if (!effect_ || !picture || !w || !h)
		return out;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_matrix_push();
	gs_matrix_identity();
	out.rect = reduce(picture, w, h, "RectFirst", "RectReduce");
	out.area = out.rect ? reduce(picture, w, h, "AreaFirst", "AreaReduce") : nullptr;
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(previous);
	if (!out.known())
		out = Seen();
	return out;
}

gs_texture_t *VisibleRect::top(gs_texture_t *picture, uint32_t w, uint32_t h)
{
	if (!effect_ || !has_top_ || !picture || !w || !h)
		return nullptr;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_matrix_push();
	gs_matrix_identity();
	gs_texture_t *out = reduce(picture, w, h, "TopFirst", "TopReduce");
	gs_matrix_pop();
	gs_enable_framebuffer_srgb(previous);
	return out;
}

bool LookPaint::may_fade(obs_sceneitem_t *item)
{
	return item && (is_morph_layer(item) || item_transition_running(item, obs_sceneitem_visible(item)));
}

void LookPaint::draw_presence(obs_sceneitem_t *item)
{
	matrix4 m;
	uint32_t w = 0, h = 0;
	gs_texture_t *picture = item_picture(item, m, w, h);
	if (!picture || !w || !h)
		return;
	gs_texture_t *top = effect_ && has_presence_ && rects_ ? rects_->top(picture, w, h) : nullptr;
	if (!top) {
		// As it is drawn: the look of before.
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		draw_picture(picture, m, w, h);
		gs_blend_state_pop();
		return;
	}
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_mul(&m);
	gs_effect_set_texture(p_image_, picture);
	gs_effect_set_texture(p_area_tex_, top);
	while (gs_effect_loop(effect_, "Presence"))
		gs_draw_sprite(picture, 0, w, h);
	gs_matrix_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

Seen LookPaint::visible_rect(gs_texture_t *picture, uint32_t w, uint32_t h)
{
	return rects_ ? rects_->measure(picture, w, h) : Seen();
}

namespace {

// The rect and area values (or the stand-in), for BoxRing and FollowGate.
void set_seen(gs_eparam_t *rect, gs_eparam_t *area, gs_eparam_t *known, gs_eparam_t *pic, const Seen &seen, double pw,
	      double ph, gs_texture_t *none)
{
	const bool ok = seen.known() && pw > 0.0 && ph > 0.0;
	vec2 size;
	vec2_set(&size, ok ? (float)pw : 1.0f, ok ? (float)ph : 1.0f);
	gs_effect_set_texture(rect, ok ? seen.rect : none);
	gs_effect_set_texture(area, ok ? seen.area : none);
	gs_effect_set_float(known, ok ? 1.0f : 0.0f);
	gs_effect_set_vec2(pic, &size);
}

// Both halves are drawn taking the larger value, each channel by itself.
void max_blending()
{
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	gs_blend_op(GS_BLEND_OP_MAX);
}

} // namespace

void LookPaint::draw_box_ring(const matrix4 &m, const ShadowLook &look, float strength, const Seen &seen, double pw,
			      double ph, bool turned)
{
	if (!effect_ || !look.box_line() || strength <= 0.001f || turned)
		return;
	// The box's sides on the canvas.
	const double bw = std::hypot((double)m.x.x, (double)m.x.y);
	const double bh = std::hypot((double)m.y.x, (double)m.y.y);
	if (bw < 0.5 || bh < 0.5)
		return;
	// A sprite of the box grown by the line (and a pixel for its smoothed
	// edge), in canvas pixels, under the box's own turn.
	const double grow = look.outline_width + 1.0;
	const double sw = std::ceil(bw + 2.0 * grow), sh = std::ceil(bh + 2.0 * grow);
	vec2 size;
	vec2_set(&size, (float)sw, (float)sh);
	vec4 rect;
	vec4_set(&rect, (float)grow, (float)grow, (float)bw, (float)bh);
	gs_effect_set_vec2(p_box_size_, &size);
	gs_effect_set_vec4(p_box_rect_, &rect);
	// (Square: a rounded corner of no size.)
	const bool square = look.outline_corners == 5;
	gs_effect_set_float(p_corner_size_, square ? 0.0f : (float)look.outline_corner_size);
	gs_effect_set_float(p_corner_style_, square ? 0.0f : (float)(look.outline_corners - 1));
	gs_effect_set_float(p_ring_strength_, std::clamp(strength, 0.0f, 1.0f));
	gs_effect_set_float(p_outline_width_, (float)look.outline_width);
	gs_effect_set_float(p_force_follow_, 0.0f);
	design_.set(look.design);
	set_seen(p_rect_tex_, p_area_tex_, p_rect_known_, p_rect_pic_, seen, pw, ph, no_rect_);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	max_blending();
	gs_matrix_push();
	gs_matrix_mul(&m);
	gs_matrix_scale3f((float)(1.0 / bw), (float)(1.0 / bh), 1.0f);
	gs_matrix_translate3f((float)-grow, (float)-grow, 0.0f);
	while (gs_effect_loop(effect_, "BoxRing"))
		gs_draw_sprite(nullptr, 0, (uint32_t)sw, (uint32_t)sh);
	gs_matrix_pop();
	gs_blend_op(GS_BLEND_OP_ADD);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

void LookPaint::draw_follow(gs_texture_t *picture, const matrix4 &m, double pw, double ph, const Seen &seen,
			    bool turned, float amount)
{
	if (!effect_ || !picture || pw < 1.0 || ph < 1.0 || amount <= 0.001f)
		return;
	gs_effect_set_texture(p_image_, picture);
	gs_effect_set_float(p_force_follow_, turned ? 1.0f : 0.0f);
	gs_effect_set_float(p_follow_amount_, std::min(amount, 64.0f));
	set_seen(p_rect_tex_, p_area_tex_, p_rect_known_, p_rect_pic_, seen, pw, ph, no_rect_);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	max_blending();
	gs_matrix_push();
	gs_matrix_mul(&m);
	while (gs_effect_loop(effect_, "FollowGate"))
		gs_draw_sprite(nullptr, 0, 1, 1);
	gs_matrix_pop();
	gs_blend_op(GS_BLEND_OP_ADD);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

bool box_turned(const matrix4 &m)
{
	// (A box line only for a box that is not turned: a turned one keeps a
	// line that follows it.)
	const double ax = std::fabs((double)m.x.x), ay = std::fabs((double)m.x.y);
	const double bx = std::fabs((double)m.y.x), by = std::fabs((double)m.y.y);
	return ay > 1e-3 * std::max(ax, 1e-9) || bx > 1e-3 * std::max(by, 1e-9);
}

void LookPaint::cover_ring(obs_sceneitem_t *item)
{
	matrix4 m;
	uint32_t w = 0, h = 0;
	gs_texture_t *picture = item_picture(item, m, w, h);
	if (!picture || !w || !h)
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ZERO, GS_BLEND_INVSRCALPHA);
	draw_picture(picture, m, w, h);
	gs_blend_state_pop();
}

void LookPaint::draw_item_ring(obs_sceneitem_t *item, const ShadowLook &look)
{
	// The item drawn by itself (its crop and any transition running), so
	// the line goes round what shows of it, not round its box: a box can
	// hold see-through margins.
	matrix4 m;
	uint32_t w = 0, h = 0;
	gs_texture_t *picture = item_picture(item, m, w, h);
	if (!picture || !w || !h)
		return;
	const Seen seen = visible_rect(picture, w, h);
	matrix4 unit, box;
	matrix4_identity(&unit);
	vec4_set(&unit.x, (float)w, 0.0f, 0.0f, 0.0f);
	vec4_set(&unit.y, 0.0f, (float)h, 0.0f, 0.0f);
	matrix4_mul(&box, &unit, &m);
	const bool turned = box_turned(box);
	// (An inner line of a source below lies under this one.)
	if (look.inner_line()) {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ZERO, GS_BLEND_INVSRCALPHA);
		draw_picture(picture, m, w, h);
		gs_blend_state_pop();
	}
	draw_box_ring(box, look, 1.0f, seen, (double)w, (double)h, turned);
	draw_follow(picture, box, (double)w, (double)h, seen, turned, 1.0f);
}

bool LookPaint::compose_layers(gs_texture_t *scene, const std::vector<gs_texture_t *> &casts,
			       const std::vector<gs_texture_t *> &masks, const ShadowLook &look, uint32_t cx,
			       uint32_t cy, gs_texrender_t *into, const std::vector<gs_texture_t *> *rings)
{
	std::vector<Stage> stages;
	for (size_t i = 0; i < casts.size(); i++) {
		if (!casts[i] || i >= masks.size() || !masks[i])
			continue;
		Stage stage;
		stage.cast = casts[i];
		stage.mask = masks[i];
		stage.ring = rings && i < rings->size() ? (*rings)[i] : nullptr;
		stages.push_back(stage);
	}
	return compose_stages(scene, stages, look, cx, cy, into);
}

bool LookPaint::compose_stages(gs_texture_t *scene, const std::vector<Stage> &stages, const ShadowLook &look,
			       uint32_t cx, uint32_t cy, gs_texrender_t *into)
{
	if (!scene || stages.empty())
		return false;
	gs_texture_t *under = scene;
	bool straight = false;
	int turn = 0;
	for (size_t i = 0; i < stages.size(); i++) {
		gs_texrender_t *to = into ? into : out_;
		if (i + 1 != stages.size()) {
			if (!between_[turn])
				between_[turn] = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			to = between_[turn];
			turn ^= 1;
		}
		const Stage &s = stages[i];
		if (!compose(under, s.cast, s.mask, look, cx, cy, to, straight, s.ring, s.presence))
			return false;
		under = gs_texrender_get_texture(to);
		straight = true;
	}
	return true;
}

bool LookPaint::mix(gs_texture_t *a, gs_texture_t *b, double t, uint32_t cx, uint32_t cy)
{
	if (!effect_ || !has_mix_ || !a || !b || !cx || !cy)
		return false;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_texrender_reset(out_);
	bool drawn = false;
	if (gs_texrender_begin(out_, cx, cy)) {
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_effect_set_texture(p_image_, a);
		gs_effect_set_texture(p_shadow_, b);
		gs_effect_set_float(p_ring_strength_, (float)std::clamp(t, 0.0, 1.0));
		while (gs_effect_loop(effect_, "MixSides"))
			gs_draw_sprite(a, 0, cx, cy);
		gs_blend_state_pop();
		gs_texrender_end(out_);
		drawn = true;
	}
	gs_enable_framebuffer_srgb(previous);
	return drawn;
}

void LookPaint::draw_out(uint32_t cx, uint32_t cy)
{
	gs_texture_t *texture = gs_texrender_get_texture(out_);
	if (!texture)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
	const bool linear = gs_get_linear_srgb();
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear);
	if (linear)
		gs_effect_set_texture_srgb(image, texture);
	else
		gs_effect_set_texture(image, texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);
	gs_enable_framebuffer_srgb(previous);
}

} // namespace stagekit
