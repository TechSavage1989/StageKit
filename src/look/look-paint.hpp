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
#include "../common/pattern-frame.hpp"

#include <obs.h>
#include <graphics/matrix4.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace stagekit {

// How the line is drawn (`outline_design`, by number, append-only): 0 solid,
// as it always was; then the still designs and the ones that move. Sizes in
// the pixels of whoever draws (canvas pixels for Scene Shadow).
struct OutlineDesign {
	enum Kind {
		Solid = 0,
		Double = 1,
		Dashed = 2,
		Dotted = 3,
		Gradient = 4,
		Neon = 5,
		Inner = 6,
		Marching = 7,
		Travelling = 8,
		Pulse = 9,
		Shimmer = 10,
		Rainbow = 11,
		// Filled with one of Ambient Background's patterns (outline-pattern.*):
		// the line is a window onto the pattern.
		Aurora = 12,
		Plasma = 13,
		Lava = 14,
		Waves = 15,
		Nebula = 16,
		Flow = 17,
		Stars = 18,
		Bokeh = 19,
		Rays = 20,
		Circuit = 21,
		Hex = 22,
		Rain = 23,
		Tunnel = 24,
		Ripples = 25,
		Embers = 26,
		CircuitTraces = 27,
		DataLines = 28,
		// (Hex columns is a background only: a view, not a texture.)
		Mesh = 29,
		Halftone = 30,
		DotGrid = 31,
		HexWire = 32,
		HexMosaic = 33,
		kLast = HexMosaic,
	};
	int kind = Solid;
	uint32_t color2 = 0xFFFFAA00; // the gradient's other end, the shimmer's highlight
	double dash = 24.0, gap = 16.0;
	double speed = 60.0; // pixels a second for dashes; 600 is one round a second; 0 stands still
	double glow = 1.5;   // Neon: how far its halo reaches past the line, in line widths
	// The pattern fills: their third colour (the outline colour and `color2`
	// are the first two) and how large the pattern is, 1 as designed.
	uint32_t color3 = 0xFFA046C8;
	double pattern_scale = 1.0;
	// Position X, Position Y and Rotation. A pattern fill: where the pattern
	// lies in the line's picture. Dashes and the colours that go round the
	// border (`turns()`): the rotation alone, as how far round they begin.
	PatternPose pose;
	bool turns() const
	{
		return kind == Dashed || kind == Marching || kind == Gradient || kind == Travelling ||
		       kind == Shimmer || kind == Rainbow;
	}
	bool pattern() const { return kind >= Aurora && kind <= kLast; }
	// Drawn inside the source's edge, with no line outside it.
	bool inner() const { return kind == Inner; }
	// Neon: how far the halo of light reaches past a line `width` wide.
	double halo_reach(double width) const { return kind == Neon ? width * glow : 0.0; }
};

// Neon's halo: the blur's strength for a reach of one pixel (measured: a
// KawaseBlur's picture is gone some two thirds of its strength from an edge).
constexpr double kHaloBlur = 1.4;

// The design as the settings say, and part-way between two (the kind changes
// half-way).
OutlineDesign read_outline_design(obs_data_t *settings);
OutlineDesign lerp_design(const OutlineDesign &a, const OutlineDesign &b, double t);

// The design's values in an effect (the same names in stagekit-shadow.effect
// and stagekit-scene-shadow.effect), set at every run of a technique that
// draws a line. What moves goes by OBS's video frame time, wrapped here, so
// a shader never sees a large number.
class DesignUniforms {
public:
	void find(gs_effect_t *effect);
	// `pattern`: the picture a pattern fill's line takes its colour from
	// (outline_pattern_texture), or null for the design's own colours;
	// `stand_in`: any texture, given in its place then, because a value an
	// effect reads must be set at every run. Without `stand_in` the two
	// are left alone (a technique that draws no colour).
	void set(const OutlineDesign &design, gs_texture_t *pattern = nullptr, gs_texture_t *stand_in = nullptr) const;
	// Neon's halo, for the technique that lays it (Ring; Compose): `halo`,
	// the line's shape softened, or null for none; `stand_in`, any texture,
	// given in its place then. At every run of that technique.
	void set_halo(const OutlineDesign &design, uint32_t outline_color, double opacity, gs_texture_t *halo,
		      gs_texture_t *stand_in) const;

private:
	gs_eparam_t *kind_ = nullptr, *color2_ = nullptr, *dash_ = nullptr, *gap_ = nullptr;
	gs_eparam_t *phase_ = nullptr, *turn_ = nullptr, *pulse_ = nullptr;
	gs_eparam_t *halo_tex_ = nullptr, *halo_color_ = nullptr;
	gs_eparam_t *pattern_tex_ = nullptr, *pattern_on_ = nullptr;
};

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
	// The line's corners (Shadow, Glow & Outline's `outline_corners`, by
	// number, append-only): 0 round the source's own shape, as it always
	// was; 1 rounded, 2 cut, 3 notched, 4 scooped, 5 square, as a shape round the
	// source's box, its outer corners `outline_corner_size` across.
	int outline_corners = 0;
	double outline_corner_size = 20.0;
	OutlineDesign design;
	// How dense the shadow or glow is (`glow_intensity` / 100): 1 as it
	// always was; more fills its soft tail in without a hard edge.
	double intensity = 1.0;
	bool box_line() const { return outline && outline_corners > 0; }
	// An inner line of a source lies on the source, so whatever is stacked
	// above it must cover it (LookPaint::cover_ring).
	bool inner_line() const { return box_line() && design.inner(); }

	// Whether anything is drawn at all.
	bool any() const { return (shadow && opacity > 0.001) || (outline && outline_opacity > 0.001); }
};

