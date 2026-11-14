/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Ambient Background: one source that produces a soft, slowly moving
// backdrop from an image (or another source), optionally tinted by whatever
// capture is live. It replaces the usual stack of source clones, nested
// scenes and five or six filters per variant.
//
// Pipeline (the picture is built at half resolution, since it ends up
// blurred anyway):
//   base image/source --scroll, saturation, glow--> scene
//   live capture      --cover, opacity------------> scene
//   scene -> dual-Kawase blur -> square/hex mosaic -> texture overlay,
//   vignette, brightness -> output

#include "ambient-background.hpp"
#include "../common/timing.hpp"

#include "../capture/standby.hpp"
#include "../common/kawase-blur.hpp"
#include "../common/scene-util.hpp"
#include "../common/start-image.hpp"
#include "../frame/auto-frame.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/image-file.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

enum class BaseType : int { Image = 0, Source = 1, StartImage = 2 };
enum class LiveMode : int { None = 0, Auto = 1, Source = 2 };
// How the live capture is laid over the screen.
enum class LiveFit : int { Fill = 0, Fit = 1, Stretch = 2, Width = 3, Height = 4 };
// Saved by number: new ones go at the end. From Circle on they follow the
// blur shader's shape numbers, one higher.
enum class PixelStyle : int {
	None = 0,
	Square = 1,
	Hex = 2,
	Circle = 3,
	Triangle = 4,
	Voronoi = 5,
	Rhomboid = 6,
	Triakis = 7,
};
// How the picture is blurred. Temporal blurs over time, not across the picture.
enum class BlurKind : int { Kawase = 0, Gaussian = 1, Box = 2, Temporal = 3 };
enum class OverlayMode : int { Multiply = 0, Screen = 1 };

// Enough halvings for the strongest blur the slider offers (1000).
constexpr int kBlurLevels = 8;
constexpr double kInternalScale = 0.5;
// A Gaussian or box blur reaching further than this many (internal) pixels is
// drawn the Dual Kawase way: the taps would stand too far apart.
constexpr double kTapLimit = 125.0;

struct Settings {
	BaseType base_type = BaseType::Image;
	std::string image;
	std::string base_source;
	double speed_x = 30.0;
	double speed_y = 15.0;
	double zoom = 1.2;
	bool mirror = true;
	LiveMode live_mode = LiveMode::None;
	std::string live_source;
	double live_opacity = 0.7;
	LiveFit live_fit = LiveFit::Fill;
	double live_zoom = 1.0;
	bool live_replace = false;     // the capture is the whole background while there is one
	bool live_ready = true;        // keep the capture running in scenes that do not show it
	bool live_skip_standby = true; // a Standby Picture standing in for the capture is no capture
	double blur = 60.0;
	BlurKind blur_kind = BlurKind::Kawase;
	PixelStyle pixel_style = PixelStyle::None;
	double pixel_size = 10.0;
	double pixel_smooth = 0.0; // 0..1 of a fifth of a cell
	double pixel_origin_x = 0.5;
	double pixel_origin_y = 0.5;
	double pixel_rotation = 0.0;
	double bloom = 0.0;           // how strongly the bright parts shine, 0 = off
	double bloom_threshold = 0.6; // how bright a part must be to shine
	double bloom_size = 80.0;     // how far the light spreads, on the blur's scale
	double saturation = 1.0;
	double glow = 0.0;
	double brightness = 1.0;
	double vignette = 0.0;
	std::string overlay;
	OverlayMode overlay_mode = OverlayMode::Multiply;
	double overlay_strength = 1.0;
	bool overlay_gray = true;
};

// Finds the largest visible capture in a scene, looking inside nested
// scenes and groups.
struct CaptureSearch {
	obs_source_t *best = nullptr;
	double best_area = 0.0;
	double scale = 1.0;
	int depth = 0;
};

bool search_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<CaptureSearch *>(param);
	if (!obs_sceneitem_visible(item))
		return true;
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	vec2 item_scale;
	obs_sceneitem_get_scale(item, &item_scale);
	const double scale = search->scale * std::fabs((double)item_scale.x * (double)item_scale.y);

	obs_scene_t *nested = obs_scene_from_source(source);
	if (!nested)
		nested = obs_group_from_source(source);
	if (nested) {
		if (search->depth < 8) {
			const double outer = search->scale;
			search->scale = scale;
			search->depth++;
			obs_scene_enum_items(nested, search_item, search);
			search->depth--;
			search->scale = outer;
		}
		return true;
	}

	if (is_capture_source(source)) {
		const double area = (double)obs_source_get_width(source) * obs_source_get_height(source) * scale;
		if (area > search->best_area) {
			search->best_area = area;
			search->best = source;
		}
	}
	return true;
}

class AmbientBackground {
public:
	AmbientBackground(obs_data_t *settings, obs_source_t *source);
	~AmbientBackground();

	void update(obs_data_t *settings);
	void tick(float seconds);
	void render();
	void enum_children(obs_source_enum_proc_t cb, void *param);

	uint32_t width() const { return width_; }
	uint32_t height() const { return height_; }

private:
	void load_image(gs_image_file4_t &file, bool &loaded, const std::string &path);
	void set_child(obs_weak_source_t *&slot, obs_source_t *source);
	obs_source_t *child(obs_weak_source_t *slot);
	void resolve_children();
	gs_texture_t *render_child(obs_source_t *child, gs_texrender_t *target, bool picture_only = false);
	void draw_layer(gs_texture_t *tex, uint32_t w, uint32_t h, double scroll_x, double scroll_y, double zoom,
			double opacity, const Settings &s, LiveFit fit = LiveFit::Fill, bool clip = false,
			double focus_x = 0.5, double focus_y = 0.5);
	void pass(gs_effect_t *effect, gs_eparam_t *image, gs_texrender_t *dst, gs_texture_t *src, uint32_t w,
		  uint32_t h, const char *technique);
	gs_texture_t *blur(gs_texture_t *input, uint32_t w, uint32_t h, double strength);
	gs_texture_t *blur_taps(gs_texture_t *input, uint32_t w, uint32_t h, double strength, bool box);
	gs_texture_t *trail(gs_texture_t *input, uint32_t w, uint32_t h, double strength);

	obs_source_t *source_;
	std::mutex mutex_; // guards settings_ and the child references
	Settings settings_;
	uint32_t width_ = 1920;
	uint32_t height_ = 1080;
	double time_ = 0.0;
	double resolve_timer_ = 0.0;
	// False while the capture is only being kept running, not shown.
	std::atomic<bool> live_shown_{true};
	bool rendering_ = false;

	obs_weak_source_t *base_child_ = nullptr;
	obs_weak_source_t *live_child_ = nullptr;

	gs_image_file4_t base_image_ = {};
	bool base_image_loaded_ = false;
	std::string base_image_path_;
	uint64_t start_revision_ = 0;
	uint64_t start_choice_ = start_image_choice();
	gs_image_file4_t overlay_image_ = {};
	bool overlay_image_loaded_ = false;

	gs_effect_t *effect_ = nullptr;
	gs_eparam_t *p_image_ = nullptr;
	gs_eparam_t *p_overlay_ = nullptr;
	gs_eparam_t *p_uv_scale_ = nullptr;
	gs_eparam_t *p_uv_offset_ = nullptr;
	gs_eparam_t *p_wrap_mirror_ = nullptr;
	gs_eparam_t *p_clip_edges_ = nullptr;
	gs_eparam_t *p_opacity_ = nullptr;
	gs_eparam_t *p_saturation_ = nullptr;
	gs_eparam_t *p_gain_ = nullptr;
	gs_eparam_t *p_overlay_scale_ = nullptr;
	gs_eparam_t *p_overlay_strength_ = nullptr;
	gs_eparam_t *p_overlay_mode_ = nullptr;
	gs_eparam_t *p_overlay_gray_ = nullptr;
	gs_eparam_t *p_brightness_ = nullptr;
	gs_eparam_t *p_vignette_ = nullptr;

