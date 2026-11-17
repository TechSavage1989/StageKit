/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// What the Blur filter draws with, shared with Scene Blur: the look read from
// settings (the same names and numbers in both), its controls, and the passes
// that blur a picture and lay the blur over it. One copy of the blur, so the
// two filters always look the same.

#pragma once

#include "../common/easing.hpp"

#include <obs.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace stagekit {

// Kept by number in settings: new ones go at the end.
enum class BlurType : int {
	Gaussian = 0,
	Box = 1,
	Kawase = 2,
	Pixelate = 3,
	Zoom = 4,
	Motion = 5,
	Hexagon = 6, // before the mosaic had shapes: now Pixelate with that shape
	Temporal = 7,
};

// The shape of a mosaic's cells. Saved by number: new ones go at the end.
// The shader goes by the same numbers.
enum class PixelShape : int {
	Square = 0,
	Hexagon = 1,
	Circle = 2,
	Triangle = 3,
	Voronoi = 4,
	Rhomboid = 5,
	Triakis = 6,
};

enum class RampStyle : int {
	Grow = 0, // blur strength grows from 0 to the configured strength
	Fade = 1, // full-strength blur cross-fades over the original
};

// The slider's top. It was 250; the owner asked for more.
inline constexpr double kMaxBlurStrength = 1000.0;

// How the blur looks.
struct BlurLook {
	BlurType type = BlurType::Gaussian;
	double strength = 30.0;
	double angle_deg = 0.0;
	double center_x = 0.5;
	double center_y = 0.5;
	PixelShape pixel_shape = PixelShape::Square;
	double pixel_smooth = 0.0; // 0..1 of a fifth of a cell
	double pixel_glass = 0.0;  // 0..1: each cell lit like a piece of glass
	int pixel_glass_style = 0; // 0 rounded, 1 edges; kept by number
	double origin_x = 0.5;     // where the mosaic is laid out from, 0..1 of the picture
	double origin_y = 0.5;
	double rotation_deg = 0.0;
	// "Whole cells" (Mosaic edge, `pixel_edge` 1): a cell the picture's edge
	// cuts is left out, so the outline is made of whole cells and the
	// image's alpha follows it. Never drawn past the edge.
	bool whole_cells = false;
	// Not a setting: the sides of the picture (left, top, right, bottom)
	// that are no edge for whole cells, because the screen's edge cuts the
	// source there anyway (Scene Blur, each source by itself).
	std::array<bool, 4> open_edges{};
};

// How a mosaic's edge is drawn (`pixel_edge`). Kept by number: new ones go
// at the end.
enum class MosaicEdge : int {
	Cut = 0,   // cells cut straight at the picture's edge, as before
	Whole = 1, // whole cells only
};

// A trail (Temporal) of its own, for a picture blurred apart from the rest
// (Scene Blur's "Each source by itself"). It moves on once per video frame
// by the frame's own time, however often it is drawn in that frame. Its
// pictures are made the first time and freed by forget_trail_pictures (in
// the graphics context).
struct BlurTrail {
	gs_texrender_t *past[2] = {nullptr, nullptr};
	int index = 0;
	bool valid = false;
	uint32_t w = 0, h = 0;
	uint64_t frame = 0; // the video frame it last moved on in
};
void forget_trail_pictures(BlurTrail &trail);