// The look as the settings say (the ids of Shadow, Glow & Outline).
ShadowLook read_shadow_look(obs_data_t *settings);

// The controls Shadow, Glow & Outline and Scene Shadow share for the line's
// corners (into the line's group) and the shadow's intensity (into the
// shadow's group), and their defaults.
void add_outline_corner_properties(obs_properties_t *outline);
void add_outline_design_properties(obs_properties_t *outline);
void add_intensity_property(obs_properties_t *shadow);
void outline_glow_defaults(obs_data_t *settings);

// Both opacities times `level` (a switch fading the look in or out).
ShadowLook scaled_look(ShadowLook look, double level);

// The look part-way from `a` to `b` (t from 0 to 1). A side that is missing,
// or has a part switched off, is the other side's with that part's opacity
// at 0, so the shadow fades in or out where it is.
ShadowLook lerp_look(const ShadowLook *a, const ShadowLook *b, double t);

// Whether an item's show (or hide) transition is running now: OBS then draws
// the transition in the item's place, and only drawing it ends it.
bool item_transition_running(obs_sceneitem_t *item, bool show);

// How far a source's shadow, glow or outline can reach past it, in canvas
// pixels (generously: the softened edge, the offset and the line).
double shadow_reach(const ShadowLook &look);

// A source's place on the canvas, for shadow_layers.
struct LayerBox {
	bool present = false; // in the stack: drawn and not background
	double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Each source by itself: the sources (bottom first) in runs whose shadows
// cannot reach each other, so each run can be drawn as one layer: a source
// starts a new run where its box, grown by `reach`, meets the grown box of
// one already in the run. The first index of each run; at most `most` runs
// (the last takes the rest). Empty when no source is present.
std::vector<size_t> shadow_layers(const std::vector<LayerBox> &boxes, double reach, size_t most);

// Where an item is drawn on its scene's canvas now (its box under the draw
// transform, crop left out), on the drawing thread. False when it draws
// nothing (hidden with no transition running, no size).
bool item_drawn_box(obs_sceneitem_t *item, LayerBox &out);

struct ItemDraw;

// Whether a box (the unit square under `m`) is turned or slanted on the canvas.
bool box_turned(const matrix4 &m);

// What shows of a picture, worked out on the graphics card (nothing read
// back): `rect`, a one-pixel 32-bit float picture holding x0, y0, x1, y1 in
// the picture's pixels (empty: x1 < x0), and `area`, one holding the sum of
// its alpha in red (how much of that rectangle it fills).
struct Seen {
	gs_texture_t *rect = nullptr;
	gs_texture_t *area = nullptr;
	bool known() const { return rect && area; }
};

// Works Seen out. On the drawing thread; each result is kept until the next
// begin_frame.
class VisibleRect {
public:
	VisibleRect();
	~VisibleRect();
	VisibleRect(const VisibleRect &) = delete;
	VisibleRect &operator=(const VisibleRect &) = delete;
	void begin_frame() { next_ = 0; }
	Seen measure(gs_texture_t *picture, uint32_t w, uint32_t h);
	// The most alpha anywhere in a picture, in the red of a one-pixel
	// picture (nullptr when it cannot be worked out).
	gs_texture_t *top(gs_texture_t *picture, uint32_t w, uint32_t h);

private:
	gs_texture_t *reduce(gs_texture_t *picture, uint32_t w, uint32_t h, const char *first, const char *rest);
	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_in_size_ = nullptr;
	bool has_top_ = false;
	std::vector<std::vector<gs_texrender_t *>> sets_; // one chain of levels per reduction this frame
	size_t next_ = 0;
};

class LookPaint {
public:
	LookPaint();
	~LookPaint();
	LookPaint(const LookPaint &) = delete;
	LookPaint &operator=(const LookPaint &) = delete;

