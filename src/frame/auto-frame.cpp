/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Auto Frame: gives a capture a fixed-size frame and fits whatever the
// capture currently outputs inside it.
//
// A game or emulator capture changes size whenever the program changes
// resolution, which normally means one hand-built scene per picture shape
// plus automation to switch between them. With this filter the source's
// size never changes, so its place in the scene, its shadow and anything
// linked to it stay put, while the picture inside glides to its new fit.

#include "auto-frame.hpp"

#include "../common/easing.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

enum class FitMode : int {
	Fit = 0,     // whole picture visible, bars where the shapes differ
	Fill = 1,    // frame covered, picture cropped where the shapes differ
	Whole = 2,   // largest whole-number scale that fits (crisp pixel art)
	Stretch = 3, // picture stretched to the frame
};

enum class Shape : int {
	Source = 0, // use the capture's own width : height
	R4x3 = 1,
	R16x9 = 2,
	R8x7 = 3,
	R3x2 = 4,
	R1x1 = 5,
	R5x4 = 6,
	R16x10 = 7,
	R10x9 = 8,
	Custom = 9,
};

// Removing black bars a game or emulator draws around its picture.
enum class Trim : int {
	Off = 0,
	Automatic = 1,
	Manual = 2,
};

struct Settings {
	uint32_t frame_w = 1920;
	uint32_t frame_h = 1080;
	FitMode fit = FitMode::Fit;
	Shape shape = Shape::Source;
	double custom_w = 4.0;
	double custom_h = 3.0;
	int align_x = 1; // 0 left, 1 centre, 2 right
	int align_y = 1; // 0 top, 1 centre, 2 bottom
	bool sharp = false;
	Trim trim = Trim::Automatic;
	double trim_x = 0.0; // manual: pixels off the left and off the right
	double trim_y = 0.0; // manual: pixels off the top and off the bottom
	double trim_wait_s = 3.0;
	// Automatic trimming can be told the picture's shape instead of looking
	// for it: by a Picture Shape key (force_ratio), or by a rule naming the
	// program being captured (rules: text found in the program -> shape).
	double force_ratio = 0.0;
	std::vector<std::pair<std::string, double>> rules;
	double glide_s = 0.35;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

struct Rect {
	double x = 0, y = 0, w = 0, h = 0;
};

double shape_ratio(const Settings &s, uint32_t w, uint32_t h)
{
	switch (s.shape) {
	case Shape::R4x3:
		return 4.0 / 3.0;
	case Shape::R16x9:
		return 16.0 / 9.0;
	case Shape::R8x7:
		return 8.0 / 7.0;
	case Shape::R3x2:
		return 3.0 / 2.0;
	case Shape::R1x1:
		return 1.0;
	case Shape::R5x4:
		return 5.0 / 4.0;
	case Shape::R16x10:
		return 16.0 / 10.0;
	case Shape::R10x9:
		return 10.0 / 9.0;
	case Shape::Custom:
		if (s.custom_w > 0.0 && s.custom_h > 0.0)
			return s.custom_w / s.custom_h;
		break;
	case Shape::Source:
		break;
	}
	return (double)w / (double)h;
}

// Where the picture goes inside the frame.
Rect target_rect(const Settings &s, uint32_t w, uint32_t h)
{
	const double fw = s.frame_w, fh = s.frame_h;
	const double ratio = shape_ratio(s, w, h);
	// The picture's size in display pixels at 1x: its height, and the
	// width its shape calls for.
	const double base_h = h;
	const double base_w = h * ratio;

	Rect r;
	switch (s.fit) {
	case FitMode::Stretch:
		r.w = fw;
		r.h = fh;
		break;
	case FitMode::Fill: {
		const double k = std::max(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Whole: {
		const double k = std::max(1.0, std::floor(std::min(fw / base_w, fh / base_h)));
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	case FitMode::Fit:
	default: {
		const double k = std::min(fw / base_w, fh / base_h);
		r.w = base_w * k;
		r.h = base_h * k;
		break;
	}
	}
	r.x = (fw - r.w) * (s.align_x * 0.5);
	r.y = (fh - r.h) * (s.align_y * 0.5);
	return r;
}

bool same(const Rect &a, const Rect &b)
{
	return std::fabs(a.x - b.x) < 0.01 && std::fabs(a.y - b.y) < 0.01 && std::fabs(a.w - b.w) < 0.01 &&
	       std::fabs(a.h - b.h) < 0.01;
}

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

// "4:3", "64:49" or "1.306" -> a width / height ratio, or 0.
double parse_ratio(const std::string &text)
{
	try {
		const size_t colon = text.find(':');
		if (colon == std::string::npos)
			return std::max(0.0, std::stod(text));
		const double a = std::stod(text.substr(0, colon)), b = std::stod(text.substr(colon + 1));
		return a > 0.0 && b > 0.0 ? a / b : 0.0;
	} catch (...) {
		return 0.0;
	}
}

// A rule is written "snes9x = 4:3": text to look for in the captured
// program's name or window title, then the shape of its picture.
bool parse_rule(const std::string &line, std::string &program, double &ratio)
{
	const size_t equals = line.rfind('=');
	if (equals == std::string::npos)
		return false;
	auto trimmed = [](std::string t) {
		const char *space = " \t";
		const size_t a = t.find_first_not_of(space), b = t.find_last_not_of(space);
		return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
	};
	program = lower(trimmed(line.substr(0, equals)));
	ratio = parse_ratio(trimmed(line.substr(equals + 1)));
	return !program.empty() && ratio > 0.0;
}

class AutoFrame {
public:
	AutoFrame(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		obs_enter_graphics();
		tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_frame_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		tr_probe_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_sampler_info point = {};
		point.filter = GS_FILTER_POINT;
		point.address_u = GS_ADDRESS_CLAMP;
		point.address_v = GS_ADDRESS_CLAMP;
		point.address_w = GS_ADDRESS_CLAMP;
		point.max_anisotropy = 1;
		point_sampler_ = gs_samplerstate_create(&point);
		obs_leave_graphics();
		update(settings);

		proc_handler_add(
			obs_source_get_proc_handler(source_),
			"void get_picture(out float x, out float y, out float width, out float height)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoFrame *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_float(cd, "x", self->current_.x);
				calldata_set_float(cd, "y", self->current_.y);
				calldata_set_float(cd, "width", self->current_.w);
				calldata_set_float(cd, "height", self->current_.h);
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_picture_shape(in float ratio)",
			[](void *data, calldata_t *cd) {
				static_cast<AutoFrame *>(data)->set_forced_ratio(calldata_float(cd, "ratio"));
			},
			this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void measure_now()",
			[](void *data, calldata_t *) { static_cast<AutoFrame *>(data)->measure_now(); }, this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void get_trim(out float x, out float y)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<AutoFrame *>(data);
				std::lock_guard lock(self->mutex_);
				calldata_set_float(cd, "x", self->crop_goal_.x);
				calldata_set_float(cd, "y", self->crop_goal_.y);
			},
			this);
	}

	~AutoFrame()
	{
		obs_enter_graphics();
		gs_texrender_destroy(tr_input_);
		gs_texrender_destroy(tr_frame_);
		gs_texrender_destroy(tr_probe_);
		gs_stagesurface_destroy(stage_[0]);
		gs_stagesurface_destroy(stage_[1]);
		gs_samplerstate_destroy(point_sampler_);
		obs_leave_graphics();
	}

	void update(obs_data_t *settings)
	{
		Settings s;
		s.frame_w = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_width"), 2, 16384);
		s.frame_h = (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "frame_height"), 2, 16384);
		s.fit = static_cast<FitMode>(obs_data_get_int(settings, "fit"));
		s.shape = static_cast<Shape>(obs_data_get_int(settings, "shape"));
		s.custom_w = obs_data_get_double(settings, "custom_w");
		s.custom_h = obs_data_get_double(settings, "custom_h");
		s.align_x = (int)std::clamp<long long>(obs_data_get_int(settings, "align_x"), 0, 2);
		s.align_y = (int)std::clamp<long long>(obs_data_get_int(settings, "align_y"), 0, 2);
		s.sharp = obs_data_get_bool(settings, "sharp");
		s.trim = static_cast<Trim>(obs_data_get_int(settings, "trim"));
		s.trim_x = std::max(0.0, obs_data_get_double(settings, "trim_x"));
		s.trim_y = std::max(0.0, obs_data_get_double(settings, "trim_y"));
		s.trim_wait_s = std::max(0.0, obs_data_get_double(settings, "trim_wait"));
		s.force_ratio = std::max(0.0, obs_data_get_double(settings, "force_ratio"));
		obs_data_array_t *rules = obs_data_get_array(settings, "rules");
		for (size_t i = 0; i < obs_data_array_count(rules); i++) {
			obs_data_t *entry = obs_data_array_item(rules, i);
			std::string program;
			double ratio = 0.0;
			if (parse_rule(obs_data_get_string(entry, "value"), program, ratio))
				s.rules.emplace_back(program, ratio);
			obs_data_release(entry);
		}
		obs_data_array_release(rules);
		s.glide_s = std::max<long long>(0, obs_data_get_int(settings, "glide_ms")) / 1000.0;
		s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
		s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
		std::lock_guard lock(mutex_);
		settings_ = s;
	}

	void tick(float seconds)
	{
		// Once a second, see which program is being captured and whether a
		// rule gives its picture shape.
		identity_timer_ -= seconds;
		double rule_ratio = -1.0;
		if (identity_timer_ <= 0.0) {
			identity_timer_ = 1.0;
			const std::string program = captured_program();
			std::lock_guard lock(mutex_);
			rule_ratio = 0.0;
			for (const auto &[text, ratio] : settings_.rules) {
				if (!program.empty() && program.find(text) != std::string::npos) {
					rule_ratio = ratio;
					break;
				}
			}
		}
		std::lock_guard lock(mutex_);
		pending_time_ += seconds;
		if (rule_ratio >= 0.0)
			rule_ratio_ = rule_ratio;
	}

	// The shape a Picture Shape key asked for; 0 goes back to automatic.
	void set_forced_ratio(double ratio)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		obs_data_set_double(settings, "force_ratio", std::max(0.0, ratio));
		obs_source_update(source_, settings);
		obs_data_release(settings);
	}

	obs_source_t *source() const { return source_; }

	// Lower-case text describing the program a capture source is showing:
	// what a game or window capture reports it is attached to, plus its
	// "window" setting (title:class:executable).
	std::string captured_program()
	{
		obs_source_t *parent = obs_filter_get_parent(source_);
		if (!parent)
			return {};
		std::string text;
		calldata_t cd = {};
		if (proc_handler_call(obs_source_get_proc_handler(parent), "get_hooked", &cd)) {
			for (const char *key : {"executable", "title"}) {
				const char *value = nullptr;
				if (calldata_get_string(&cd, key, &value) && value)
					text += std::string(value) + " ";
			}
		}
		calldata_free(&cd);
		obs_data_t *settings = obs_source_get_settings(parent);
		text += obs_data_get_string(settings, "window");
		obs_data_release(settings);
		return lower(text);
	}

	// Asks for the bars to be measured from the picture as it is right now
	// and stored as a fixed trim.
	void measure_now()
	{
		std::lock_guard lock(mutex_);
		measure_once_[0] = measure_once_[1] = true;
		measured_ready_ = false;
	}

	uint32_t width()
	{
		std::lock_guard lock(mutex_);
		return settings_.frame_w;
	}
	uint32_t height()
	{
		std::lock_guard lock(mutex_);
		return settings_.frame_h;
	}

	void render()
	{
		obs_source_t *target = obs_filter_get_target(source_);
		const uint32_t w = target ? obs_source_get_base_width(target) : 0;
		const uint32_t h = target ? obs_source_get_base_height(target) : 0;
		if (!w || !h || !obs_filter_get_parent(source_))
			return; // nothing to show yet: the frame stays empty

		Settings s;
		Rect rect, crop;
		double dt;
		bool probe_now = false, publish_measured = false;
		double measured_x = 0.0, measured_y = 0.0;
		{
			std::lock_guard lock(mutex_);
			s = settings_;
			dt = pending_time_;
			pending_time_ = 0.0;

			if (w != last_w_ || h != last_h_) {
				// A new resolution: forget the bars found at the old one.
				last_w_ = w;
				last_h_ = h;
				bars_[0] = bars_[1] = Bars();
			}
			// How much to take off each side.
			double trim_x = 0.0, trim_y = 0.0;
			if (s.trim == Trim::Manual) {
				trim_x = s.trim_x;
				trim_y = s.trim_y;
			} else if (s.trim == Trim::Automatic) {
				trim_x = bars_[0].applied;
				trim_y = bars_[1].applied;
			}
			// A known shape beats looking: centre that shape in the capture.
			const double known = s.force_ratio > 0.0 ? s.force_ratio : rule_ratio_;
			const bool shape_known = s.trim == Trim::Automatic && known > 0.0;
			if (shape_known) {
				trim_x = trim_y = 0.0;
				if ((double)w / (double)h > known)
					trim_x = (w - h * known) / 2.0;
				else
					trim_y = (h - w / known) / 2.0;
			}
			if ((s.trim == Trim::Automatic && !shape_known) || measure_once_[0] || measure_once_[1]) {
				probe_timer_ -= dt;
				if (probe_timer_ <= 0.0) {
					// While measuring on request, look every frame to finish at once.
					probe_timer_ = measure_once_[0] || measure_once_[1] ? 0.0 : kProbeInterval;
					probe_now = true;
				}
			}
			publish_measured = measured_ready_;
			measured_ready_ = false;
			measured_x = measured_[0];
			measured_y = measured_[1];
			trim_x = std::clamp(trim_x, 0.0, w * 0.45);
			trim_y = std::clamp(trim_y, 0.0, h * 0.45);
			const Rect crop_goal{trim_x, trim_y, w - 2.0 * trim_x, h - 2.0 * trim_y};
			const Rect goal =
				target_rect(s, (uint32_t)std::lround(crop_goal.w), (uint32_t)std::lround(crop_goal.h));

			if (!have_rect_) {
				// First picture: no glide, just be in the right place.
				current_ = from_ = goal_ = goal;
				crop_current_ = crop_from_ = crop_goal_ = crop_goal;
				have_rect_ = true;
				elapsed_ = duration_ = 0.0;
			} else if (!same(goal, goal_) || !same(crop_goal, crop_goal_)) {
				from_ = current_;
				goal_ = goal;
				crop_from_ = crop_current_;
				crop_goal_ = crop_goal;
				elapsed_ = 0.0;
				duration_ = s.glide_s;
			}
			if (elapsed_ < duration_) {
				elapsed_ = std::min(duration_, elapsed_ + dt);
				const double t = ease(s.curve, s.mode, elapsed_ / duration_);
				auto mix = [t](const Rect &a, const Rect &b) {
					return Rect{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t,
						    a.h + (b.h - a.h) * t};
				};
				current_ = mix(from_, goal_);
				crop_current_ = mix(crop_from_, crop_goal_);
			} else {
				current_ = goal_;
				crop_current_ = crop_goal_;
			}
			rect = current_;
			crop = crop_current_;
		}

		if (publish_measured) {
			// Store what was measured as a fixed trim, where the user can see and edit it.
			obs_data_t *settings = obs_source_get_settings(source_);
			obs_data_set_int(settings, "trim", (int)Trim::Manual);
			obs_data_set_double(settings, "trim_x", measured_x);
			obs_data_set_double(settings, "trim_y", measured_y);
			obs_source_update(source_, settings);
			obs_data_release(settings);
		}

		// 1. The capture as it is.
		gs_texrender_reset(tr_input_);
		if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
			return;
		if (!gs_texrender_begin(tr_input_, w, h))
			return;
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
		gs_blend_state_pop();
		gs_texrender_end(tr_input_);
		gs_texture_t *input = gs_texrender_get_texture(tr_input_);
		if (!input)
			return;

		gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

		if (probe_now)
			look_for_bars(input, w, h, effect, image, s.trim_wait_s);

		// 2. Placed inside the frame. Drawing into a frame-sized target also
		// crops anything a "fill" fit pushes past the edges.
		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_texrender_reset(tr_frame_);
		if (gs_texrender_begin(tr_frame_, s.frame_w, s.frame_h)) {
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)s.frame_w, 0.0f, (float)s.frame_h, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(image, input);
			if (s.sharp)
				gs_effect_set_next_sampler(image, point_sampler_);
			gs_matrix_push();
			gs_matrix_translate3f((float)rect.x, (float)rect.y, 0.0f);
			// Only the part of the capture inside the bars is drawn.
			const uint32_t cx = (uint32_t)std::clamp(std::lround(crop.x), 0L, (long)w - 1);
			const uint32_t cy = (uint32_t)std::clamp(std::lround(crop.y), 0L, (long)h - 1);
			const uint32_t cw = (uint32_t)std::clamp(std::lround(crop.w), 1L, (long)(w - cx));
			const uint32_t ch = (uint32_t)std::clamp(std::lround(crop.h), 1L, (long)(h - cy));
			gs_matrix_scale3f((float)(rect.w / cw), (float)(rect.h / ch), 1.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite_subregion(input, 0, cx, cy, cw, ch);
			gs_matrix_pop();
			gs_blend_state_pop();
			gs_texrender_end(tr_frame_);
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		// 3. The finished frame.
		if (gs_texture_t *frame = gs_texrender_get_texture(tr_frame_)) {
			const bool linear = gs_get_linear_srgb();
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(linear);
			if (linear)
				gs_effect_set_texture_srgb(image, frame);
			else
				gs_effect_set_texture(image, frame);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(frame, 0, s.frame_w, s.frame_h);
			gs_enable_framebuffer_srgb(previous);
		}
	}

private:
	static constexpr double kProbeInterval = 0.4;

	// What has been seen of the bars along one direction (0: left/right,
	// 1: top/bottom), and how much is currently trimmed.
	struct Bars {
		double applied = 0.0;    // pixels trimmed from each side
		double candidate = -1.0; // what the last look suggested
		double stable_for = 0.0; // how long the candidate has held
		int agree = 0;           // looks in a row that agreed
	};

	// Looks at the capture for black bars. One direction per call, to keep
	// the cost down: a thin copy of the picture is read back and scanned.
	// Bars must stay the same for a while before more is trimmed, so a dark
	// moment in the game does not zoom the picture in; when the picture
	// grows into the bars the trim is released quickly.
	void look_for_bars(gs_texture_t *input, uint32_t w, uint32_t h, gs_effect_t *effect, gs_eparam_t *image,
			   double wait_s)
	{
		const int axis = probe_axis_;
		probe_axis_ = 1 - probe_axis_;
		// Full resolution along the direction being measured, coarse across it.
		const uint32_t pw = axis == 0 ? w : std::min(w, 160u);
		const uint32_t ph = axis == 0 ? std::min(h, 90u) : h;

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_texrender_reset(tr_probe_);
		if (gs_texrender_begin(tr_probe_, pw, ph)) {
			vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)pw, 0.0f, (float)ph, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			gs_effect_set_texture(image, input);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(input, 0, pw, ph);
			gs_blend_state_pop();
			gs_texrender_end(tr_probe_);
		}
		gs_enable_framebuffer_srgb(previous_srgb);

		gs_stagesurf_t *&stage = stage_[axis];
		if (!stage || gs_stagesurface_get_width(stage) != pw || gs_stagesurface_get_height(stage) != ph) {
			gs_stagesurface_destroy(stage);
			stage = gs_stagesurface_create(pw, ph, GS_RGBA);
		}
		gs_texture_t *probe = gs_texrender_get_texture(tr_probe_);
		if (!stage || !probe)
			return;
		gs_stage_texture(stage, probe);
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (!gs_stagesurface_map(stage, &data, &linesize))
			return;

		// A line is "bar" when nothing along it is brighter than near-black.
		const uint32_t lines = axis == 0 ? pw : ph;
		const uint32_t across = axis == 0 ? ph : pw;
		auto is_bar = [&](uint32_t line) {
			for (uint32_t i = 0; i < across; i++) {
				const uint32_t x = axis == 0 ? line : i, y = axis == 0 ? i : line;
				const uint8_t *px = data + (size_t)y * linesize + (size_t)x * 4;
				if (px[3] > 16 && (px[0] > 16 || px[1] > 16 || px[2] > 16))
					return false;
			}
			return true;
		};
		uint32_t first = 0, last = lines;
		while (first < lines && is_bar(first))
			first++;
		while (last > first && is_bar(last - 1))
			last--;
		gs_stagesurface_unmap(stage);

		std::lock_guard lock(mutex_);
		Bars &bars = bars_[axis];
		if (first >= lines) {
			// Entirely black (a fade, a loading screen): learn nothing from it.
			bars.candidate = -1.0;
			bars.agree = 0;
			bars.stable_for = 0.0;
			return;
		}
		// Bars are trimmed evenly, by the smaller side, so a dark edge in the
		// picture itself is never mistaken for more bar.
		const double seen = std::min<double>(first, lines - last);
		if (bars.candidate >= 0.0 && std::fabs(seen - bars.candidate) <= 2.0) {
			bars.agree++;
			bars.stable_for += kProbeInterval * 2.0; // each direction is looked at every other time
		} else {
			bars.candidate = seen;
			bars.agree = 1;
			bars.stable_for = 0.0;
		}
		// "Trim to what is on screen now": take this look as it is.
		if (measure_once_[axis]) {
			measure_once_[axis] = false;
			measured_[axis] = seen;
			if (!measure_once_[0] && !measure_once_[1])
				measured_ready_ = true;
		}

		const bool less_bar = seen < bars.applied - 2.0;
		const bool more_bar = seen > bars.applied + 2.0;
		if (less_bar && bars.agree >= 2) {
			// The picture reaches further out than thought: bars are never lit.
			bars.applied = seen;
		} else if (more_bar && bars.stable_for >= wait_s) {
			// A game with a black background looks just like bars. Real bars
			// leave a picture in one of the shapes consoles and emulators
			// output; a dark scene leaves an arbitrary one. Only trim more
			// when what would be left is exactly such a shape.
			const double other = bars_[1 - axis].applied;
			const double pic_w = w - 2.0 * (axis == 0 ? seen : other);
			const double pic_h = h - 2.0 * (axis == 1 ? seen : other);
			if (is_picture_shape(pic_w, pic_h))
				bars.applied = seen;
		}
	}

	// Whether a picture size is one of the shapes games and emulators output,
	// to within a few pixels.
	static bool is_picture_shape(double pic_w, double pic_h)
	{
		// 64:49 is SNES/NES pixels (8:7 each) shown on a 4:3 screen.
		static const double shapes[] = {16.0 / 9.0, 16.0 / 10.0, 3.0 / 2.0,  4.0 / 3.0,   64.0 / 49.0,
						5.0 / 4.0,  8.0 / 7.0,   10.0 / 9.0, 16.0 / 15.0, 1.0};
		if (pic_w < 16.0 || pic_h < 16.0)
			return false;
		const double tolerance = std::max(4.0, pic_h * 0.004);
		for (double shape : shapes)
			if (std::fabs(pic_w - pic_h * shape) <= tolerance)
				return true;
		return false;
	}

	obs_source_t *source_;
	std::mutex mutex_;
	Settings settings_;
	bool have_rect_ = false;
	Rect current_, from_, goal_;
	Rect crop_current_, crop_from_, crop_goal_;
	uint32_t last_w_ = 0, last_h_ = 0;
	Bars bars_[2];
	double probe_timer_ = 0.0;
	bool measure_once_[2] = {false, false};
	double measured_[2] = {0.0, 0.0};
	double identity_timer_ = 0.0;
	double rule_ratio_ = 0.0;
	bool measured_ready_ = false;
	int probe_axis_ = 0;
	gs_texrender_t *tr_probe_ = nullptr;
	gs_stagesurf_t *stage_[2] = {nullptr, nullptr};
	double elapsed_ = 0.0, duration_ = 0.0, pending_time_ = 0.0;
	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_frame_ = nullptr;
	gs_samplerstate_t *point_sampler_ = nullptr;
};

bool shape_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool custom = static_cast<Shape>(obs_data_get_int(settings, "shape")) == Shape::Custom;
	obs_property_set_visible(obs_properties_get(props, "custom_w"), custom);
	obs_property_set_visible(obs_properties_get(props, "custom_h"), custom);
	const auto trim = static_cast<Trim>(obs_data_get_int(settings, "trim"));
	obs_property_set_visible(obs_properties_get(props, "trim_wait"), trim == Trim::Automatic);
	obs_property_set_visible(obs_properties_get(props, "rules"), trim == Trim::Automatic);
	obs_property_set_visible(obs_properties_get(props, "trim_x"), trim == Trim::Manual);
	obs_property_set_visible(obs_properties_get(props, "trim_y"), trim == Trim::Manual);
	return true;
}

