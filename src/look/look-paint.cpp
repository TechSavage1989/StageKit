/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "look-paint.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>

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
	// A glow sits evenly all round.
	if (s.glow)
		s.offset_x = s.offset_y = 0.0;
	if (s.outline_width < 0.5)
		s.outline = false;
	return s;
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
			x = ShadowLook(y);
			x.opacity = 0.0;
			x.outline = outline;
			x.outline_width = outline_width;
			x.outline_color = outline_color;
			x.outline_opacity = outline_opacity;
		}
		if (!x.outline && y.outline) {
			x.outline = true;
			x.outline_width = y.outline_width;
			x.outline_color = y.outline_color;
			x.outline_opacity = 0.0;
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
	}
	blur_ = std::make_unique<KawaseBlur>();
	out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);
}

LookPaint::~LookPaint()
{
	obs_enter_graphics();
	blur_.reset();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(out_);
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

void LookPaint::copy_onto(gs_texture_t *texture, uint32_t cx, uint32_t cy)
{
	if (!texture)
		return;
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), texture);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(texture, 0, cx, cy);
	gs_blend_state_pop();
}

bool LookPaint::compose(gs_texture_t *scene, gs_texture_t *cast, gs_texture_t *mask, const ShadowLook &look,
			uint32_t cx, uint32_t cy)
{
	if (!effect_ || !scene || !cast || !cx || !cy)
		return false;
	// The passes work on raw colour values, as Shadow, Glow & Outline's do.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_texture_t *softened = cast;
	if (look.shadow && look.softness > 0.0)
		softened = blur_->blur(cast, cx, cy, look.softness);
	gs_texrender_reset(out_);
	bool drawn = false;
	if (gs_texrender_begin(out_, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		vec2 size, offset;
		vec2_set(&size, (float)cx, (float)cy);
		vec2_set(&offset, look.glow ? 0.0f : (float)look.offset_x, look.glow ? 0.0f : (float)look.offset_y);
		vec4 color, line;
		vec4_from_rgba(&color, look.color);
		color.w = look.shadow ? (float)look.opacity : 0.0f;
		vec4_from_rgba(&line, look.outline_color);
		line.w = look.outline ? (float)look.outline_opacity : 0.0f;
		gs_effect_set_texture(p_image_, scene);
		gs_effect_set_texture(p_shadow_, softened);
		gs_effect_set_texture(p_cast_, cast);
		gs_effect_set_texture(p_mask_, mask ? mask : cast);
		gs_effect_set_vec2(p_out_size_, &size);
		gs_effect_set_vec2(p_offset_, &offset);
		gs_effect_set_vec4(p_color_, &color);
		gs_effect_set_float(p_gain_, (float)(1.0 + look.spread * 6.0));
		gs_effect_set_float(p_picture_, look.picture ? 1.0f : 0.0f);
		gs_effect_set_float(p_boost_, (float)look.boost);
		gs_effect_set_float(p_outline_width_, (float)look.outline_width);
		gs_effect_set_vec4(p_outline_color_, &line);
		const bool ring = look.outline && look.outline_width >= 0.5 && look.outline_opacity > 0.001;
		while (gs_effect_loop(effect_, ring ? "Ring" : "MaskedShadow"))
			gs_draw_sprite(scene, 0, cx, cy);
		gs_blend_state_pop();
		gs_texrender_end(out_);
		drawn = true;
	}
	gs_enable_framebuffer_srgb(previous_srgb);
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
