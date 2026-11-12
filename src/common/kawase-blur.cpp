/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "kawase-blur.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>

namespace stagekit {

KawaseBlur::KawaseBlur()
{
	char *path = obs_module_file("effects/stagekit-blur.effect");
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "failed to load the blur effect: %s", errors ? errors : "not found");
	bfree(errors);
	bfree(path);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_texel_ = gs_effect_get_param_by_name(effect_, "texel");
		p_offset_ = gs_effect_get_param_by_name(effect_, "offset");
	}
	out_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (auto &tr : down_)
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (auto &tr : up_)
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
}

KawaseBlur::~KawaseBlur()
{
	gs_effect_destroy(effect_);
	gs_texrender_destroy(out_);
	for (auto *tr : down_)
		gs_texrender_destroy(tr);
	for (auto *tr : up_)
		gs_texrender_destroy(tr);
}

void KawaseBlur::pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_effect_set_texture(p_image_, src);
	while (gs_effect_loop(effect_, technique))
		gs_draw_sprite(src, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(dst);
}

gs_texture_t *KawaseBlur::blur(gs_texture_t *input, uint32_t w, uint32_t h, double strength)
{
	const double scaled = strength * 0.14;
	if (!effect_ || !input || scaled < 0.05)
		return input;

	int levels = std::clamp((int)std::floor(std::log2(std::max(scaled, 1.0))) + 1, 1, kLevels);
	while (levels > 1 && ((w >> levels) < 2 || (h >> levels) < 2))
		levels--;
	const float offset = (float)(scaled / std::pow(2.0, levels - 1));

	std::array<uint32_t, kLevels + 1> lw{}, lh{};
	lw[0] = w;
	lh[0] = h;
	for (int i = 1; i <= levels; i++) {
		lw[i] = std::max(1u, lw[i - 1] / 2);
		lh[i] = std::max(1u, lh[i - 1] / 2);
	}

	gs_texture_t *cur = input;
	for (int i = 0; i < levels; i++) {
		vec2 texel;
		vec2_set(&texel, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &texel);
		gs_effect_set_float(p_offset_, offset);
		pass(down_[i], cur, lw[i + 1], lh[i + 1], "KawaseDown");
		cur = gs_texrender_get_texture(down_[i]);
	}
	for (int i = levels; i >= 1; i--) {
		vec2 texel;
		vec2_set(&texel, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &texel);
		gs_effect_set_float(p_offset_, offset);
		gs_texrender_t *dst = i == 1 ? out_ : up_[i - 1];
		pass(dst, cur, lw[i - 1], lh[i - 1], "KawaseUp");
		cur = gs_texrender_get_texture(dst);
	}
	return cur;
}

} // namespace stagekit