// How it comes and goes.
struct BlurRamp {
	RampStyle style = RampStyle::Grow;
	double in_s = 0.4;
	double out_s = 0.4;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

// The look as the settings say. An old "Hexagons" type is rewritten in the
// settings as Pixelate with hexagon cells.
BlurLook read_blur_look(obs_data_t *settings);
BlurRamp read_blur_ramp(obs_data_t *settings);

// The controls, as the Blur filter has them: the "Blur" group (type,
// strength and each type's own settings) and the "Timing" group (blur
// transition, blur-in and blur-out duration, easing).
void add_blur_look_properties(obs_properties_t *props);
void add_blur_ramp_properties(obs_properties_t *props);
// Their defaults.
void blur_look_defaults(obs_data_t *settings);
// Shows each look control for the blur types it is for. A window with a
// "blur_area" list (Scene Blur) shows the mosaic's edge only for "Each
// source by itself", where there is a source's edge to keep to.
void blur_look_visibility(obs_properties_t *props, obs_data_t *settings);

// The strength to blur at and how much of the blur to lay over, for a
// switch at `level` (0..1): a Grow ramp grows the strength, a Fade fades the
// blur in at full strength.
void blur_at_level(const BlurLook &look, RampStyle style, double level, double &strength, double &mix);

// "gaussian", "pixelate", "hexagon" ...: a look's name for keys and requests.
const char *blur_type_name(BlurType type, PixelShape shape);
// The look's entry in the Stream Deck keys' Blur style list (streamdeck/src/
// key-types.json): "gaussian", "trail", "pixelate", "hexagons",
// "mosaic_circle" ... and "glass" for a mosaic with glass.
const char *blur_style_name(const BlurLook &look);

// The blur passes. Make, use and destroy on any thread; the graphics calls
// enter the graphics context themselves where they need to.
class BlurPipeline {
public:
	BlurPipeline();
	~BlurPipeline();
	BlurPipeline(const BlurPipeline &) = delete;
	BlurPipeline &operator=(const BlurPipeline &) = delete;

	// Whether the blur's shader loaded (the newer looks may still be missing).
	bool ready() const { return effect_ != nullptr; }
	bool has_looks() const { return looks_ != nullptr; }

	struct Result {
		gs_texture_t *a = nullptr;
		gs_texture_t *b = nullptr; // optional second result, cross-faded in by b_mix
		double b_mix = 0.0;
	};

	// Blurs `input` (w by h, straight alpha) at `strength`. `mix` may be
	// lowered (the first, smallest Kawase step fades in). On the drawing
	// thread.
	// `trail`: a trail of the caller's own for Temporal (nullptr: the
	// pipeline's, moved on by frame_passed).
	// `kept_to_mask`: `input`'s alpha is kept to a mask (keep_to_mask), so a
	// mosaic cell that finds nothing near its centre looks further out in
	// it, never outside it.
	Result run(const BlurLook &look, gs_texture_t *input, uint32_t w, uint32_t h, double strength, double &mix,
		   BlurTrail *trail = nullptr, bool kept_to_mask = false);
	// `picture` with its alpha times `mask` (red), into `into` (w by h): a
	// blur of it averages only what lies where the mask is.
	gs_texture_t *keep_to_mask(gs_texrender_t *into, gs_texture_t *picture, gs_texture_t *mask, uint32_t w,
				   uint32_t h);
	// `input` blurred and laid over itself by `mix`, as the Blur filter
	// draws it, into `into` (w by h, straight alpha). Its picture, or
	// nullptr.
	gs_texture_t *blur_into(gs_texrender_t *into, const BlurLook &look, gs_texture_t *input, uint32_t w, uint32_t h,
				double strength, double mix, BlurTrail *trail = nullptr);
	// The blur laid over `input` by `mix`, onto what is being drawn.
	void composite(gs_texture_t *input, const Result &blurred, uint32_t w, uint32_t h, double mix);
	// The blur laid over `original` by `mix` only where `mask` (red) says,
	// onto what is being drawn. False without the newer shader file.
	// `keep_alpha`: the blur gives colour only, the alpha is `original`'s
	// (a blur of a picture kept to the mask).
	bool composite_masked(gs_texture_t *original, const Result &blurred, gs_texture_t *mask, uint32_t w, uint32_t h,
			      double mix, bool keep_alpha = false);
	// One piece of a mask: `texture` (w by h) drawn where the matrix puts
	// it, its coverage counted (blurred) or held clear. The caller sets the
	// blending (see MaskPiece in stagekit-looks.effect).
	void draw_mask_piece(gs_texture_t *texture, uint32_t w, uint32_t h, bool counted);