	bool ready() const { return effect_ != nullptr; }

	// At the start of each drawing: the pictures borrowed for cropped items
	// are free again.
	void begin_frame()
	{
		spare_ = 0;
		if (rects_)
			rects_->begin_frame();
	}

	// Draws an item as its scene draws it, onto what is being drawn: its
	// place, size and crop, and a show or hide transition while one runs
	// (a hidden item with none draws nothing). Not followed: a crop to the
	// bounding box, the scale filter and the blending mode, which only
	// change a silhouette's edge.
	void draw_item(obs_sceneitem_t *item);
	// The same item drawn into a picture of its own, the size of the part
	// of its source that shows (`w` by `h`), and `m`, the transform that
	// puts that picture where the scene draws it. nullptr when it draws
	// nothing. The picture is borrowed until the next begin_frame.
	gs_texture_t *item_picture(obs_sceneitem_t *item, matrix4 &m, uint32_t &w, uint32_t &h);
	// A picture of an item made some other way (`texture`, w by h, straight
	// alpha, as item_picture's matrix places it: Scene Blur's source blurred
	// by itself) drawn where `m` puts it, with the blending set now.
	void draw_picture(gs_texture_t *texture, const matrix4 &m, uint32_t w, uint32_t h);

	// A picture of the canvas (cx by cy), cleared, ready to draw items into
	// with the blending OBS fills a filter's picture with. `end_canvas`
	// gives it back.
	bool begin_canvas(gs_texrender_t *into, uint32_t cx, uint32_t cy);
	void end_canvas(gs_texrender_t *into);
	// A whole picture laid onto the canvas being drawn, replacing what is
	// there (to start a mask from the silhouette), or, with `over`, laid over
	// it as a premultiplied picture (a layer's mask under the masks above it:
	// the shader reads the mask's colours as the sources' part of a pixel, so
	// what is on top must be on top there too).
	void copy_onto(gs_texture_t *texture, uint32_t cx, uint32_t cy, bool over = false);