// Refills the "copy from" item list for the chosen scene.
bool copy_scene_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	obs_property_t *list = obs_properties_get(props, "copy_item");
	obs_property_list_clear(list);
	obs_source_t *scene = obs_get_source_by_name(obs_data_get_string(settings, "copy_scene"));
	add_scene_items_to_list(scene, list);
	obs_source_release(scene);
	return true;
}

// Sets the frame to the size an existing item takes up in its scene.
bool copy_frame_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *filter = static_cast<obs_source_t *>(data);
	obs_data_t *settings = obs_source_get_settings(filter);
	obs_source_t *scene = obs_get_source_by_name(obs_data_get_string(settings, "copy_scene"));
	obs_sceneitem_t *item = find_item(scene, obs_data_get_string(settings, "copy_item"));
	bool changed = false;
	if (item) {
		refresh_item(item);
		const Box box = scene_box(scene, item);
		if (box.w >= 2.0 && box.h >= 2.0) {
			obs_data_set_int(settings, "frame_width", std::lround(box.w));
			obs_data_set_int(settings, "frame_height", std::lround(box.h));
			obs_source_update(filter, settings);
			changed = true;
		}
	}
	obs_source_release(scene);
	obs_data_release(settings);
	return changed;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "frame_width", obs_module_text("Frame.Width"), 2, 16384, 1);
	obs_properties_add_int(props, "frame_height", obs_module_text("Frame.Height"), 2, 16384, 1);

	// Take the size from something already laid out, instead of typing it.
	obs_properties_t *copy = obs_properties_create();
	obs_property_t *copy_scene = obs_properties_add_list(copy, "copy_scene", obs_module_text("Frame.CopyScene"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(copy_scene, "", "");
	obs_enum_scenes(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		copy_scene);
	obs_property_set_modified_callback(copy_scene, copy_scene_modified);
	obs_properties_add_list(copy, "copy_item", obs_module_text("Frame.CopyItem"), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
	obs_properties_add_button2(copy, "copy_frame", obs_module_text("Frame.CopyButton"), copy_frame_clicked,
				   data ? static_cast<AutoFrame *>(data)->source() : nullptr);
	obs_properties_add_group(props, "copy_group", obs_module_text("Frame.CopyGroup"), OBS_GROUP_NORMAL, copy);

	obs_property_t *fit = obs_properties_add_list(props, "fit", obs_module_text("Frame.Fit"), OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fit"), (int)FitMode::Fit);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Whole"), (int)FitMode::Whole);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Fill"), (int)FitMode::Fill);
	obs_property_list_add_int(fit, obs_module_text("Frame.Fit.Stretch"), (int)FitMode::Stretch);

	obs_property_t *shape = obs_properties_add_list(props, "shape", obs_module_text("Frame.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Source"), (int)Shape::Source);
	struct {
		Shape value;
		const char *text;
	} shapes[] = {{Shape::R4x3, "4:3"},   {Shape::R16x9, "16:9"},   {Shape::R8x7, "8:7"}, {Shape::R3x2, "3:2"},
		      {Shape::R10x9, "10:9"}, {Shape::R16x10, "16:10"}, {Shape::R5x4, "5:4"}, {Shape::R1x1, "1:1"}};
	for (const auto &entry : shapes)
		obs_property_list_add_int(shape, entry.text, (int)entry.value);
	obs_property_list_add_int(shape, obs_module_text("Frame.Shape.Custom"), (int)Shape::Custom);
	obs_property_set_modified_callback(shape, shape_modified);
	obs_properties_add_float(props, "custom_w", obs_module_text("Frame.CustomW"), 0.01, 1000.0, 0.01);
	obs_properties_add_float(props, "custom_h", obs_module_text("Frame.CustomH"), 0.01, 1000.0, 0.01);

	obs_property_t *ax = obs_properties_add_list(props, "align_x", obs_module_text("Frame.AlignX"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ax, obs_module_text("Frame.Left"), 0);
	obs_property_list_add_int(ax, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ax, obs_module_text("Frame.Right"), 2);
	obs_property_t *ay = obs_properties_add_list(props, "align_y", obs_module_text("Frame.AlignY"),
						     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(ay, obs_module_text("Frame.Top"), 0);
	obs_property_list_add_int(ay, obs_module_text("Frame.Center"), 1);
	obs_property_list_add_int(ay, obs_module_text("Frame.Bottom"), 2);

	obs_property_t *trim = obs_properties_add_list(props, "trim", obs_module_text("Frame.Trim"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Automatic"), (int)Trim::Automatic);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Manual"), (int)Trim::Manual);
	obs_property_list_add_int(trim, obs_module_text("Frame.Trim.Off"), (int)Trim::Off);
	obs_property_set_modified_callback(trim, shape_modified);
	obs_properties_add_button2(
		props, "trim_now", obs_module_text("Frame.TrimNow"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			if (d)
				static_cast<AutoFrame *>(d)->measure_now();
			return false;
		},
		data);
	obs_properties_add_editable_list(props, "rules", obs_module_text("Frame.Rules"), OBS_EDITABLE_LIST_TYPE_STRINGS,
					 nullptr, nullptr);
	obs_property_t *wait =
		obs_properties_add_float_slider(props, "trim_wait", obs_module_text("Frame.TrimWait"), 0.0, 15.0, 0.5);
	obs_property_float_set_suffix(wait, " s");
	obs_property_t *tx =
		obs_properties_add_float(props, "trim_x", obs_module_text("Frame.TrimX"), 0.0, 8000.0, 1.0);
	obs_property_float_set_suffix(tx, " px");
	obs_property_t *ty =
		obs_properties_add_float(props, "trim_y", obs_module_text("Frame.TrimY"), 0.0, 8000.0, 1.0);
	obs_property_float_set_suffix(ty, " px");

	obs_properties_add_bool(props, "sharp", obs_module_text("Frame.Sharp"));
	obs_property_t *glide =
		obs_properties_add_int_slider(props, "glide_ms", obs_module_text("Frame.Glide"), 0, 5000, 50);
	obs_property_int_set_suffix(glide, " ms");
	add_easing_properties(props, "");

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Frame.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "frame_width", have ? ovi.base_width : 1920);
	obs_data_set_default_int(settings, "frame_height", have ? ovi.base_height : 1080);
	obs_data_set_default_int(settings, "fit", (int)FitMode::Fit);
	obs_data_set_default_int(settings, "shape", (int)Shape::Source);
	obs_data_set_default_double(settings, "custom_w", 4.0);
	obs_data_set_default_double(settings, "custom_h", 3.0);
	obs_data_set_default_int(settings, "align_x", 1);
	obs_data_set_default_int(settings, "align_y", 1);
	obs_data_set_default_bool(settings, "sharp", false);
	obs_data_set_default_int(settings, "trim", (int)Trim::Automatic);
	obs_data_set_default_double(settings, "trim_wait", 3.0);
	obs_data_set_default_double(settings, "trim_x", 0.0);
	obs_data_set_default_double(settings, "trim_y", 0.0);
	obs_data_set_default_double(settings, "force_ratio", 0.0);
	obs_data_set_default_int(settings, "glide_ms", 350);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
}

// ============================================================ Picture Shape
// A key for Auto Frame: switching this filter on tells the Auto Frame on the
// same source which shape the picture has (or to go back to working it out),
// then it switches itself off again. Point a Stream Deck Filter key at it.

thread_local bool t_shape_self_change = false;

class PictureShape {
public:
	PictureShape(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void trigger()",
			[](void *data, calldata_t *) { static_cast<PictureShape *>(data)->pending_ = true; }, this);
	}

	~PictureShape()
	{
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);
		detach();
	}

	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		const std::string shape = obs_data_get_string(settings, "shape");
		ratio_ = shape == "custom" ? parse_ratio(obs_data_get_string(settings, "custom")) : parse_ratio(shape);
	}

	void tick()
	{
		if (!ready_) {
			quiet_enable(false); // a key saved in the "on" state must not fire at start-up
			ready_ = true;
			return;
		}
		if (!pending_.exchange(false))
			return;
		double ratio;
		{
			std::lock_guard lock(mutex_);
			ratio = ratio_;
		}
		if (parent_) {
			obs_source_enum_filters(
				parent_,
				[](obs_source_t *, obs_source_t *filter, void *param) {
					const char *id = obs_source_get_unversioned_id(filter);
					if (id && strcmp(id, kAutoFrameId) == 0) {
						calldata_t cd = {};
						calldata_set_float(&cd, "ratio", *static_cast<double *>(param));
						proc_handler_call(obs_source_get_proc_handler(filter),
								  "set_picture_shape", &cd);
						calldata_free(&cd);
					}
				},
				&ratio);
		}
		quiet_enable(false);
	}

	void attach(obs_source_t *parent)
	{
		detach();
		parent_ = parent;
		const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";
		std::string text = obs_module_text("Hotkey.PictureShape");
		const size_t at = text.find("%1");
		if (at != std::string::npos)
			text.replace(at, 2, name);
		hotkey_ = obs_hotkey_register_source(
			parent, ("stagekit.shape." + name).c_str(), text.c_str(),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				if (pressed)
					static_cast<PictureShape *>(data)->pending_ = true;
			},
			this);
	}

	void detach()
	{
		if (hotkey_ != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(hotkey_);
		hotkey_ = OBS_INVALID_HOTKEY_ID;
		parent_ = nullptr;
	}

	obs_source_t *source() const { return source_; }

private:
	void quiet_enable(bool enabled)
	{
		if (obs_source_enabled(source_) == enabled)
			return;
		t_shape_self_change = true;
		obs_source_set_enabled(source_, enabled);
		t_shape_self_change = false;
	}

	static void enable_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<PictureShape *>(data);
		if (!t_shape_self_change && self->ready_ && calldata_bool(cd, "enabled"))
			self->pending_ = true;
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	double ratio_ = 0.0;
	std::atomic<bool> pending_{false};
	std::atomic<bool> ready_{false};
	obs_hotkey_id hotkey_ = OBS_INVALID_HOTKEY_ID;
};

