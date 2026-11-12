/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Ramp Blur: a single filter that smoothly blurs a source in and out.
//
// Replaces the usual Move-plugin chain (blur filter + "move value up" +
// "move value down" + "move action" filters per source) with one filter
// that has its own hotkeys, obs-websocket requests and Stream Deck support.

#include "ramp-blur.hpp"

#include "../api/vendor-api.hpp"
#include "../common/easing.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <set>
#include <string>

namespace stagekit {

namespace {

enum class BlurType : int {
	Gaussian = 0,
	Box = 1,
	Kawase = 2,
	Pixelate = 3,
	Zoom = 4,
	Motion = 5,
};

enum class RampStyle : int {
	Grow = 0, // blur strength grows from 0 to the configured strength
	Fade = 1, // full-strength blur cross-fades over the original
};

enum class StartState : int {
	Off = 0,
	On = 1,
	Remember = 2,
};

constexpr int kMaxKawaseLevels = 8;
constexpr double kMaxStrength = 250.0;

struct Settings {
	BlurType type = BlurType::Gaussian;
	double strength = 30.0;
	double angle_deg = 0.0;
	double center_x = 0.5;
	double center_y = 0.5;
	RampStyle style = RampStyle::Grow;
	double ramp_in_s = 0.4;
	double ramp_out_s = 0.4;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	StartState start = StartState::Off;
};

std::mutex g_registry_mutex;
std::set<RampBlur *> g_registry;

std::string replace_all(std::string text, const std::string &from, const std::string &to)
{
	for (size_t pos = text.find(from); pos != std::string::npos; pos = text.find(from, pos + to.size()))
		text.replace(pos, from.size(), to);
	return text;
}

} // namespace

class RampBlur {
public:
	RampBlur(obs_data_t *settings, obs_source_t *source);
	~RampBlur();

	void update(obs_data_t *settings);
	void tick(float seconds);
	void render();

	void attach(obs_source_t *parent);
	void detach();
	void on_rename();

	void set_active(bool active, double duration_ms);
	bool active() const
	{
		std::lock_guard lock(mutex_);
		return target_active_;
	}
	double level() const
	{
		std::lock_guard lock(mutex_);
		return level_;
	}
	BlurType type() const
	{
		std::lock_guard lock(mutex_);
		return settings_.type;
	}
	obs_source_t *source() const { return source_; }

private:
	bool capture_input(uint32_t w, uint32_t h);
	void pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique);
	struct Blurred {
		gs_texture_t *a = nullptr;
		gs_texture_t *b = nullptr; // optional second result, cross-faded in by b_mix
		double b_mix = 0.0;
	};
	using LevelTargets = std::array<gs_texrender_t *, kMaxKawaseLevels>;

	Blurred run_blur(const Settings &s, gs_texture_t *input, uint32_t w, uint32_t h, double strength, double &mix);
	gs_texture_t *kawase_chain(gs_texture_t *input, uint32_t w, uint32_t h, int levels, float offset,
				   LevelTargets &down, LevelTargets &up, gs_texrender_t *out);
	void composite(gs_texture_t *input, const Blurred &blurred, uint32_t w, uint32_t h, double mix);

	void register_hotkeys(obs_source_t *parent);
	void unregister_hotkeys();
	void remember_state(bool active);

	static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void rename_cb(void *data, calldata_t *cd);

	obs_source_t *source_;
	mutable std::mutex mutex_;
	Settings settings_;
	bool target_active_ = false;
	double level_ = 0.0;
	Tween tween_;

	obs_source_t *parent_ = nullptr;
	obs_hotkey_id hk_on_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_off_ = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hk_toggle_ = OBS_INVALID_HOTKEY_ID;

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

	gs_texrender_t *tr_input_ = nullptr;
	gs_texrender_t *tr_a_ = nullptr;
	gs_texrender_t *tr_b_ = nullptr;
	LevelTargets tr_down_{};
	LevelTargets tr_up_{};
	LevelTargets tr_down2_{};
	LevelTargets tr_up2_{};
};

// ------------------------------------------------------------------ setup