	gs_effect_t *blur_effect_ = nullptr;
	gs_eparam_t *pb_image_ = nullptr;
	gs_eparam_t *pb_texel_ = nullptr;
	gs_eparam_t *pb_offset_ = nullptr;
	gs_eparam_t *pb_direction_ = nullptr;
	gs_eparam_t *pb_radius_ = nullptr;
	gs_eparam_t *pb_samples_ = nullptr;
	gs_eparam_t *p_size_ = nullptr;
	gs_eparam_t *p_cell_ = nullptr;
	// The mosaic's cell shapes, the trail and bloom are a shader file of their
	// own, newer than the rest and proven on OpenGL only. If Direct3D should
	// refuse it, the older files still load and everything older works.
	gs_effect_t *looks_ = nullptr;
	gs_eparam_t *pl_image_ = nullptr;
	gs_eparam_t *pl_image2_ = nullptr;
	gs_eparam_t *pl_mix2_ = nullptr;
	gs_eparam_t *pl_texel_ = nullptr;
	gs_eparam_t *pl_size_ = nullptr;
	gs_eparam_t *pl_strength_ = nullptr;
	gs_eparam_t *pl_shape_ = nullptr;
	gs_eparam_t *pl_smoothing_ = nullptr;
	gs_eparam_t *pl_origin_ = nullptr;
	gs_eparam_t *pl_turn_ = nullptr;
	gs_eparam_t *pl_bloom_threshold_ = nullptr;
	gs_eparam_t *pl_bloom_amount_ = nullptr;
	// Bloom: the bright parts, and those spread out.
	gs_texrender_t *tr_bright_ = nullptr;
	gs_texrender_t *tr_bloom_ = nullptr;
	std::unique_ptr<KawaseBlur> bloom_blur_;
	gs_texrender_t *tr_taps_ = nullptr; // between the two passes of a Gaussian or box blur
	// Temporal: what was drawn last time, in more than 8 bits so that a long
	// trail fades all the way out; moved on once per frame.
	gs_texrender_t *tr_past_[2] = {nullptr, nullptr};
	int past_ = 0;
	bool past_valid_ = false;
	uint32_t past_w_ = 0, past_h_ = 0;
	std::atomic<float> frame_seconds_{0.0f};
	std::atomic<bool> new_frame_{false};

	gs_texrender_t *tr_base_ = nullptr;
	gs_texrender_t *tr_live_ = nullptr;
	gs_texrender_t *tr_scene_ = nullptr;

	// Whether the live capture is showing anything, looked at twice a second
	// through a small copy of it.
	bool live_shows_something(gs_texture_t *live);
	gs_texrender_t *tr_probe_ = nullptr;
	gs_stagesurf_t *probe_stage_ = nullptr;
	double probe_at_ = -1.0;    // when it was last looked at
	double black_since_ = -1.0; // when it was first seen all black
	bool live_empty_ = false;
	gs_texrender_t *tr_blur_ = nullptr;
	gs_texrender_t *tr_pixel_ = nullptr;
	gs_texrender_t *tr_final_ = nullptr;
	std::array<gs_texrender_t *, kBlurLevels> tr_down_{};
	std::array<gs_texrender_t *, kBlurLevels> tr_up_{};
};

gs_effect_t *load_effect(const char *file)
{
	char *path = obs_module_file(file);
	char *errors = nullptr;
	gs_effect_t *effect = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect)
		obs_log(LOG_ERROR, "Ambient Background: failed to load '%s': %s", file, errors ? errors : "not found");
	bfree(errors);
	bfree(path);
	return effect;
}

AmbientBackground::AmbientBackground(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	obs_enter_graphics();
	effect_ = load_effect("effects/stagekit-ambient.effect");
	blur_effect_ = load_effect("effects/stagekit-blur.effect");
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_overlay_ = gs_effect_get_param_by_name(effect_, "overlay");
		p_uv_scale_ = gs_effect_get_param_by_name(effect_, "uv_scale");
		p_uv_offset_ = gs_effect_get_param_by_name(effect_, "uv_offset");
		p_wrap_mirror_ = gs_effect_get_param_by_name(effect_, "wrap_mirror");
		p_clip_edges_ = gs_effect_get_param_by_name(effect_, "clip_edges");
		p_opacity_ = gs_effect_get_param_by_name(effect_, "opacity");
		p_saturation_ = gs_effect_get_param_by_name(effect_, "saturation");
		p_gain_ = gs_effect_get_param_by_name(effect_, "gain");
		p_size_ = gs_effect_get_param_by_name(effect_, "size");
		p_cell_ = gs_effect_get_param_by_name(effect_, "cell");
		p_overlay_scale_ = gs_effect_get_param_by_name(effect_, "overlay_scale");
		p_overlay_strength_ = gs_effect_get_param_by_name(effect_, "overlay_strength");
		p_overlay_mode_ = gs_effect_get_param_by_name(effect_, "overlay_mode");
		p_overlay_gray_ = gs_effect_get_param_by_name(effect_, "overlay_gray");
		p_brightness_ = gs_effect_get_param_by_name(effect_, "brightness");
		p_vignette_ = gs_effect_get_param_by_name(effect_, "vignette");
	}
	if (blur_effect_) {
		pb_image_ = gs_effect_get_param_by_name(blur_effect_, "image");
		pb_texel_ = gs_effect_get_param_by_name(blur_effect_, "texel");
		pb_offset_ = gs_effect_get_param_by_name(blur_effect_, "offset");
		pb_direction_ = gs_effect_get_param_by_name(blur_effect_, "direction");
		pb_radius_ = gs_effect_get_param_by_name(blur_effect_, "radius");
		pb_samples_ = gs_effect_get_param_by_name(blur_effect_, "samples");
	}
	looks_ = load_effect("effects/stagekit-looks.effect");
	if (looks_) {
		pl_image_ = gs_effect_get_param_by_name(looks_, "image");
		pl_image2_ = gs_effect_get_param_by_name(looks_, "image2");
		pl_mix2_ = gs_effect_get_param_by_name(looks_, "mix2");
		pl_texel_ = gs_effect_get_param_by_name(looks_, "texel");
		pl_size_ = gs_effect_get_param_by_name(looks_, "size");
		pl_strength_ = gs_effect_get_param_by_name(looks_, "strength");
		pl_shape_ = gs_effect_get_param_by_name(looks_, "shape");
		pl_smoothing_ = gs_effect_get_param_by_name(looks_, "smoothing");
		pl_origin_ = gs_effect_get_param_by_name(looks_, "origin");
		pl_turn_ = gs_effect_get_param_by_name(looks_, "turn");
		pl_bloom_threshold_ = gs_effect_get_param_by_name(looks_, "bloom_threshold");
		pl_bloom_amount_ = gs_effect_get_param_by_name(looks_, "bloom_amount");
	} else {
		obs_log(LOG_ERROR,
			"Ambient Background: cell shapes other than squares and hexagons, the Temporal blur type and Bloom are not available.");
	}
	bloom_blur_ = std::make_unique<KawaseBlur>();
	for (gs_texrender_t *&tr : tr_past_)
		tr = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	for (gs_texrender_t **tr :
	     {&tr_base_, &tr_live_, &tr_scene_, &tr_blur_, &tr_pixel_, &tr_final_, &tr_bright_, &tr_bloom_, &tr_taps_})
		*tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (auto &tr : tr_down_)
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (auto &tr : tr_up_)
		tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

	update(settings);
}