obs_properties_t *picture_shape_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *shape = obs_properties_add_list(props, "shape", obs_module_text("Shape.Shape"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(shape, obs_module_text("Shape.Automatic"), "0");
	const char *shapes[][2] = {{"4:3", "4:3"},     {"Shape.Snes", "64:49"}, {"3:2", "3:2"}, {"8:7", "8:7"},
				   {"10:9", "10:9"},   {"16:15", "16:15"},      {"5:4", "5:4"}, {"1:1", "1:1"},
				   {"16:10", "16:10"}, {"16:9", "16:9"}};
	for (const auto &entry : shapes)
		obs_property_list_add_string(
			shape, strncmp(entry[0], "Shape.", 6) == 0 ? obs_module_text(entry[0]) : entry[0], entry[1]);
	obs_property_list_add_string(shape, obs_module_text("Frame.Shape.Custom"), "custom");
	obs_property_set_modified_callback(shape, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		obs_property_set_visible(obs_properties_get(p, "custom"),
					 strcmp(obs_data_get_string(settings, "shape"), "custom") == 0);
		return true;
	});
	obs_properties_add_text(props, "custom", obs_module_text("Shape.Custom"), OBS_TEXT_DEFAULT);
	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Shape.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_auto_frame()
{
	obs_source_info info = {};
	info.id = kAutoFrameId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Frame.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new AutoFrame(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<AutoFrame *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<AutoFrame *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.get_width = [](void *data) {
		return static_cast<AutoFrame *>(data)->width();
	};
	info.get_height = [](void *data) {
		return static_cast<AutoFrame *>(data)->height();
	};
	info.video_tick = [](void *data, float seconds) {
		static_cast<AutoFrame *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<AutoFrame *>(data)->render();
	};
	obs_register_source(&info);

	obs_source_info shape = {};
	shape.id = kPictureShapeId;
	shape.type = OBS_SOURCE_TYPE_FILTER;
	shape.output_flags = OBS_SOURCE_VIDEO;
	shape.get_name = [](void *) {
		return obs_module_text("Shape.Name");
	};
	shape.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new PictureShape(settings, source);
	};
	shape.destroy = [](void *data) {
		delete static_cast<PictureShape *>(data);
	};
	shape.update = [](void *data, obs_data_t *settings) {
		static_cast<PictureShape *>(data)->update(settings);
	};
	shape.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_string(settings, "shape", "4:3");
		obs_data_set_default_string(settings, "custom", "4:3");
	};
	shape.get_properties = picture_shape_properties;
	shape.video_tick = [](void *data, float) {
		static_cast<PictureShape *>(data)->tick();
	};
	shape.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<PictureShape *>(data)->source());
	};
	shape.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<PictureShape *>(data)->attach(parent);
	};
	shape.filter_remove = [](void *data, obs_source_t *) {
		static_cast<PictureShape *>(data)->detach();
	};
	obs_register_source(&shape);
}

} // namespace stagekit