	// The trail (Temporal) moves on once per frame: call from the tick.
	void frame_passed(float seconds);
	// The trail starts afresh next time (the blur was off).
	void forget_trail() { past_valid_ = false; }

private:
	static constexpr int kMaxKawaseLevels = 8;
	using LevelTargets = std::array<gs_texrender_t *, kMaxKawaseLevels>;

	// (`looks`: with the newer shader file instead of the blur's own.)
	void pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique,
		  bool looks = false);
	Result own_trail(BlurTrail &trail, gs_texture_t *input, uint32_t w, uint32_t h, double strength);
	gs_texture_t *kawase_chain(gs_texture_t *input, uint32_t w, uint32_t h, int levels, float offset,
				   LevelTargets &down, LevelTargets &up, gs_texrender_t *out);

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_image2_ = nullptr;
	gs_eparam_t *p_mix2_ = nullptr;
	gs_eparam_t *p_original_ = nullptr;
	gs_eparam_t *p_texel_ = nullptr;
	gs_eparam_t *p_size_ = nullptr;
	gs_eparam_t *p_direction_ = nullptr;
	gs_eparam_t *p_radius_ = nullptr;
	gs_eparam_t *p_samples_ = nullptr;
	gs_eparam_t *p_offset_ = nullptr;
	gs_eparam_t *p_center_ = nullptr;
	gs_eparam_t *p_strength_ = nullptr;
	gs_eparam_t *p_mix_ = nullptr;
	// The mosaic's cell shapes, the trail and Scene Blur's mask are a shader
	// file of their own, newer than the rest and proven on OpenGL only. If
	// Direct3D should refuse it, the blur's own file still loads and
	// everything older works.
	gs_effect_t *looks_ = nullptr;
	gs_eparam_t *pl_image_ = nullptr;
	gs_eparam_t *pl_image2_ = nullptr;
	gs_eparam_t *pl_mix2_ = nullptr;
	gs_eparam_t *pl_texel_ = nullptr;
	gs_eparam_t *pl_size_ = nullptr;
	gs_eparam_t *pl_strength_ = nullptr;
	gs_eparam_t *pl_shape_ = nullptr;
	gs_eparam_t *pl_smoothing_ = nullptr;
	gs_eparam_t *pl_glass_ = nullptr;
	gs_eparam_t *pl_glass_style_ = nullptr;
	gs_eparam_t *pl_whole_ = nullptr;
	gs_eparam_t *pl_open_edge_ = nullptr;
	gs_eparam_t *pl_fallback_ = nullptr;
	gs_eparam_t *pl_keep_alpha_ = nullptr;
	gs_eparam_t *pl_origin_ = nullptr;
	gs_eparam_t *pl_turn_ = nullptr;
	gs_eparam_t *pl_original_ = nullptr;
	gs_eparam_t *pl_mask_ = nullptr;
	gs_eparam_t *pl_mix_ = nullptr;
	gs_eparam_t *pl_counted_ = nullptr;

	gs_texrender_t *tr_a_ = nullptr;
	gs_texrender_t *tr_b_ = nullptr;
	// Temporal: what was drawn last time, kept in more than 8 bits so that a
	// long trail still fades all the way out. Moved on once per frame, however
	// often the filter is drawn in it.
	gs_texrender_t *tr_past_[2] = {nullptr, nullptr};
	int past_ = 0;
	bool past_valid_ = false;
	uint32_t past_w_ = 0, past_h_ = 0;
	std::atomic<float> frame_seconds_{0.0f};
	std::atomic<bool> new_frame_{false};
	LevelTargets tr_down_{};
	LevelTargets tr_up_{};
	LevelTargets tr_down2_{};
	LevelTargets tr_up2_{};
};

} // namespace stagekit