AmbientBackground::~AmbientBackground()
{
	set_child(base_child_, nullptr);
	set_child(live_child_, nullptr);

	obs_enter_graphics();
	if (base_image_loaded_)
		gs_image_file4_free(&base_image_);
	if (overlay_image_loaded_)
		gs_image_file4_free(&overlay_image_);
	gs_effect_destroy(effect_);
	gs_effect_destroy(blur_effect_);
	gs_effect_destroy(looks_);
	gs_texrender_destroy(tr_probe_);
	gs_stagesurface_destroy(probe_stage_);
	for (gs_texrender_t *tr : {tr_base_, tr_live_, tr_scene_, tr_blur_, tr_pixel_, tr_final_, tr_bright_, tr_bloom_,
				   tr_taps_, tr_past_[0], tr_past_[1]})
		gs_texrender_destroy(tr);
	bloom_blur_.reset();
	for (auto *tr : tr_down_)
		gs_texrender_destroy(tr);
	for (auto *tr : tr_up_)
		gs_texrender_destroy(tr);
	obs_leave_graphics();
}

void AmbientBackground::load_image(gs_image_file4_t &file, bool &loaded, const std::string &path)
{
	obs_enter_graphics();
	if (loaded) {
		gs_image_file4_free(&file);
		loaded = false;
	}
	obs_leave_graphics();
	if (path.empty())
		return;

	gs_image_file4_init(&file, path.c_str(), GS_IMAGE_ALPHA_STRAIGHT);
	obs_enter_graphics();
	gs_image_file4_init_texture(&file);
	obs_leave_graphics();
	loaded = true;
	if (!file.image3.image2.image.loaded)
		obs_log(LOG_WARNING, "Ambient Background: could not load image '%s'", path.c_str());
}

void AmbientBackground::update(obs_data_t *settings)
{
	Settings s;
	s.base_type = static_cast<BaseType>(obs_data_get_int(settings, "base_type"));
	s.image = obs_data_get_string(settings, "image");
	s.base_source = obs_data_get_string(settings, "base_source");
	s.speed_x = obs_data_get_double(settings, "speed_x");
	s.speed_y = obs_data_get_double(settings, "speed_y");
	s.zoom = std::clamp(obs_data_get_double(settings, "zoom") / 100.0, 1.0, 4.0);
	s.mirror = obs_data_get_bool(settings, "mirror");
	s.live_mode = static_cast<LiveMode>(obs_data_get_int(settings, "live_mode"));
	s.live_source = obs_data_get_string(settings, "live_source");
	s.live_opacity = std::clamp(obs_data_get_double(settings, "live_opacity") / 100.0, 0.0, 1.0);
	s.live_fit = static_cast<LiveFit>(obs_data_get_int(settings, "live_fit"));
	s.live_zoom = std::clamp(obs_data_get_double(settings, "live_zoom") / 100.0, 0.25, 8.0);
	s.live_replace = obs_data_get_bool(settings, "live_replace");
	s.live_ready = obs_data_get_bool(settings, "live_ready");
	s.live_skip_standby = obs_data_get_bool(settings, "live_skip_standby");
	s.blur = std::clamp(obs_data_get_double(settings, "blur"), 0.0, 1000.0);
	s.blur_kind = static_cast<BlurKind>(std::clamp<long long>(obs_data_get_int(settings, "blur_type"), 0, 3));
	s.pixel_style = static_cast<PixelStyle>(std::clamp<long long>(obs_data_get_int(settings, "pixel_style"), 0, 7));
	s.pixel_size = std::clamp(obs_data_get_double(settings, "pixel_size"), 2.0, 200.0);
	s.pixel_smooth = std::clamp(obs_data_get_double(settings, "pixel_smooth") / 100.0, 0.0, 1.0);
	s.pixel_origin_x = obs_data_get_double(settings, "pixel_origin_x") / 100.0;
	s.pixel_origin_y = obs_data_get_double(settings, "pixel_origin_y") / 100.0;
	s.pixel_rotation = obs_data_get_double(settings, "pixel_rotation");
	s.bloom = std::clamp(obs_data_get_double(settings, "bloom") / 100.0, 0.0, 3.0);
	s.bloom_threshold = std::clamp(obs_data_get_double(settings, "bloom_threshold") / 100.0, 0.0, 1.0);
	s.bloom_size = std::clamp(obs_data_get_double(settings, "bloom_size"), 0.0, 1000.0);
	s.saturation = std::clamp(obs_data_get_double(settings, "saturation") / 100.0, 0.0, 3.0);
	s.glow = std::clamp(obs_data_get_double(settings, "glow") / 100.0, 0.0, 1.0);
	s.brightness = std::clamp(obs_data_get_double(settings, "brightness") / 100.0, 0.0, 2.0);
	s.vignette = std::clamp(obs_data_get_double(settings, "vignette") / 100.0, 0.0, 1.0);
	s.overlay = obs_data_get_string(settings, "overlay");
	s.overlay_mode = static_cast<OverlayMode>(obs_data_get_int(settings, "overlay_mode"));
	s.overlay_strength = std::clamp(obs_data_get_double(settings, "overlay_strength") / 100.0, 0.0, 1.0);
	s.overlay_gray = obs_data_get_bool(settings, "overlay_gray");

	std::string old_overlay;
	{
		std::lock_guard lock(mutex_);
		old_overlay = settings_.overlay;
		settings_ = s;
	}
	// The picture is this source's own file, or the shared start image.
	// With no start image chosen yet, its own file stands in.
	const std::string start = ambient_picture_path();
	const std::string wanted = s.base_type == BaseType::StartImage && !start.empty() ? start : s.image;
	start_revision_ = start_image_revision();
	if (wanted != base_image_path_ || (!wanted.empty() && !base_image_loaded_)) {
		base_image_path_ = wanted;
		load_image(base_image_, base_image_loaded_, wanted);
	}
	if (s.overlay != old_overlay || (!s.overlay.empty() && !overlay_image_loaded_))
		load_image(overlay_image_, overlay_image_loaded_, s.overlay);
	resolve_timer_ = 0.0;
}

void AmbientBackground::set_child(obs_weak_source_t *&slot, obs_source_t *source)
{
	obs_weak_source_t *weak = source ? obs_source_get_weak_source(source) : nullptr;
	obs_weak_source_t *old;
	{
		std::lock_guard lock(mutex_);
		old = slot;
		slot = weak;
	}
	// Tell OBS which sources this one shows, so captures and media are
	// activated while the background is on screen and released afterwards.
	if (!obs_weak_source_references_source(old, source)) {
		if (obs_source_t *old_source = old ? obs_weak_source_get_source(old) : nullptr) {
			obs_source_remove_active_child(source_, old_source);
			obs_source_release(old_source);
		}
		if (source)
			obs_source_add_active_child(source_, source);
	}
	obs_weak_source_release(old);
}

obs_source_t *AmbientBackground::child(obs_weak_source_t *slot)
{
	return slot ? obs_weak_source_get_source(slot) : nullptr;
}

