/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <cmath>

namespace stagekit {

// The Mosaic technique of stagekit-looks.effect, as every user of it sets it:
// the Blur filter and Scene Blur (through BlurPipeline) and Ambient
// Background. OBS loads an effect file once and shares that copy between all
// who load it, and Direct3D draws nothing with a uniform that was never set
// ("Not all shader parameters were set"; OpenGL takes zeros). So every
// uniform the mosaic reads is set here, at every run, by every user: a new
// one goes in MosaicLook and in set() below, never in one caller only. Up to
// beta170 Ambient Background set none of `whole`, `open_edge` and
// `fallback`, and on Windows its mosaic drew nothing (a black background)
// until a blur had set them.
struct MosaicLook {
	double width = 1.0, height = 1.0; // the picture, in pixels
	double cell = 1.0;                // the cell size, in pixels
	int shape = 0;                    // the shader's numbers: 0 square, 1 hexagon ...
	double smoothing = 0.0;           // pixels
	double glass = 0.0;               // 0..1
	int glass_style = 0;
	double origin_x = 0.0, origin_y = 0.0; // pixels
	double rotation_deg = 0.0;
	bool whole_cells = false;                          // a cell the edge cuts is left out
	bool open_edges[4] = {false, false, false, false}; // left, top, right, bottom: no edge there
	bool kept_to_mask = false;                         // the picture's alpha is a mask
};

class MosaicUniforms {
public:
	void find(gs_effect_t *effect)
	{
		texel_ = param(effect, "texel");
		size_ = param(effect, "size");
		strength_ = param(effect, "strength");
		shape_ = param(effect, "shape");
		smoothing_ = param(effect, "smoothing");
		glass_ = param(effect, "glass");
		glass_style_ = param(effect, "glass_style");
		origin_ = param(effect, "origin");
		turn_ = param(effect, "turn");
		whole_ = param(effect, "whole");
		open_edge_ = param(effect, "open_edge");
		fallback_ = param(effect, "fallback");
	}

	void set(const MosaicLook &m) const
	{
		vec2 size, texel, origin, turn;
		vec2_set(&size, (float)m.width, (float)m.height);
		vec2_set(&texel, (float)(1.0 / m.width), (float)(1.0 / m.height));
		const double rad = m.rotation_deg * 3.14159265358979323846 / 180.0;
		vec2_set(&origin, (float)m.origin_x, (float)m.origin_y);
		vec2_set(&turn, (float)std::cos(rad), (float)std::sin(rad));
		vec4 open;
		vec4_set(&open, m.open_edges[0] ? 1.0f : 0.0f, m.open_edges[1] ? 1.0f : 0.0f,
			 m.open_edges[2] ? 1.0f : 0.0f, m.open_edges[3] ? 1.0f : 0.0f);
		set2(texel_, texel);
		set2(size_, size);
		set1(strength_, m.cell);
		set1(shape_, m.shape);
		set1(smoothing_, m.smoothing);
		set1(glass_, m.glass);
		set1(glass_style_, m.glass_style);
		set2(origin_, origin);
		set2(turn_, turn);
		set1(whole_, m.whole_cells ? 1.0 : 0.0);
		if (open_edge_)
			gs_effect_set_vec4(open_edge_, &open);
		set1(fallback_, m.kept_to_mask ? 1.0 : 0.0);
	}

private:
	static gs_eparam_t *param(gs_effect_t *effect, const char *name)
	{
		return effect ? gs_effect_get_param_by_name(effect, name) : nullptr;
	}
	static void set1(gs_eparam_t *p, double v)
	{
		if (p)
			gs_effect_set_float(p, (float)v);
	}
	static void set2(gs_eparam_t *p, const vec2 &v)
	{
		if (p)
			gs_effect_set_vec2(p, &v);
	}

	gs_eparam_t *texel_ = nullptr, *size_ = nullptr, *strength_ = nullptr, *shape_ = nullptr;
	gs_eparam_t *smoothing_ = nullptr, *glass_ = nullptr, *glass_style_ = nullptr, *origin_ = nullptr;
	gs_eparam_t *turn_ = nullptr, *whole_ = nullptr, *open_edge_ = nullptr, *fallback_ = nullptr;
};

} // namespace stagekit