RampBlur::RampBlur(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	char *path = obs_module_file("effects/stagekit-blur.effect");
	obs_enter_graphics();
	char *errors = nullptr;
	effect_ = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	if (!effect_)
		obs_log(LOG_ERROR, "Ramp Blur: failed to load effect '%s': %s", path ? path : "(missing)",
			errors ? errors : "unknown error");
	bfree(errors);
	if (effect_) {
		p_image_ = gs_effect_get_param_by_name(effect_, "image");
		p_image2_ = gs_effect_get_param_by_name(effect_, "image2");
		p_mix2_ = gs_effect_get_param_by_name(effect_, "mix2");
		p_original_ = gs_effect_get_param_by_name(effect_, "original");
		p_texel_ = gs_effect_get_param_by_name(effect_, "texel");
		p_size_ = gs_effect_get_param_by_name(effect_, "size");
		p_direction_ = gs_effect_get_param_by_name(effect_, "direction");
		p_radius_ = gs_effect_get_param_by_name(effect_, "radius");
		p_samples_ = gs_effect_get_param_by_name(effect_, "samples");
		p_offset_ = gs_effect_get_param_by_name(effect_, "offset");
		p_center_ = gs_effect_get_param_by_name(effect_, "center");
		p_strength_ = gs_effect_get_param_by_name(effect_, "strength");
		p_mix_ = gs_effect_get_param_by_name(effect_, "mix_amount");
	}
	tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_a_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	tr_b_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (LevelTargets *targets : {&tr_down_, &tr_up_, &tr_down2_, &tr_up2_})
		for (auto &tr : *targets)
			tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();
	bfree(path);

	update(settings);

	// Start state is applied once, when the filter is created or loaded.
	const auto start = static_cast<StartState>(obs_data_get_int(settings, "start_state"));
	const bool start_on = start == StartState::On ||
			      (start == StartState::Remember && obs_data_get_bool(settings, "active"));
	target_active_ = start_on;
	level_ = start_on ? 1.0 : 0.0;
	tween_.start(level_, level_, 0.0, EaseCurve::Linear, EaseMode::InOut);

	signal_handler_connect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);

	proc_handler_t *ph = obs_source_get_proc_handler(source_);
	proc_handler_add(
		ph, "void blur_on()",
		[](void *data, calldata_t *) { static_cast<RampBlur *>(data)->set_active(true, -1.0); }, this);
	proc_handler_add(
		ph, "void blur_off()",
		[](void *data, calldata_t *) { static_cast<RampBlur *>(data)->set_active(false, -1.0); }, this);
	proc_handler_add(
		ph, "void blur_toggle()",
		[](void *data, calldata_t *) {
			auto *self = static_cast<RampBlur *>(data);
			self->set_active(!self->active(), -1.0);
		},
		this);
	proc_handler_add(
		ph, "void get_state(out bool active, out float level)",
		[](void *data, calldata_t *cd) {
			auto *self = static_cast<RampBlur *>(data);
			calldata_set_bool(cd, "active", self->active());
			calldata_set_float(cd, "level", self->level());
		},
		this);

	std::lock_guard lock(g_registry_mutex);
	g_registry.insert(this);
}

RampBlur::~RampBlur()
{
	{
		std::lock_guard lock(g_registry_mutex);
		g_registry.erase(this);
	}
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);
	unregister_hotkeys();

	obs_enter_graphics();
	gs_effect_destroy(effect_);
	gs_texrender_destroy(tr_input_);
	gs_texrender_destroy(tr_a_);
	gs_texrender_destroy(tr_b_);
	for (LevelTargets *targets : {&tr_down_, &tr_up_, &tr_down2_, &tr_up2_})
		for (auto *tr : *targets)
			gs_texrender_destroy(tr);
	obs_leave_graphics();
}