// Looks the base and live sources up again. Runs a couple of times a second
// so sources that load later, get renamed or change scene are picked up.
void AmbientBackground::resolve_children()
{
	Settings s;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
	}

	obs_source_t *base = nullptr;
	if (s.base_type == BaseType::Source && !s.base_source.empty())
		base = obs_get_source_by_name(s.base_source.c_str());
	if (base == source_) {
		obs_source_release(base);
		base = nullptr;
	}
	set_child(base_child_, base);
	obs_source_release(base);

	obs_source_t *live = nullptr;
	bool shown = true;
	if (s.live_mode == LiveMode::Source && !s.live_source.empty()) {
		live = obs_get_source_by_name(s.live_source.c_str());
	} else if (s.live_mode == LiveMode::Auto) {
		obs_source_t *scene_source = scene_on_screen();
		if (obs_scene_t *scene = obs_scene_from_source(scene_source)) {
			CaptureSearch search;
			obs_scene_enum_items(scene, search_item, &search);
			live = obs_source_get_ref(search.best);
		}
		obs_source_release(scene_source);
		if (!live && s.live_ready) {
			// This scene shows no capture. A capture that is let go of stops
			// and takes a moment to start again, so the one last used (or,
			// failing that, the largest in any scene) is kept running, out of
			// sight, ready for the next scene that shows it.
			shown = false;
			{
				std::lock_guard lock(mutex_);
				live = child(live_child_);
			}
			if (live && !is_capture_source(live)) {
				obs_source_release(live);
				live = nullptr;
			}
			if (!live) {
				CaptureSearch anywhere;
				obs_enum_scenes(
					[](void *param, obs_source_t *any_scene) {
						if (obs_scene_t *sc = obs_scene_from_source(any_scene))
							obs_scene_enum_items(sc, search_item, param);
						return true;
					},
					&anywhere);
				live = obs_source_get_ref(anywhere.best);
			}
		}
	}
	live_shown_ = shown;
	if (live == source_) {
		obs_source_release(live);
		live = nullptr;
	}
	set_child(live_child_, live);
	obs_source_release(live);
}

void AmbientBackground::tick(float seconds)
{
	time_ += seconds;
	frame_seconds_ = std::min(frame_seconds_.load() + seconds, 1.0f); // since the trail last moved on
	new_frame_ = true;

	// A start image picked by the owner becomes this background's picture
	// without a visit to these settings.
	if (start_image_choice() != start_choice_) {
		start_choice_ = start_image_choice();
		obs_data_t *settings = obs_source_get_settings(source_);
		if (static_cast<BaseType>(obs_data_get_int(settings, "base_type")) == BaseType::Image) {
			obs_data_set_int(settings, "base_type", (int)BaseType::StartImage);
			obs_source_update(source_, settings);
		}
		obs_data_release(settings);
	}

	// Pick up a new start image as soon as one is chosen.
	if (start_image_revision() != start_revision_) {
		start_revision_ = start_image_revision();
		bool uses_it;
		{
			std::lock_guard lock(mutex_);
			uses_it = settings_.base_type == BaseType::StartImage;
		}
		const std::string path = ambient_picture_path();
		if (uses_it && !path.empty() && path != base_image_path_) {
			base_image_path_ = path;
			load_image(base_image_, base_image_loaded_, path);
		}
	}

	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		width_ = ovi.base_width;
		height_ = ovi.base_height;
	}

	resolve_timer_ -= seconds;
	if (resolve_timer_ <= 0.0) {
		// Often enough that a scene change is followed at once.
		resolve_timer_ = 0.1;
		resolve_children();
	}
}

void AmbientBackground::enum_children(obs_source_enum_proc_t cb, void *param)
{
	obs_source_t *base, *live;
	{
		std::lock_guard lock(mutex_);
		base = child(base_child_);
		live = child(live_child_);
	}
	if (base)
		cb(source_, base, param);
	if (live)
		cb(source_, live, param);
	obs_source_release(base);
	obs_source_release(live);
}

// ---------------------------------------------------------------- rendering

gs_texture_t *AmbientBackground::render_child(obs_source_t *child, gs_texrender_t *target, bool picture_only)
{
	const uint32_t w = obs_source_get_width(child);
	const uint32_t h = obs_source_get_height(child);
	if (!w || !h)
		return nullptr;

	// A capture with an Auto Frame is a frame with the game picture somewhere
	// inside it. Only the picture is wanted: the empty frame around it would
	// show as black bars.
	double px = 0.0, py = 0.0, pw = w, ph = h;
	if (picture_only)
		auto_frame_picture(child, px, py, pw, ph);
	const uint32_t tw = std::max(1u, (uint32_t)std::lround(pw)), th = std::max(1u, (uint32_t)std::lround(ph));

	gs_texrender_reset(target);
	if (!gs_texrender_begin(target, tw, th))
		return nullptr;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho((float)px, (float)(px + pw), (float)py, (float)(py + ph), -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	obs_source_video_render(child);
	gs_blend_state_pop();
	gs_texrender_end(target);
	return gs_texrender_get_texture(target);
}

// Nothing drawn at all counts as empty at once. A picture that is black all
// over counts after two seconds, so a game's cut to black does not flash the
// usual picture in; anything visible counts as a picture again immediately.
bool AmbientBackground::live_shows_something(gs_texture_t *live)
{
	constexpr uint32_t kProbe = 64;
	constexpr double kEvery = 0.5, kBlackFor = 2.0;
	if (probe_at_ >= 0.0 && time_ >= probe_at_ && time_ - probe_at_ < kEvery)
		return !live_empty_;
	probe_at_ = time_;

	if (!tr_probe_)
		tr_probe_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!probe_stage_)
		probe_stage_ = gs_stagesurface_create(kProbe, kProbe, GS_RGBA);
	gs_texrender_reset(tr_probe_);
	if (!tr_probe_ || !probe_stage_ || !gs_texrender_begin(tr_probe_, kProbe, kProbe))
		return !live_empty_;
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)kProbe, 0.0f, (float)kProbe, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), live);
	while (gs_effect_loop(def, "Draw"))
		gs_draw_sprite(live, 0, kProbe, kProbe);
	gs_blend_state_pop();
	gs_texrender_end(tr_probe_);

	bool anything = false, bright = false;
	gs_stage_texture(probe_stage_, gs_texrender_get_texture(tr_probe_));
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (!gs_stagesurface_map(probe_stage_, &data, &linesize))
		return !live_empty_;
	for (uint32_t y = 0; y < kProbe && !bright; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < kProbe; x++) {
			const uint8_t *p = row + x * 4;
			if (p[3] <= 12)
				continue;
			anything = true;
			if (std::max({p[0], p[1], p[2]}) > 16) {
				bright = true;
				break;
			}
		}
	}
	gs_stagesurface_unmap(probe_stage_);

	if (bright) {
		black_since_ = -1.0;
		live_empty_ = false;
	} else if (!anything) {
		black_since_ = -1.0;
		live_empty_ = true;
	} else {
		if (black_since_ < 0.0 || time_ < black_since_)
			black_since_ = time_;
		if (time_ - black_since_ >= kBlackFor)
			live_empty_ = true;
	}
	return !live_empty_;
}