	// The shadow and outline of `cast` laid over `scene`, off `mask`
	// (nullptr: the mask is `cast` itself). Kept in a picture of its own
	// until the next call; false when nothing could be drawn.
	// (`into`: a picture of the caller's instead, left as it is for
	// draw_out.)
	// (`scene_straight`: `scene` holds straight alpha, as compose's own
	// pictures do, instead of OBS's premultiplied scene.)
	// (`ring`: with a line round the boxes (`box_line`), the lines as drawn
	// by draw_box_ring; nullptr: round the sources' own shape.)
	// (`presence`: the mask with the casting sources whole where they
	// stand, however far they have faded in or out (alpha; draw_presence),
	// so a fading source's line and shadow are never laid under the source
	// itself. Only for a cast of fading sources by themselves: a line is
	// kept off the whole of it. nullptr: none is fading, the mask says it.)
	bool compose(gs_texture_t *scene, gs_texture_t *cast, gs_texture_t *mask, const ShadowLook &look, uint32_t cx,
		     uint32_t cy, gs_texrender_t *into = nullptr, bool scene_straight = false,
		     gs_texture_t *ring = nullptr, gs_texture_t *presence = nullptr);
	// Each source by itself: one shadow per layer of sources, from the
	// bottom, each laid over the picture the one below it made. `casts[i]`
	// is layer i's casting sources (nullptr: it casts nothing) and
	// `masks[i]` every source of layer i and above, which its shadow is kept
	// off. Where to (`into`) and false as compose.
	bool compose_layers(gs_texture_t *scene, const std::vector<gs_texture_t *> &casts,
			    const std::vector<gs_texture_t *> &masks, const ShadowLook &look, uint32_t cx, uint32_t cy,
			    gs_texrender_t *into = nullptr, const std::vector<gs_texture_t *> *rings = nullptr);
	// The same as a list of compose's own arguments, each laid over the
	// picture the one before made (`mask` nullptr: the cast).
	struct Stage {
		gs_texture_t *cast = nullptr, *mask = nullptr, *ring = nullptr, *presence = nullptr;
	};
	bool compose_stages(gs_texture_t *scene, const std::vector<Stage> &stages, const ShadowLook &look, uint32_t cx,
			    uint32_t cy, gs_texrender_t *into = nullptr);
	// Two pictures compose made (`into`), the second `t` of the way over the
	// first in linear light, for draw_out. False when it could not be done.
	bool mix(gs_texture_t *a, gs_texture_t *b, double t, uint32_t cx, uint32_t cy);
	// Whether an item is, or may be, drawn part-way see-through as a whole:
	// its show or hide transition is running, or it is the item Morph's
	// layer. Such an item goes into a presence canvas with draw_presence.
	static bool may_fade(obs_sceneitem_t *item);
	// An item into a presence canvas (begin_canvas): where it is drawn now,
	// whole however far it has faded (its alpha over the most alpha in it).
	void draw_presence(obs_sceneitem_t *item);
	// A line round a box, with the look's corners (`box_line`), onto the
	// canvas being drawn (begin_canvas): the box is the unit square under
	// `m` (canvas pixels), counted `strength` of the way (a Morph part-way).
	// `seen` (VisibleRect's, for a picture pw by ph laid on that square):
	// the line goes round the part that shows instead of the whole box.
	// The canvas holds the line in alpha and, in red, the sources outlined
	// as Follow the source instead (`draw_follow`); both are drawn taking
	// the larger value, so any number of sources share it.
	void draw_box_ring(const matrix4 &m, const ShadowLook &look, float strength, const Seen &seen = Seen(),
			   double pw = 0.0, double ph = 0.0, bool turned = false);
	// The other half: `picture` (pw by ph, laid on the unit square under
	// `m`) into the canvas's red, where it fills under nine tenths of the
	// rectangle that shows of it (text, a logo, a group with gaps: those
	// keep a line that follows them), or always when `turned` (a turned
	// box gets no box line). `amount` scales it (a Morph part-way).
	void draw_follow(gs_texture_t *picture, const matrix4 &m, double pw, double ph, const Seen &seen, bool turned,
			 float amount);
	// What shows of a picture (VisibleRect), with LookPaint's own.
	Seen visible_rect(gs_texture_t *picture, uint32_t w, uint32_t h);
	// Both halves for an item as its scene draws it now (nothing for an
	// item that draws nothing).
	void draw_item_ring(obs_sceneitem_t *item, const ShadowLook &look);
	// An item stacked above the ones whose lines are in the ring canvas
	// being drawn: what it covers of them is taken out, so an inner line
	// (which lies on its source) does not show through a source above it.
	void cover_ring(obs_sceneitem_t *item);
	// What `compose` made, onto what is being drawn, following OBS's
	// linear-light setting.
	void draw_out(uint32_t cx, uint32_t cy);

private:
	gs_texrender_t *spare();
	gs_texture_t *picture_of(const ItemDraw &d);

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
	gs_eparam_t *p_straight_ = nullptr;
	gs_eparam_t *p_ring_shape_ = nullptr;
	gs_eparam_t *p_ring_box_ = nullptr;
	gs_eparam_t *p_intensity_ = nullptr;
	gs_eparam_t *p_box_size_ = nullptr;
	gs_eparam_t *p_box_rect_ = nullptr;
	gs_eparam_t *p_corner_size_ = nullptr;
	gs_eparam_t *p_corner_style_ = nullptr;
	gs_eparam_t *p_ring_strength_ = nullptr;
	gs_eparam_t *p_rect_tex_ = nullptr;
	gs_eparam_t *p_rect_known_ = nullptr;
	gs_eparam_t *p_rect_pic_ = nullptr;
	gs_eparam_t *p_area_tex_ = nullptr;
	gs_eparam_t *p_force_follow_ = nullptr;
	gs_eparam_t *p_follow_amount_ = nullptr;
	gs_eparam_t *p_follow_tex_ = nullptr;
	gs_eparam_t *p_presence_ = nullptr;
	DesignUniforms design_;
	bool has_presence_ = false;
	bool has_mix_ = false;
	std::unique_ptr<VisibleRect> rects_;
	gs_texture_t *no_rect_ = nullptr;
	gs_texrender_t *outlined_ = nullptr; // the casting sources with their line, which casts the shadow
	std::unique_ptr<KawaseBlur> blur_;
	std::unique_ptr<KawaseBlur> halo_blur_; // Neon's halo, made when first wanted
	gs_texrender_t *out_ = nullptr;
	gs_texrender_t *between_[2] = {nullptr, nullptr}; // compose_layers' pictures on the way
	std::vector<gs_texrender_t *> spares_;
	size_t spare_ = 0;
};

} // namespace stagekit