void RampBlur::update(obs_data_t *settings)
{
	Settings s;
	s.type = static_cast<BlurType>(obs_data_get_int(settings, "blur_type"));
	s.strength = std::clamp(obs_data_get_double(settings, "strength"), 0.0, kMaxStrength);
	s.angle_deg = obs_data_get_double(settings, "angle");
	s.center_x = obs_data_get_double(settings, "center_x") / 100.0;
	s.center_y = obs_data_get_double(settings, "center_y") / 100.0;
	s.style = static_cast<RampStyle>(obs_data_get_int(settings, "ramp_style"));
	s.ramp_in_s = std::max<long long>(0, obs_data_get_int(settings, "ramp_in_ms")) / 1000.0;
	s.ramp_out_s = std::max<long long>(0, obs_data_get_int(settings, "ramp_out_ms")) / 1000.0;
	s.curve = static_cast<EaseCurve>(obs_data_get_int(settings, "ease_curve"));
	s.mode = static_cast<EaseMode>(obs_data_get_int(settings, "ease_mode"));
	s.start = static_cast<StartState>(obs_data_get_int(settings, "start_state"));

	std::lock_guard lock(mutex_);
	settings_ = s;
}

// ---------------------------------------------------------------- control

void RampBlur::set_active(bool active, double duration_ms)
{
	bool changed;
	{
		std::lock_guard lock(mutex_);
		changed = active != target_active_;
		target_active_ = active;
		const double target = active ? 1.0 : 0.0;
		const double full = duration_ms >= 0.0 ? duration_ms / 1000.0
						       : (active ? settings_.ramp_in_s : settings_.ramp_out_s);
		// Reversing part-way through a ramp only takes the remaining distance,
		// so the speed stays consistent.
		const double distance = std::clamp(std::fabs(target - level_), 0.0, 1.0);
		tween_.start(level_, target, full * distance, settings_.curve, settings_.mode);
		if (tween_.duration <= 0.0)
			level_ = target;
	}
	if (changed) {
		remember_state(active);
		vendor_emit_blur_changed(source_, active);
	}
}

void RampBlur::remember_state(bool active)
{
	obs_data_t *settings = obs_source_get_settings(source_);
	if (!settings)
		return;
	if (static_cast<StartState>(obs_data_get_int(settings, "start_state")) == StartState::Remember)
		obs_data_set_bool(settings, "active", active);
	obs_data_release(settings);
}

void RampBlur::tick(float seconds)
{
	std::lock_guard lock(mutex_);
	// Back/Elastic curves overshoot; a negative blur makes no sense.
	level_ = std::max(0.0, tween_.advance(seconds));
}

// ---------------------------------------------------------------- hotkeys

void RampBlur::hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *self = static_cast<RampBlur *>(data);
	if (id == self->hk_on_)
		self->set_active(true, -1.0);
	else if (id == self->hk_off_)
		self->set_active(false, -1.0);
	else if (id == self->hk_toggle_)
		self->set_active(!self->active(), -1.0);
}

void RampBlur::register_hotkeys(obs_source_t *parent)
{
	unregister_hotkeys();
	if (!parent)
		return;
	parent_ = parent;
	const std::string name = obs_source_get_name(source_) ? obs_source_get_name(source_) : "";

	auto reg = [&](const char *key, const char *text_key) {
		const std::string id = std::string("stagekit.blur.") + key + "." + name;
		const std::string desc = replace_all(obs_module_text(text_key), "%1", name);
		return obs_hotkey_register_source(parent, id.c_str(), desc.c_str(), hotkey_cb, this);
	};
	hk_toggle_ = reg("toggle", "Hotkey.BlurToggle");
	hk_on_ = reg("on", "Hotkey.BlurOn");
	hk_off_ = reg("off", "Hotkey.BlurOff");
}