// Draws tex so it covers the current render target (out_w x out_h set by the
// caller's ortho), scrolled by the given number of output pixels.
void AmbientBackground::draw_layer(gs_texture_t *tex, uint32_t out_w, uint32_t out_h, double scroll_x, double scroll_y,
				   double zoom, double opacity, const Settings &s, LiveFit fit, bool clip,
				   double focus_x, double focus_y)
{
	const double tex_aspect = (double)gs_texture_get_width(tex) / (double)gs_texture_get_height(tex);
	const double out_aspect = (double)out_w / (double)out_h;
	// The part of the texture one screen shows, across and down. Below 1
	// the texture is cropped; above 1 it is smaller than the screen.
	const double match_width = tex_aspect / out_aspect;  // down, when the widths match
	const double match_height = out_aspect / tex_aspect; // across, when the heights match
	const bool wider = tex_aspect > out_aspect;
	double fx = 1.0, fy = 1.0;
	switch (fit) {
	case LiveFit::Fit:
		(wider ? fy : fx) = wider ? match_width : match_height;
		break;
	case LiveFit::Stretch:
		break;
	case LiveFit::Width:
		fy = match_width;
		break;
	case LiveFit::Height:
		fx = match_height;
		break;
	default: // fill
		(wider ? fx : fy) = wider ? match_height : match_width;
	}
	fx /= zoom;
	fy /= zoom;

	// Scroll is given in output pixels; one uv unit of the texture spans
	// out_w / fx output pixels. Mirroring repeats every two units.
	// Zoomed in, the picture is centred on the chosen point as far as its
	// edges allow.
	const double ox = std::fmod(scroll_x * fx / (double)out_w, 2.0) + (focus_x - 0.5) * std::max(0.0, 1.0 - fx);
	const double oy = std::fmod(scroll_y * fy / (double)out_h, 2.0) + (focus_y - 0.5) * std::max(0.0, 1.0 - fy);

	vec2 scale, offset;
	vec2_set(&scale, (float)fx, (float)fy);
	vec2_set(&offset, (float)ox, (float)oy);
	gs_effect_set_texture(p_image_, tex);
	gs_effect_set_vec2(p_uv_scale_, &scale);
	gs_effect_set_vec2(p_uv_offset_, &offset);
	gs_effect_set_float(p_wrap_mirror_, s.mirror ? 1.0f : 0.0f);
	gs_effect_set_float(p_clip_edges_, clip ? 1.0f : 0.0f);
	gs_effect_set_float(p_opacity_, (float)opacity);
	gs_effect_set_float(p_saturation_, (float)s.saturation);
	gs_effect_set_float(p_gain_, (float)(1.0 + s.glow * 3.0));
	while (gs_effect_loop(effect_, "Layer"))
		gs_draw_sprite(tex, 0, out_w, out_h);
}

void AmbientBackground::pass(gs_effect_t *effect, gs_eparam_t *image, gs_texrender_t *dst, gs_texture_t *src,
			     uint32_t w, uint32_t h, const char *technique)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_effect_set_texture(image, src);
	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(src, 0, w, h);
	gs_blend_state_pop();
	gs_texrender_end(dst);
}

// Dual-Kawase blur. strength is in pixels of the input texture, on the same
// scale as Ramp Blur's strength.
gs_texture_t *AmbientBackground::blur(gs_texture_t *input, uint32_t w, uint32_t h, double strength)
{
	const double scaled = strength * 0.14;
	if (!blur_effect_ || scaled < 0.05)
		return input;

	int levels = std::clamp((int)std::floor(std::log2(std::max(scaled, 1.0))) + 1, 1, kBlurLevels);
	while (levels > 1 && ((w >> levels) < 2 || (h >> levels) < 2))
		levels--;
	const float offset = (float)(scaled / std::pow(2.0, levels - 1));

	std::array<uint32_t, kBlurLevels + 1> lw{}, lh{};
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
		gs_effect_set_vec2(pb_texel_, &texel);
		gs_effect_set_float(pb_offset_, offset);
		pass(blur_effect_, pb_image_, tr_down_[i], cur, lw[i + 1], lh[i + 1], "KawaseDown");
		cur = gs_texrender_get_texture(tr_down_[i]);
	}
	for (int i = levels; i >= 1; i--) {
		vec2 texel;
		vec2_set(&texel, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(pb_texel_, &texel);
		gs_effect_set_float(pb_offset_, offset);
		gs_texrender_t *dst = i == 1 ? tr_blur_ : tr_up_[i - 1];
		pass(blur_effect_, pb_image_, dst, cur, lw[i - 1], lh[i - 1], "KawaseUp");
		cur = gs_texrender_get_texture(dst);
	}
	return cur;
}

// A Gaussian or box blur, tap by tap in two passes. Past what the taps can
// cover without showing as stripes it is drawn the Dual Kawase way, as the
// Blur filter does.
gs_texture_t *AmbientBackground::blur_taps(gs_texture_t *input, uint32_t w, uint32_t h, double strength, bool box)
{
	if (!blur_effect_ || strength < 0.5)
		return input;
	if (strength > kTapLimit)
		return blur(input, w, h, strength);
	const char *technique = box ? "Box" : "Gaussian";
	const float taps = (float)std::clamp(std::ceil(strength), 1.0, box ? 64.0 : 32.0);
	vec2 texel, dir;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);
	for (int axis = 0; axis < 2; axis++) {
		gs_effect_set_vec2(pb_texel_, &texel);
		gs_effect_set_float(pb_radius_, (float)strength);
		gs_effect_set_float(pb_samples_, taps);
		vec2_set(&dir, axis == 0 ? texel.x : 0.0f, axis == 0 ? 0.0f : texel.y);
		gs_effect_set_vec2(pb_direction_, &dir);
		pass(blur_effect_, pb_image_, axis == 0 ? tr_taps_ : tr_blur_, input, w, h, technique);
		input = gs_texrender_get_texture(axis == 0 ? tr_taps_ : tr_blur_);
		if (!input)
			return nullptr;
	}
	return input;
}

// Temporal: each frame is the picture mixed with what was drawn the frame
// before, so whatever moves leaves a trail. The strength is how long a thing
// stays, a hundredth of a second for each point, whatever the frame rate.
gs_texture_t *AmbientBackground::trail(gs_texture_t *input, uint32_t w, uint32_t h, double strength)
{
	if (!looks_ || strength < 0.5) {
		past_valid_ = false;
		return input;
	}
	if (w != past_w_ || h != past_h_) {
		past_w_ = w;
		past_h_ = h;
		past_valid_ = false;
	}
	gs_texture_t *past = past_valid_ ? gs_texrender_get_texture(tr_past_[past_]) : nullptr;
	// Drawn again in the same frame (the preview and the output both show it).
	if (past && !new_frame_.exchange(false))
		return past;
	new_frame_ = false;
	const double keep = past ? std::exp(-(double)frame_seconds_.exchange(0.0f) / (strength / 100.0)) : 0.0;
	gs_effect_set_texture(pl_image2_, past ? past : input);
	gs_effect_set_float(pl_mix2_, (float)std::clamp(keep, 0.0, 0.999));
	pass(looks_, pl_image_, tr_past_[1 - past_], input, w, h, "Temporal");
	past_ = 1 - past_;
	past_valid_ = true;
	return gs_texrender_get_texture(tr_past_[past_]);
}

