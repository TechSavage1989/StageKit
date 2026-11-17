/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// What Scene Shadow draws with, shared with Morph so that a change of scene
// carries the shadow along: the look read from settings, a scene's items
// drawn again as silhouettes, and the pass that lays the shadow (and an
// outline) over a finished picture, kept off every source that is not
// background.

#pragma once

#include "../common/kawase-blur.hpp"

#include <obs.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace stagekit {

// A shadow, glow or outline, with the same settings (names and numbers) as
// Shadow, Glow & Outline, all in canvas pixels.
struct ShadowLook {
	bool shadow = true;
	bool glow = false;    // all round instead of to one side
	bool picture = false; // the sources' own colours instead of `color`
	uint32_t color = 0xFF000000;
	double opacity = 0.6; // 0..1
	double offset_x = 12.0, offset_y = 12.0;
	double softness = 20.0;
	double spread = 0.0; // 0..1
	double boost = 0.3;  // 0..1
	bool outline = false;
	double outline_width = 6.0;
	uint32_t outline_color = 0xFFFFFFFF;
	double outline_opacity = 1.0; // 0..1

	// Whether anything is drawn at all.
	bool any() const { return (shadow && opacity > 0.001) || (outline && outline_opacity > 0.001); }
};

// The look as the settings say (the ids of Shadow, Glow & Outline).
ShadowLook read_shadow_look(obs_data_t *settings);

// Both opacities times `level` (a switch fading the look in or out).
ShadowLook scaled_look(ShadowLook look, double level);

// The look part-way from `a` to `b` (t from 0 to 1). A side that is missing,
// or has a part switched off, is the other side's with that part's opacity
// at 0, so the shadow fades in or out where it is.
ShadowLook lerp_look(const ShadowLook *a, const ShadowLook *b, double t);

// Whether an item's show (or hide) transition is running now: OBS then draws
// the transition in the item's place, and only drawing it ends it.
bool item_transition_running(obs_sceneitem_t *item, bool show);

class LookPaint {
public:
	LookPaint();
	~LookPaint();
	LookPaint(const LookPaint &) = delete;
	LookPaint &operator=(const LookPaint &) = delete;

	bool ready() const { return effect_ != nullptr; }

	// At the start of each drawing: the pictures borrowed for cropped items
	// are free again.
	void begin_frame() { spare_ = 0; }

	// Draws an item as its scene draws it, onto what is being drawn: its
	// place, size and crop, and a show or hide transition while one runs
	// (a hidden item with none draws nothing). Not followed: a crop to the
	// bounding box, the scale filter and the blending mode, which only
	// change a silhouette's edge.
	void draw_item(obs_sceneitem_t *item);

	// A picture of the canvas (cx by cy), cleared, ready to draw items into
	// with the blending OBS fills a filter's picture with. `end_canvas`
	// gives it back.
	bool begin_canvas(gs_texrender_t *into, uint32_t cx, uint32_t cy);
	void end_canvas(gs_texrender_t *into);
	// A whole picture laid onto the canvas being drawn, replacing what is
	// there (to start a mask from the silhouette).
	void copy_onto(gs_texture_t *texture, uint32_t cx, uint32_t cy);

	// The shadow and outline of `cast` laid over `scene`, off `mask`
	// (nullptr: the mask is `cast` itself). Kept in a picture of its own
	// until the next call; false when nothing could be drawn.
	bool compose(gs_texture_t *scene, gs_texture_t *cast, gs_texture_t *mask, const ShadowLook &look, uint32_t cx,
		     uint32_t cy);
	// What `compose` made, onto what is being drawn, following OBS's
	// linear-light setting.
	void draw_out(uint32_t cx, uint32_t cy);

private:
	gs_texrender_t *spare();

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_shadow_ = nullptr;
	gs_eparam_t *p_cast_ = nullptr;
	gs_eparam_t *p_mask_ = nullptr;
	gs_eparam_t *p_out_size_ = nullptr;
	gs_eparam_t *p_offset_ = nullptr;
	gs_eparam_t *p_color_ = nullptr;
	gs_eparam_t *p_gain_ = nullptr;
	gs_eparam_t *p_picture_ = nullptr;
	gs_eparam_t *p_boost_ = nullptr;
	gs_eparam_t *p_outline_width_ = nullptr;
	gs_eparam_t *p_outline_color_ = nullptr;
	std::unique_ptr<KawaseBlur> blur_;
	gs_texrender_t *out_ = nullptr;
	std::vector<gs_texrender_t *> spares_;
	size_t spare_ = 0;
};

} // namespace stagekit