void RampBlur::unregister_hotkeys()
{
	for (obs_hotkey_id *id : {&hk_on_, &hk_off_, &hk_toggle_}) {
		if (*id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(*id);
		*id = OBS_INVALID_HOTKEY_ID;
	}
	parent_ = nullptr;
}

void RampBlur::attach(obs_source_t *parent)
{
	register_hotkeys(parent);
}

void RampBlur::detach()
{
	unregister_hotkeys();
}

void RampBlur::rename_cb(void *data, calldata_t *)
{
	static_cast<RampBlur *>(data)->on_rename();
}

void RampBlur::on_rename()
{
	// Hotkey ids include the filter name; re-register and carry the
	// user's key bindings across.
	if (!parent_)
		return;
	obs_data_array_t *on = obs_hotkey_save(hk_on_);
	obs_data_array_t *off = obs_hotkey_save(hk_off_);
	obs_data_array_t *toggle = obs_hotkey_save(hk_toggle_);
	register_hotkeys(parent_);
	obs_hotkey_load(hk_on_, on);
	obs_hotkey_load(hk_off_, off);
	obs_hotkey_load(hk_toggle_, toggle);
	obs_data_array_release(on);
	obs_data_array_release(off);
	obs_data_array_release(toggle);
}

// -------------------------------------------------------------- rendering

bool RampBlur::capture_input(uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr_input_);
	if (!obs_source_process_filter_begin(source_, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
		return false;
	if (!gs_texrender_begin_with_color_space(tr_input_, w, h, GS_CS_SRGB))
		return false;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	obs_source_process_filter_end(source_, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
	gs_blend_state_pop();
	gs_texrender_end(tr_input_);
	return true;
}

void RampBlur::pass(gs_texrender_t *dst, gs_texture_t *src, uint32_t w, uint32_t h, const char *technique)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin_with_color_space(dst, w, h, GS_CS_SRGB))
		return;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	// Sample and write through sRGB views so the blur happens in linear
	// light; bright highlights spread naturally instead of going muddy.
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(p_image_, src);
	while (gs_effect_loop(effect_, technique))
		gs_draw_sprite(src, 0, w, h);
	gs_enable_framebuffer_srgb(previous);

	gs_blend_state_pop();
	gs_texrender_end(dst);
}

gs_texture_t *RampBlur::kawase_chain(gs_texture_t *input, uint32_t w, uint32_t h, int levels, float offset,
				     LevelTargets &down, LevelTargets &up, gs_texrender_t *out)
{
	std::array<uint32_t, kMaxKawaseLevels + 1> lw{}, lh{};
	lw[0] = w;
	lh[0] = h;
	for (int i = 1; i <= levels; i++) {
		lw[i] = std::max(1u, lw[i - 1] / 2);
		lh[i] = std::max(1u, lh[i - 1] / 2);
	}

	gs_texture_t *cur = input;
	for (int i = 0; i < levels; i++) {
		vec2 t;
		vec2_set(&t, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &t);
		gs_effect_set_float(p_offset_, offset);
		pass(down[i], cur, lw[i + 1], lh[i + 1], "KawaseDown");
		cur = gs_texrender_get_texture(down[i]);
	}
	for (int i = levels; i >= 1; i--) {
		vec2 t;
		vec2_set(&t, 1.0f / (float)lw[i], 1.0f / (float)lh[i]);
		gs_effect_set_vec2(p_texel_, &t);
		gs_effect_set_float(p_offset_, offset);
		gs_texrender_t *dst = i == 1 ? out : up[i - 1];
		pass(dst, cur, lw[i - 1], lh[i - 1], "KawaseUp");
		cur = gs_texrender_get_texture(dst);
	}
	return cur;
}

RampBlur::Blurred RampBlur::run_blur(const Settings &s, gs_texture_t *input, uint32_t w, uint32_t h, double strength,
				     double &mix)
{
	vec2 texel, size;
	vec2_set(&texel, 1.0f / (float)w, 1.0f / (float)h);
	vec2_set(&size, (float)w, (float)h);
	gs_effect_set_vec2(p_texel_, &texel);
	gs_effect_set_vec2(p_size_, &size);

	// Gaussian weighting hides wide sample spacing; a flat box kernel does
	// not, so it gets twice the taps before spacing starts to stripe.
	const double max_taps = s.type == BlurType::Box ? 64.0 : 32.0;
	auto taps = [max_taps](double px) {
		return (float)std::clamp(std::ceil(px), 1.0, max_taps);
	};

	switch (s.type) {
	case BlurType::Gaussian:
	case BlurType::Box: {
		const char *tech = s.type == BlurType::Gaussian ? "Gaussian" : "Box";
		vec2 dir;
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, taps(strength));
		vec2_set(&dir, texel.x, 0.0f);
		gs_effect_set_vec2(p_direction_, &dir);
		pass(tr_a_, input, w, h, tech);

		gs_effect_set_vec2(p_texel_, &texel);
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, taps(strength));
		vec2_set(&dir, 0.0f, texel.y);
		gs_effect_set_vec2(p_direction_, &dir);
		pass(tr_b_, gs_texrender_get_texture(tr_a_), w, h, tech);
		return {gs_texrender_get_texture(tr_b_)};
	}

	case BlurType::Motion: {
		const double rad = s.angle_deg * 3.14159265358979323846 / 180.0;
		vec2 dir;
		vec2_set(&dir, (float)std::cos(rad) * texel.x, (float)std::sin(rad) * texel.y);
		gs_effect_set_vec2(p_direction_, &dir);
		gs_effect_set_float(p_radius_, (float)strength);
		gs_effect_set_float(p_samples_, (float)std::clamp(std::ceil(strength), 1.0, 64.0));
		pass(tr_a_, input, w, h, "Motion");
		return {gs_texrender_get_texture(tr_a_)};
	}

	case BlurType::Pixelate:
		gs_effect_set_float(p_strength_, (float)(1.0 + strength));
		pass(tr_a_, input, w, h, "Pixelate");
		return {gs_texrender_get_texture(tr_a_)};

	case BlurType::Zoom: {
		// Strength 250 pulls samples 60% of the way to the centre.
		const double amount = strength / kMaxStrength * 0.6;
		const double reach_px = amount * 0.75 * std::max(w, h);
		vec2 center;
		vec2_set(&center, (float)s.center_x, (float)s.center_y);
		gs_effect_set_vec2(p_center_, &center);
		gs_effect_set_float(p_strength_, (float)amount);
		gs_effect_set_float(p_samples_, (float)std::clamp(std::ceil(reach_px), 1.0, 64.0));
		pass(tr_a_, input, w, h, "Zoom");
		return {gs_texrender_get_texture(tr_a_)};
	}

	case BlurType::Kawase: {
		// Each level halves the resolution. The level count and per-level
		// offset are chosen so the blur grows continuously with strength.
		// kKawaseScale makes a given strength look about as strong as the
		// Gaussian at the same number.
		constexpr double kKawaseScale = 0.14;
		const double scaled = strength * kKawaseScale;
		const double k = std::max(scaled, 1.0);
		if (scaled < 1.0)
			mix *= scaled; // fade in the first, smallest step
		auto fits = [w, h](int levels) {
			return (w >> levels) >= 2 && (h >> levels) >= 2;
		};
		int levels = std::clamp((int)std::floor(std::log2(k)) + 1, 1, kMaxKawaseLevels);
		while (levels > 1 && !fits(levels))
			levels--;
		const double offset = k / std::pow(2.0, levels - 1); // [1, 2) unless capped

		// Changing the level count abruptly shows as a visible pop while
		// ramping. Across each octave, cross-fade towards the result with
		// one more level at half the offset; at the end of the octave that
		// is exactly what the next octave starts with.
		Blurred out;
		out.a = kawase_chain(input, w, h, levels, (float)offset, tr_down_, tr_up_, tr_a_);
		if (levels < kMaxKawaseLevels && fits(levels + 1)) {
			out.b_mix = std::clamp(offset - 1.0, 0.0, 1.0);
			if (out.b_mix > 0.001)
				out.b = kawase_chain(input, w, h, levels + 1, (float)(offset * 0.5), tr_down2_, tr_up2_,
						     tr_b_);
		}
		return out;
	}
	}
	return {};
}