void AmbientBackground::render()
{
	// A base or live source could (indirectly) contain this background.
	if (rendering_ || !effect_)
		return;
	rendering_ = true;

	Settings s;
	obs_source_t *base_source, *live_source;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
		base_source = child(base_child_);
		live_source = child(live_child_);
	}

	const uint32_t w = width_, h = height_;
	const uint32_t iw = std::max(2u, (uint32_t)(w * kInternalScale));
	const uint32_t ih = std::max(2u, (uint32_t)(h * kInternalScale));

	// Everything below works on raw (non-linear) colour values.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	gs_texture_t *base = nullptr;
	if (s.base_type == BaseType::Source) {
		if (base_source)
			base = render_child(base_source, tr_base_);
	} else if (base_image_loaded_) {
		base = base_image_.image3.image2.image.texture;
	}
	gs_texture_t *live = live_source && live_shown_ ? render_child(live_source, tr_live_, true) : nullptr;
	// A capture with nothing in it is no capture: the usual picture shows.
	if (live && !live_shows_something(live))
		live = nullptr;
	// Nor is a Standby Picture standing in for it: blended in, the
	// background would turn into that still picture.
	if (live && s.live_skip_standby && standby_waiting(live_source))
		live = nullptr;

	gs_texrender_reset(tr_scene_);
	if (gs_texrender_begin(tr_scene_, iw, ih)) {
		vec4 black;
		vec4_set(&black, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
		gs_ortho(0.0f, (float)iw, 0.0f, (float)ih, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		// "Replace": while there is a capture it is the whole background;
		// the moment there is none, the usual picture is back.
		const bool replace = live && s.live_replace;
		if (base && !replace) {
			// The collection-wide framing applies to the shared picture only.
			AmbientView view;
			if (s.base_type == BaseType::StartImage)
				view = ambient_view();
			draw_layer(base, iw, ih, s.speed_x * time_ * kInternalScale, s.speed_y * time_ * kInternalScale,
				   s.zoom * view.zoom, 1.0, s, LiveFit::Fill, false, view.focus_x, view.focus_y);
		}
		if (live) {
			// Blended over the picture, a capture that does not cover the
			// screen leaves the picture showing around it. As the whole
			// background, the edges are filled the usual way instead.
			const bool whole = replace || !base;
			draw_layer(live, iw, ih, 0.0, 0.0, s.live_zoom, whole ? 1.0 : s.live_opacity, s, s.live_fit,
				   !whole);
		}
		gs_blend_state_pop();
		gs_texrender_end(tr_scene_);
	}

	gs_texture_t *const scene = gs_texrender_get_texture(tr_scene_);
	gs_texture_t *cur = scene;
	if (cur) {
		if (s.blur_kind == BlurKind::Temporal) {
			cur = trail(cur, iw, ih, s.blur);
		} else {
			past_valid_ = false;
			cur = s.blur_kind == BlurKind::Kawase
				      ? blur(cur, iw, ih, s.blur * kInternalScale)
				      : blur_taps(cur, iw, ih, s.blur * kInternalScale, s.blur_kind == BlurKind::Box);
		}
	}

	// Bloom: the bright parts of the picture as it was before the blur,
	// spread out and laid over it as light.
	if (cur && scene && s.bloom > 0.001 && bloom_blur_ && looks_) {
		gs_effect_set_float(pl_bloom_threshold_, (float)s.bloom_threshold);
		pass(looks_, pl_image_, tr_bright_, scene, iw, ih, "BloomBright");
		gs_texture_t *light = gs_texrender_get_texture(tr_bright_);
		if (light)
			light = bloom_blur_->blur(light, iw, ih, s.bloom_size * kInternalScale);
		if (light) {
			gs_effect_set_texture(pl_image2_, light);
			gs_effect_set_float(pl_bloom_amount_, (float)s.bloom);
			pass(looks_, pl_image_, tr_bloom_, cur, iw, ih, "BloomAdd");
			if (gs_texture_t *with_light = gs_texrender_get_texture(tr_bloom_))
				cur = with_light;
		}
	}

	if (cur && s.pixel_style != PixelStyle::None && looks_) {
		const double cell = std::max(1.0, s.pixel_size * kInternalScale);
		const double rad = s.pixel_rotation * 3.14159265358979323846 / 180.0;
		vec2 size, texel, origin, turn;
		vec2_set(&size, (float)iw, (float)ih);
		vec2_set(&texel, 1.0f / (float)iw, 1.0f / (float)ih);
		vec2_set(&origin, (float)(s.pixel_origin_x * iw), (float)(s.pixel_origin_y * ih));
		vec2_set(&turn, (float)std::cos(rad), (float)std::sin(rad));
		gs_effect_set_vec2(pl_size_, &size);
		gs_effect_set_vec2(pl_texel_, &texel);
		gs_effect_set_float(pl_strength_, (float)cell);
		// The shader's shapes start at squares, the list here at "none".
		const int shape = s.pixel_style == PixelStyle::Square ? 0
				  : s.pixel_style == PixelStyle::Hex  ? 1
								      : (int)s.pixel_style - 1;
		gs_effect_set_float(pl_shape_, (float)shape);
		gs_effect_set_float(pl_smoothing_, (float)(s.pixel_smooth * cell * 0.2));
		gs_effect_set_vec2(pl_origin_, &origin);
		gs_effect_set_vec2(pl_turn_, &turn);
		pass(looks_, pl_image_, tr_pixel_, cur, iw, ih, "Mosaic");
		cur = gs_texrender_get_texture(tr_pixel_);
	} else if (cur && s.pixel_style != PixelStyle::None) {
		// Without the newer shader file: squares and hexagons, as before it.
		vec2 size;
		vec2_set(&size, (float)iw, (float)ih);
		gs_effect_set_vec2(p_size_, &size);
		gs_effect_set_float(p_cell_, (float)std::max(1.0, s.pixel_size * kInternalScale));
		pass(effect_, p_image_, tr_pixel_, cur, iw, ih,
		     s.pixel_style == PixelStyle::Hex ? "PixelHex" : "PixelSquare");
		cur = gs_texrender_get_texture(tr_pixel_);
	}

	if (cur) {
		gs_texture_t *overlay = overlay_image_loaded_ ? overlay_image_.image3.image2.image.texture : nullptr;
		vec2 overlay_scale;
		vec2_set(&overlay_scale, 1.0f, 1.0f);
		if (overlay) {
			const double tex_aspect =
				(double)gs_texture_get_width(overlay) / (double)gs_texture_get_height(overlay);
			const double out_aspect = (double)w / (double)h;
			if (tex_aspect > out_aspect)
				overlay_scale.x = (float)(out_aspect / tex_aspect);
			else
				overlay_scale.y = (float)(tex_aspect / out_aspect);
		}
		gs_effect_set_texture(p_overlay_, overlay ? overlay : cur);
		gs_effect_set_vec2(p_overlay_scale_, &overlay_scale);
		gs_effect_set_float(p_overlay_strength_, overlay ? (float)s.overlay_strength : 0.0f);
		gs_effect_set_float(p_overlay_mode_, s.overlay_mode == OverlayMode::Screen ? 1.0f : 0.0f);
		gs_effect_set_float(p_overlay_gray_, s.overlay_gray ? 1.0f : 0.0f);
		gs_effect_set_float(p_brightness_, (float)s.brightness);
		gs_effect_set_float(p_vignette_, (float)s.vignette);
		pass(effect_, p_image_, tr_final_, cur, w, h, "Final");
	}

	gs_enable_framebuffer_srgb(previous_srgb);
	obs_source_release(base_source);
	obs_source_release(live_source);

	// Hand the finished picture to OBS the standard way.
	if (gs_texture_t *final_tex = cur ? gs_texrender_get_texture(tr_final_) : nullptr) {
		gs_effect_t *draw = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(draw, "image");
		const bool linear = gs_get_linear_srgb();
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(linear);
		if (linear)
			gs_effect_set_texture_srgb(image, final_tex);
		else
			gs_effect_set_texture(image, final_tex);
		while (gs_effect_loop(draw, "Draw"))
			gs_draw_sprite(final_tex, 0, w, h);
		gs_enable_framebuffer_srgb(previous);
	}

	rendering_ = false;
}

// --------------------------------------------------------------- properties

bool add_source_to_list(void *param, obs_source_t *source)
{
	auto *list = static_cast<obs_property_t *>(param);
	if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
		const char *name = obs_source_get_name(source);
		if (name && *name)
			obs_property_list_add_string(list, name, name);
	}
	return true;
}

