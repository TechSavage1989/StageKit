/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#include <cstdint>

namespace stagekit {

inline constexpr const char *kCornersId = "stagekit_corners";

void register_corners();

// The look's controls and their defaults (style, size, the four corners,
// edges and line, sizes in screen pixels), for Scene Corners, which sets the
// same settings on the Corners & Edges filters it makes.
void corners_add_look_properties(obs_properties_t *props);
void corners_look_defaults(obs_data_t *settings);

// The shape Corners & Edges cuts, in its own pixels (sizes as set, before
// any scaling for how big a source is drawn).
struct CornerShape {
	int style = 0;                  // 0 round, 1 cut, 2 notch, 3 scoop
	double radii[4] = {0, 0, 0, 0}; // top-left, top-right, bottom-right, bottom-left
	double inset = 0.0;
	double soft = 0.0;
	double border = 0.0;
	uint32_t border_color = 0xFFFFFFFF;
	double border_opacity = 1.0; // 0..1
};

// The shape the look settings say (the keys of corners_add_look_properties).
CornerShape read_corner_shape(obs_data_t *settings);

// Part-way from `a` to `b` (t from 0 to 1). A missing side is the other
// side's shape at no size (no corners, edges or line), so the shape grows
// in or shrinks away; the style changes half-way.
CornerShape lerp_corner_shape(const CornerShape *a, const CornerShape *b, double t);

// Draws the shape: what Corners & Edges does to its picture, for anything
// that has a picture to shape (Scene Corners on a whole scene, Morph). On
// the drawing thread.
class CornersPaint {
public:
	CornersPaint();
	~CornersPaint();
	CornersPaint(const CornersPaint &) = delete;
	CornersPaint &operator=(const CornersPaint &) = delete;

	bool ready() const { return effect_ != nullptr; }
	// `input` (w by h) cut to `s` within the part rx, ry, rw by rh, sizes
	// worked out in a space kx, ky times larger. Kept until the next call.
	bool shape(gs_texture_t *input, uint32_t w, uint32_t h, double rx, double ry, double rw, double rh, double kx,
		   double ky, const CornerShape &s);
	// What `shape` made, onto what is being drawn, following OBS's
	// linear-light setting.
	void draw_out(uint32_t w, uint32_t h);

private:
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_size_ = nullptr;
	gs_eparam_t *p_rect_ = nullptr;
	gs_eparam_t *p_radii_ = nullptr;
	gs_eparam_t *p_style_ = nullptr;
	gs_eparam_t *p_soft_ = nullptr;
	gs_eparam_t *p_border_ = nullptr;
	gs_eparam_t *p_border_color_ = nullptr;
	gs_texrender_t *out_ = nullptr;
	bool drawn_ = false;
};

} // namespace stagekit