void RampBlur::composite(gs_texture_t *input, const Blurred &blurred, uint32_t w, uint32_t h, double mix)
{
	gs_texture_t *a = blurred.a ? blurred.a : input;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_effect_set_texture_srgb(p_original_, input);
	gs_effect_set_texture_srgb(p_image_, a);
	gs_effect_set_texture_srgb(p_image2_, blurred.b ? blurred.b : a);
	gs_effect_set_float(p_mix2_, blurred.b ? (float)blurred.b_mix : 0.0f);
	gs_effect_set_float(p_mix_, blurred.a ? (float)std::clamp(mix, 0.0, 1.0) : 0.0f);
	while (gs_effect_loop(effect_, "Composite"))
		gs_draw_sprite(input, 0, w, h);
	gs_enable_framebuffer_srgb(previous);
}

void RampBlur::render()
{
	Settings s;
	double level;
	{
		std::lock_guard lock(mutex_);
		s = settings_;
		level = level_;
	}

	double strength = s.strength;
	double mix = 1.0;
	if (s.style == RampStyle::Grow)
		strength *= level;
	else
		mix = std::min(level, 1.0);

	// Fully off costs nothing: hand rendering straight back to OBS.
	if (!effect_ || level <= 0.0005 || strength <= 0.01 || mix <= 0.0005) {
		obs_source_skip_video_filter(source_);
		return;
	}

	obs_source_t *target = obs_filter_get_target(source_);
	obs_source_t *parent = obs_filter_get_parent(source_);
	const uint32_t w = target ? obs_source_get_base_width(target) : 0;
	const uint32_t h = target ? obs_source_get_base_height(target) : 0;
	if (!parent || !w || !h) {
		obs_source_skip_video_filter(source_);
		return;
	}

	if (!capture_input(w, h))
		return;
	gs_texture_t *input = gs_texrender_get_texture(tr_input_);
	if (!input)
		return;

	const Blurred blurred = run_blur(s, input, w, h, std::min(strength, kMaxStrength), mix);
	composite(input, blurred, w, h, mix);
}