void fill_source_list(obs_property_t *list, bool include_scenes)
{
	obs_property_list_add_string(list, "", "");
	obs_enum_sources(add_source_to_list, list);
	if (include_scenes)
		obs_enum_scenes(add_source_to_list, list);
}

bool visibility_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto base = static_cast<BaseType>(obs_data_get_int(settings, "base_type"));
	const auto live = static_cast<LiveMode>(obs_data_get_int(settings, "live_mode"));
	const auto pixel = static_cast<PixelStyle>(obs_data_get_int(settings, "pixel_style"));
	const bool has_overlay = *obs_data_get_string(settings, "overlay") != 0;
	obs_property_set_visible(obs_properties_get(props, "image"), base == BaseType::Image);
	obs_property_set_visible(obs_properties_get(props, "base_source"), base == BaseType::Source);
	obs_property_set_visible(obs_properties_get(props, "live_source"), live == LiveMode::Source);
	obs_property_set_visible(obs_properties_get(props, "live_opacity"), live != LiveMode::None);
	for (const char *name : {"live_fit", "live_zoom", "live_replace", "live_ready", "live_skip_standby"})
		obs_property_set_visible(obs_properties_get(props, name), live != LiveMode::None);
	for (const char *name : {"pixel_size", "pixel_smooth", "pixel_origin_x", "pixel_origin_y", "pixel_rotation"})
		obs_property_set_visible(obs_properties_get(props, name), pixel != PixelStyle::None);
	const bool bloom = obs_data_get_double(settings, "bloom") > 0.0;
	obs_property_set_visible(obs_properties_get(props, "bloom_threshold"), bloom);
	obs_property_set_visible(obs_properties_get(props, "bloom_size"), bloom);
	obs_property_set_visible(obs_properties_get(props, "overlay_mode"), has_overlay);
	obs_property_set_visible(obs_properties_get(props, "overlay_strength"), has_overlay);
	obs_property_set_visible(obs_properties_get(props, "overlay_gray"), has_overlay);
	return true;
}

obs_property_t *percent_slider(obs_properties_t *props, const char *name, const char *text, double min, double max)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(text), min, max, 1.0);
	obs_property_float_set_suffix(p, "%");
	return p;
}

obs_properties_t *get_properties(void *)
{
	const char *image_filter =
		"Images (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp *.avif);;All files (*.*)";
	obs_properties_t *props = obs_properties_create();

	// Picture
	obs_properties_t *picture = obs_properties_create();
	obs_property_t *base = obs_properties_add_list(picture, "base_type", obs_module_text("Ambient.BaseType"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(base, obs_module_text("Ambient.BaseType.Image"), (int)BaseType::Image);
	obs_property_list_add_int(base, obs_module_text("Ambient.BaseType.StartImage"), (int)BaseType::StartImage);
	obs_property_list_add_int(base, obs_module_text("Ambient.BaseType.Source"), (int)BaseType::Source);
	obs_property_set_modified_callback(base, visibility_modified);
	obs_properties_add_path(picture, "image", obs_module_text("Ambient.Image"), OBS_PATH_FILE, image_filter,
				nullptr);
	obs_property_t *base_source = obs_properties_add_list(picture, "base_source",
							      obs_module_text("Ambient.BaseSource"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_source_list(base_source, true);
	obs_property_t *live = obs_properties_add_list(picture, "live_mode", obs_module_text("Ambient.Live"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(live, obs_module_text("Ambient.Live.None"), (int)LiveMode::None);
	obs_property_list_add_int(live, obs_module_text("Ambient.Live.Auto"), (int)LiveMode::Auto);
	obs_property_list_add_int(live, obs_module_text("Ambient.Live.Source"), (int)LiveMode::Source);
	obs_property_set_modified_callback(live, visibility_modified);
	obs_property_t *live_source = obs_properties_add_list(picture, "live_source",
							      obs_module_text("Ambient.LiveSource"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_source_list(live_source, false);
	percent_slider(picture, "live_opacity", "Ambient.LiveOpacity", 0.0, 100.0);
	obs_property_t *live_fit = obs_properties_add_list(picture, "live_fit", obs_module_text("Ambient.LiveFit"),
							   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(live_fit, obs_module_text("Ambient.LiveFit.Fill"), (int)LiveFit::Fill);
	obs_property_list_add_int(live_fit, obs_module_text("Ambient.LiveFit.Fit"), (int)LiveFit::Fit);
	obs_property_list_add_int(live_fit, obs_module_text("Ambient.LiveFit.Width"), (int)LiveFit::Width);
	obs_property_list_add_int(live_fit, obs_module_text("Ambient.LiveFit.Height"), (int)LiveFit::Height);
	obs_property_list_add_int(live_fit, obs_module_text("Ambient.LiveFit.Stretch"), (int)LiveFit::Stretch);
	percent_slider(picture, "live_zoom", "Ambient.LiveZoom", 25.0, 400.0);
	obs_property_t *replace =
		obs_properties_add_bool(picture, "live_replace", obs_module_text("Ambient.LiveReplace"));
	obs_property_t *skip_standby =
		obs_properties_add_bool(picture, "live_skip_standby", obs_module_text("Ambient.LiveSkipStandby"));
	obs_property_set_long_description(skip_standby, obs_module_text("Ambient.LiveSkipStandby.Tip"));
	obs_property_t *ready = obs_properties_add_bool(picture, "live_ready", obs_module_text("Ambient.LiveReady"));
	obs_property_set_long_description(ready, obs_module_text("Ambient.LiveReady.Tip"));
	obs_property_set_long_description(replace, obs_module_text("Ambient.LiveReplace.Tip"));
	obs_properties_add_group(props, "picture_group", obs_module_text("Ambient.Group.Picture"), OBS_GROUP_NORMAL,
				 picture);

	// Motion
	obs_properties_t *motion = obs_properties_create();
	obs_property_t *sx = obs_properties_add_float_slider(motion, "speed_x", obs_module_text("Ambient.SpeedX"),
							     -300.0, 300.0, 1.0);
	obs_property_float_set_suffix(sx, " px/s");
	obs_property_t *sy = obs_properties_add_float_slider(motion, "speed_y", obs_module_text("Ambient.SpeedY"),
							     -300.0, 300.0, 1.0);
	obs_property_float_set_suffix(sy, " px/s");
	percent_slider(motion, "zoom", "Ambient.Zoom", 100.0, 400.0);
	obs_properties_add_bool(motion, "mirror", obs_module_text("Ambient.Mirror"));
	obs_properties_add_group(props, "motion_group", obs_module_text("Ambient.Group.Motion"), OBS_GROUP_NORMAL,
				 motion);

	// Look
	obs_properties_t *look = obs_properties_create();
	obs_property_t *blur_type = obs_properties_add_list(look, "blur_type", obs_module_text("RampBlur.Type"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(blur_type, obs_module_text("RampBlur.Type.Kawase"), (int)BlurKind::Kawase);
	obs_property_list_add_int(blur_type, obs_module_text("RampBlur.Type.Gaussian"), (int)BlurKind::Gaussian);
	obs_property_list_add_int(blur_type, obs_module_text("RampBlur.Type.Box"), (int)BlurKind::Box);
	obs_property_list_add_int(blur_type, obs_module_text("RampBlur.Type.Temporal"), (int)BlurKind::Temporal);
	obs_property_set_long_description(blur_type, obs_module_text("Ambient.BlurType.Tip"));
	obs_properties_add_float_slider(look, "blur", obs_module_text("Ambient.Blur"), 0.0, 1000.0, 1.0);
	obs_property_t *pixel = obs_properties_add_list(look, "pixel_style", obs_module_text("Ambient.Pixel"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pixel, obs_module_text("Ambient.Pixel.None"), (int)PixelStyle::None);
	obs_property_list_add_int(pixel, obs_module_text("Ambient.Pixel.Hex"), (int)PixelStyle::Hex);
	obs_property_list_add_int(pixel, obs_module_text("Ambient.Pixel.Square"), (int)PixelStyle::Square);
	obs_property_list_add_int(pixel, obs_module_text("Mosaic.Circle"), (int)PixelStyle::Circle);
	obs_property_list_add_int(pixel, obs_module_text("Mosaic.Triangle"), (int)PixelStyle::Triangle);
	obs_property_list_add_int(pixel, obs_module_text("Mosaic.Voronoi"), (int)PixelStyle::Voronoi);
	obs_property_list_add_int(pixel, obs_module_text("Mosaic.Rhomboid"), (int)PixelStyle::Rhomboid);
	obs_property_list_add_int(pixel, obs_module_text("Mosaic.Triakis"), (int)PixelStyle::Triakis);
	obs_property_set_modified_callback(pixel, visibility_modified);
	obs_property_t *cell = obs_properties_add_float_slider(look, "pixel_size", obs_module_text("Ambient.PixelSize"),
							       2.0, 200.0, 1.0);
	obs_property_float_set_suffix(cell, " px");
	obs_property_t *smooth = obs_properties_add_float_slider(look, "pixel_smooth", obs_module_text("Mosaic.Smooth"),
								 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(smooth, "%");
	obs_property_set_long_description(smooth, obs_module_text("Mosaic.Smooth.Tip"));
	obs_property_t *origin_x = obs_properties_add_float_slider(look, "pixel_origin_x",
								   obs_module_text("Mosaic.OriginX"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(origin_x, "%");
	obs_property_set_long_description(origin_x, obs_module_text("Mosaic.Origin.Tip"));
	obs_property_t *origin_y = obs_properties_add_float_slider(look, "pixel_origin_y",
								   obs_module_text("Mosaic.OriginY"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(origin_y, "%");
	obs_property_set_long_description(origin_y, obs_module_text("Mosaic.Origin.Tip"));
	obs_property_t *rotation = obs_properties_add_float_slider(
		look, "pixel_rotation", obs_module_text("Mosaic.Rotation"), -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(rotation, "°");
	percent_slider(look, "saturation", "Ambient.Saturation", 0.0, 300.0);
	percent_slider(look, "glow", "Ambient.Glow", 0.0, 100.0);
	obs_property_t *bloom =
		obs_properties_add_float_slider(look, "bloom", obs_module_text("Ambient.Bloom"), 0.0, 300.0, 1.0);
	obs_property_float_set_suffix(bloom, "%");
	obs_property_set_long_description(bloom, obs_module_text("Ambient.Bloom.Tip"));
	obs_property_set_modified_callback(bloom, visibility_modified);
	obs_property_t *bloom_from = obs_properties_add_float_slider(
		look, "bloom_threshold", obs_module_text("Ambient.BloomThreshold"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(bloom_from, "%");
	obs_property_set_long_description(bloom_from, obs_module_text("Ambient.BloomThreshold.Tip"));
	obs_property_t *bloom_size = obs_properties_add_float_slider(
		look, "bloom_size", obs_module_text("Ambient.BloomSize"), 0.0, 1000.0, 1.0);
	obs_property_set_long_description(bloom_size, obs_module_text("Ambient.BloomSize.Tip"));
	percent_slider(look, "brightness", "Ambient.Brightness", 0.0, 200.0);
	percent_slider(look, "vignette", "Ambient.Vignette", 0.0, 100.0);
	obs_properties_add_group(props, "look_group", obs_module_text("Ambient.Group.Look"), OBS_GROUP_NORMAL, look);

	// Texture overlay
	obs_properties_t *tex = obs_properties_create();
	obs_property_t *overlay = obs_properties_add_path(tex, "overlay", obs_module_text("Ambient.Overlay"),
							  OBS_PATH_FILE, image_filter, nullptr);
	obs_property_set_modified_callback(overlay, visibility_modified);
	obs_property_t *mode = obs_properties_add_list(tex, "overlay_mode", obs_module_text("Ambient.OverlayMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Ambient.OverlayMode.Multiply"), (int)OverlayMode::Multiply);
	obs_property_list_add_int(mode, obs_module_text("Ambient.OverlayMode.Screen"), (int)OverlayMode::Screen);
	percent_slider(tex, "overlay_strength", "Ambient.OverlayStrength", 0.0, 100.0);
	obs_properties_add_bool(tex, "overlay_gray", obs_module_text("Ambient.OverlayGray"));
	obs_properties_add_group(props, "overlay_group", obs_module_text("Ambient.Group.Overlay"), OBS_GROUP_NORMAL,
				 tex);

	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "base_type", (int)BaseType::StartImage);
	obs_data_set_default_int(settings, "live_mode", (int)LiveMode::None);
	obs_data_set_default_double(settings, "live_opacity", 70.0);
	obs_data_set_default_int(settings, "live_fit", (int)LiveFit::Fill);
	obs_data_set_default_double(settings, "live_zoom", 100.0);
	obs_data_set_default_bool(settings, "live_replace", false);
	obs_data_set_default_bool(settings, "live_ready", true);
	obs_data_set_default_bool(settings, "live_skip_standby", true);
	obs_data_set_default_double(settings, "speed_x", 30.0);
	obs_data_set_default_double(settings, "speed_y", 15.0);
	obs_data_set_default_double(settings, "zoom", 120.0);
	obs_data_set_default_bool(settings, "mirror", true);
	obs_data_set_default_double(settings, "blur", 60.0);
	obs_data_set_default_int(settings, "pixel_style", (int)PixelStyle::None);
	obs_data_set_default_double(settings, "pixel_size", 10.0);
	obs_data_set_default_int(settings, "blur_type", (int)BlurKind::Kawase);
	obs_data_set_default_double(settings, "pixel_smooth", 0.0);
	obs_data_set_default_double(settings, "pixel_origin_x", 50.0);
	obs_data_set_default_double(settings, "pixel_origin_y", 50.0);
	obs_data_set_default_double(settings, "pixel_rotation", 0.0);
	obs_data_set_default_double(settings, "bloom", 0.0);
	obs_data_set_default_double(settings, "bloom_threshold", 60.0);
	obs_data_set_default_double(settings, "bloom_size", 80.0);
	obs_data_set_default_double(settings, "saturation", 100.0);
	obs_data_set_default_double(settings, "glow", 0.0);
	obs_data_set_default_double(settings, "brightness", 100.0);
	obs_data_set_default_double(settings, "vignette", 0.0);
	obs_data_set_default_int(settings, "overlay_mode", (int)OverlayMode::Multiply);
	obs_data_set_default_double(settings, "overlay_strength", 100.0);
	obs_data_set_default_bool(settings, "overlay_gray", true);
}

} // namespace

void register_ambient_background()
{
	obs_source_info info = {};
	info.id = kAmbientBackgroundId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_IMAGE;
	info.get_name = [](void *) {
		return obs_module_text("Ambient.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AmbientBackground(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AmbientBackground *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AmbientBackground *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<AmbientBackground *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<AmbientBackground *>(data)->height();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AmbientBackground *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AmbientBackground *>(data)->render();
	};
	info.enum_active_sources = [](void *data, obs_source_enum_proc_t cb, void *param) {
		static_cast<AmbientBackground *>(data)->enum_children(cb, param);
	};
	register_timed(&info);
}

} // namespace stagekit