// ------------------------------------------------------------- properties

namespace {

bool type_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const auto type = static_cast<BlurType>(obs_data_get_int(settings, "blur_type"));
	obs_property_set_visible(obs_properties_get(props, "angle"), type == BlurType::Motion);
	obs_property_set_visible(obs_properties_get(props, "center_x"), type == BlurType::Zoom);
	obs_property_set_visible(obs_properties_get(props, "center_y"), type == BlurType::Zoom);
	return true;
}

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *type = obs_properties_add_list(props, "blur_type", obs_module_text("RampBlur.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Gaussian"), (int)BlurType::Gaussian);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Kawase"), (int)BlurType::Kawase);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Box"), (int)BlurType::Box);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Pixelate"), (int)BlurType::Pixelate);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Zoom"), (int)BlurType::Zoom);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Motion"), (int)BlurType::Motion);
	obs_property_set_modified_callback(type, type_modified);

	obs_properties_add_float_slider(props, "strength", obs_module_text("RampBlur.Strength"), 0.0, kMaxStrength,
					0.5);
	obs_property_t *angle =
		obs_properties_add_float_slider(props, "angle", obs_module_text("RampBlur.Angle"), -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(angle, "°");
	obs_property_t *cx = obs_properties_add_float_slider(props, "center_x", obs_module_text("RampBlur.CenterX"),
							     0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cx, "%");
	obs_property_t *cy = obs_properties_add_float_slider(props, "center_y", obs_module_text("RampBlur.CenterY"),
							     0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cy, "%");

	obs_properties_t *ramp = obs_properties_create();
	obs_property_t *style = obs_properties_add_list(ramp, "ramp_style", obs_module_text("RampBlur.Style"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style, obs_module_text("RampBlur.Style.Grow"), (int)RampStyle::Grow);
	obs_property_list_add_int(style, obs_module_text("RampBlur.Style.Fade"), (int)RampStyle::Fade);
	obs_property_t *in =
		obs_properties_add_int_slider(ramp, "ramp_in_ms", obs_module_text("RampBlur.RampIn"), 0, 5000, 50);
	obs_property_int_set_suffix(in, " ms");
	obs_property_t *out =
		obs_properties_add_int_slider(ramp, "ramp_out_ms", obs_module_text("RampBlur.RampOut"), 0, 5000, 50);
	obs_property_int_set_suffix(out, " ms");
	add_easing_properties(ramp, "");
	obs_properties_add_group(props, "ramp_group", obs_module_text("RampBlur.RampGroup"), OBS_GROUP_NORMAL, ramp);

	obs_property_t *start = obs_properties_add_list(props, "start_state", obs_module_text("RampBlur.StartState"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(start, obs_module_text("RampBlur.StartState.Off"), (int)StartState::Off);
	obs_property_list_add_int(start, obs_module_text("RampBlur.StartState.On"), (int)StartState::On);
	obs_property_list_add_int(start, obs_module_text("RampBlur.StartState.Remember"), (int)StartState::Remember);

	obs_properties_add_button2(
		props, "btn_toggle", obs_module_text("RampBlur.Button.Toggle"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *self = static_cast<RampBlur *>(d);
			self->set_active(!self->active(), -1.0);
			return false;
		},
		data);
	obs_properties_add_button2(
		props, "btn_on", obs_module_text("RampBlur.Button.On"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			static_cast<RampBlur *>(d)->set_active(true, -1.0);
			return false;
		},
		data);
	obs_properties_add_button2(
		props, "btn_off", obs_module_text("RampBlur.Button.Off"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			static_cast<RampBlur *>(d)->set_active(false, -1.0);
			return false;
		},
		data);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("RampBlur.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);

	return props;
}

void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "blur_type", (int)BlurType::Gaussian);
	obs_data_set_default_double(settings, "strength", 30.0);
	obs_data_set_default_double(settings, "angle", 0.0);
	obs_data_set_default_double(settings, "center_x", 50.0);
	obs_data_set_default_double(settings, "center_y", 50.0);
	obs_data_set_default_int(settings, "ramp_style", (int)RampStyle::Grow);
	obs_data_set_default_int(settings, "ramp_in_ms", 400);
	obs_data_set_default_int(settings, "ramp_out_ms", 400);
	obs_data_set_default_int(settings, "ease_curve", (int)EaseCurve::Cubic);
	obs_data_set_default_int(settings, "ease_mode", (int)EaseMode::InOut);
	obs_data_set_default_int(settings, "start_state", (int)StartState::Off);
	obs_data_set_default_bool(settings, "active", false);
}

} // namespace

// --------------------------------------------------------------- registry

void register_ramp_blur()
{
	obs_source_info info = {};
	info.id = kRampBlurId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.get_name = [](void *) {
		return obs_module_text("RampBlur.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new RampBlur(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<RampBlur *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<RampBlur *>(data)->update(settings);
	};
	info.get_defaults = get_defaults;
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<RampBlur *>(data)->tick(seconds);
	};
	info.video_render = [](void *data, gs_effect_t *) {
		static_cast<RampBlur *>(data)->render();
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<RampBlur *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<RampBlur *>(data)->detach();
	};
	obs_register_source(&info);
}

RampBlur *ramp_blur_from_source(obs_source_t *filter)
{
	if (!filter)
		return nullptr;
	const char *id = obs_source_get_unversioned_id(filter);
	if (!id || strcmp(id, kRampBlurId) != 0)
		return nullptr;
	return static_cast<RampBlur *>(obs_obj_get_data(filter));
}

void for_each_ramp_blur(const std::function<void(RampBlur *, obs_source_t *)> &fn)
{
	std::vector<std::pair<RampBlur *, obs_source_t *>> blurs;
	{
		std::lock_guard lock(g_registry_mutex);
		for (RampBlur *blur : g_registry) {
			// get_ref fails if the filter is already being destroyed.
			if (obs_source_t *ref = obs_source_get_ref(blur->source()))
				blurs.emplace_back(blur, ref);
		}
	}
	for (auto &[blur, ref] : blurs) {
		fn(blur, ref);
		obs_source_release(ref);
	}
}

void ramp_blur_set_active(RampBlur *blur, bool active, double duration_ms)
{
	if (blur)
		blur->set_active(active, duration_ms);
}

void ramp_blur_toggle(RampBlur *blur)
{
	if (blur)
		blur->set_active(!blur->active(), -1.0);
}

bool ramp_blur_is_active(RampBlur *blur)
{
	return blur && blur->active();
}

double ramp_blur_level(RampBlur *blur)
{
	return blur ? blur->level() : 0.0;
}

const char *ramp_blur_type_name(RampBlur *blur)
{
	if (!blur)
		return "";
	switch (blur->type()) {
	case BlurType::Gaussian:
		return "gaussian";
	case BlurType::Box:
		return "box";
	case BlurType::Kawase:
		return "kawase";
	case BlurType::Pixelate:
		return "pixelate";
	case BlurType::Zoom:
		return "zoom";
	case BlurType::Motion:
		return "motion";
	}
	return "";
}

} // namespace stagekit
