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
#include "../common/timing.hpp"

#include "../api/vendor-api.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace stagekit {

namespace {

enum class BlurType : int {
	Gaussian = 0,
	Box = 1,
	Kawase = 2,
	Pixelate = 3,
	Zoom = 4,
	Motion = 5,
	Hexagon = 6,
};

enum class RampStyle : int {
	Grow = 0, // blur strength grows from 0 to the configured strength
	Fade = 1, // full-strength blur cross-fades over the original
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
	bool include_in_all = true;
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
	bool include_in_all() const
	{
		std::lock_guard lock(mutex_);
		return settings_.include_in_all;
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
	bool begin_ramp(bool active, double duration_ms);
	void set_enabled_quietly(bool enabled);
	// Tells the world the blur went on or off, and takes the other sources
	// this filter was told to blur along with it.
	void announce(bool active);
	void lead_others(bool active);
	// Captures switched on because they were live when this went on.
	std::vector<obs_weak_source_t *> led_;
	std::mutex led_mutex_; // the switch can be flipped from any thread
	void on_enabled_changed(bool enabled);

	static void hotkey_cb(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed);
	static void rename_cb(void *data, calldata_t *cd);
	static void enable_cb(void *data, calldata_t *cd);

	obs_source_t *source_;
	mutable std::mutex mutex_;
	Settings settings_;
	bool target_active_ = false;
	double level_ = 0.0;
	Tween tween_;
	// The filter's own on/off switch (eye icon, Stream Deck "Filter" key,
	// obs-websocket SetSourceFilterEnabled) is the blur's on/off state.
	bool initialized_ = false;
	std::atomic<bool> reconnect_enable_signal_{false};

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

	// The starting level is taken from the filter's enabled state on the
	// first tick (OBS restores that state just after creating the filter).
	signal_handler_connect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);
	signal_handler_connect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);

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
	for (obs_weak_source_t *weak : led_)
		obs_weak_source_release(weak);
	{
		std::lock_guard lock(g_registry_mutex);
		g_registry.erase(this);
	}
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "rename", rename_cb, this);
	signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", enable_cb, this);
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
	s.include_in_all = obs_data_get_bool(settings, "include_in_all");

	std::lock_guard lock(mutex_);
	settings_ = s;
}

// ---------------------------------------------------------------- control

namespace {
// Set while this thread is changing a filter's enabled state itself, so the
// resulting "enable" signal is not mistaken for the user flipping the switch.
thread_local bool t_changing_enabled = false;
} // namespace

// Starts the ramp. Returns true if the on/off target changed.
bool RampBlur::begin_ramp(bool active, double duration_ms)
{
	std::lock_guard lock(mutex_);
	if (!initialized_) {
		initialized_ = true;
		level_ = obs_source_enabled(source_) ? 1.0 : 0.0;
		target_active_ = level_ > 0.0;
	}
	const bool changed = active != target_active_;
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
	return changed;
}

void RampBlur::set_enabled_quietly(bool enabled)
{
	if (obs_source_enabled(source_) == enabled)
		return;
	t_changing_enabled = true;
	obs_source_set_enabled(source_, enabled);
	t_changing_enabled = false;
}

// Hotkeys, obs-websocket vendor requests and scripts come through here.
void RampBlur::set_active(bool active, double duration_ms)
{
	const bool changed = begin_ramp(active, duration_ms);
	// Ramping in needs the filter enabled right away. Ramping out keeps it
	// enabled until the ramp finishes; tick() then switches it off.
	if (active)
		set_enabled_quietly(true);
	if (changed)
		announce(active);
}

void RampBlur::announce(bool active)
{
	vendor_emit_blur_changed(source_, active);
	lead_others(active);
}

namespace {
// Set while one filter is switching the others, so they do not lead in turn.
thread_local bool t_leading = false;

// The names in a filter's "Blur these too" list.
std::vector<std::string> also_names(obs_data_t *settings)
{
	std::vector<std::string> names;
	obs_data_array_t *list = obs_data_get_array(settings, "also");
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const std::string name = obs_data_get_string(entry, "value");
		if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	return names;
}

// Every live capture showing in a scene, looking inside groups and nested scenes.
struct LiveSearch {
	std::vector<obs_source_t *> *found;
	int depth;
};
bool find_live(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *search = static_cast<LiveSearch *>(param);
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source || !obs_sceneitem_visible(item))
		return true;
	if (obs_scene_t *inner = scene_of(source)) {
		if (search->depth < 8) {
			LiveSearch deeper{search->found, search->depth + 1};
			obs_scene_enum_items(inner, find_live, &deeper);
		}
	} else if (is_capture_source(source) && obs_source_get_width(source) > 0 &&
		   std::find(search->found->begin(), search->found->end(), source) == search->found->end()) {
		search->found->push_back(source);
	}
	return true;
}

// The first Ramp Blur on a source. Not a new reference.
obs_source_t *ramp_blur_on(obs_source_t *source)
{
	obs_source_t *found = nullptr;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto **out = static_cast<obs_source_t **>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (!*out && id && strcmp(id, kRampBlurId) == 0)
				*out = filter;
		},
		&found);
	return found;
}
} // namespace

// One switch for several sources: the sources listed in this filter, and any
// live capture on screen if asked, are blurred and cleared with it. A source
// without a Ramp Blur of its own is given one like this one.
void RampBlur::lead_others(bool active)
{
	if (t_leading)
		return;
	std::lock_guard led_lock(led_mutex_);
	obs_source_t *parent = obs_filter_get_parent(source_);
	obs_data_t *settings = obs_source_get_settings(source_);
	const std::vector<std::string> names = also_names(settings);
	const bool captures = obs_data_get_bool(settings, "also_captures");
	if (names.empty() && !captures && led_.empty()) {
		obs_data_release(settings);
		return;
	}

	std::vector<obs_source_t *> targets; // each a reference
	auto add = [&](obs_source_t *source) {
		if (source && source != parent && std::find(targets.begin(), targets.end(), source) == targets.end())
			targets.push_back(obs_source_get_ref(source));
	};
	for (const std::string &name : names) {
		obs_source_t *source = obs_get_source_by_name(name.c_str());
		add(source);
		obs_source_release(source);
	}
	if (captures && active) {
		std::vector<obs_source_t *> live;
		if (obs_source_t *scene = scene_on_screen()) {
			LiveSearch search{&live, 0};
			if (obs_scene_t *s = scene_of(scene))
				obs_scene_enum_items(s, find_live, &search);
			obs_source_release(scene);
		}
		for (obs_source_t *source : live) {
			if (source == parent)
				continue;
			add(source);
			led_.push_back(obs_source_get_weak_source(source));
		}
	}
	if (!active) {
		// Whatever was live when it went on is cleared with it, even if it
		// has left the screen since.
		for (obs_weak_source_t *weak : led_) {
			obs_source_t *source = obs_weak_source_get_source(weak);
			add(source);
			obs_source_release(source);
			obs_weak_source_release(weak);
		}
		led_.clear();
	}

	t_leading = true;
	for (obs_source_t *target : targets) {
		obs_source_t *filter = ramp_blur_on(target);
		if (!filter && active) {
			// A copy of this one, without a list of its own.
			obs_data_t *copy = obs_data_create();
			obs_data_apply(copy, settings);
			for (const char *own : {"also", "also_pick", "also_captures"})
				obs_data_erase(copy, own);
			obs_source_t *made =
				obs_source_create(kRampBlurId, obs_module_text("RampBlur.LedName"), copy, nullptr);
			obs_data_release(copy);
			if (made) {
				t_changing_enabled = true;
				obs_source_set_enabled(made, false); // so it ramps in rather than appearing
				t_changing_enabled = false;
				obs_source_filter_add(target, made);
				filter = ramp_blur_on(target);
				obs_source_release(made);
			}
		}
		if (filter) {
			if (auto *blur = static_cast<RampBlur *>(obs_obj_get_data(filter)))
				blur->set_active(active, -1.0);
		}
		obs_source_release(target);
	}
	t_leading = false;
	obs_data_release(settings);
}

void RampBlur::enable_cb(void *data, calldata_t *cd)
{
	if (!t_changing_enabled)
		static_cast<RampBlur *>(data)->on_enabled_changed(calldata_bool(cd, "enabled"));
}

// The user (or a Stream Deck "Filter" key, or obs-websocket) flipped the
// filter's switch.
void RampBlur::on_enabled_changed(bool enabled)
{
	bool closing;
	double level;
	{
		std::lock_guard lock(mutex_);
		if (!initialized_)
			return; // first tick picks the state up
		closing = !target_active_ && level_ > 0.0;
		level = level_;
	}

	if (enabled) {
		if (begin_ramp(true, -1.0))
			announce(true);
		return;
	}

	if (closing) {
		// Switched "off" again while already ramping out: the key was
		// pressed a second time, so bring the blur back.
		if (begin_ramp(true, -1.0))
			announce(true);
		set_enabled_quietly(true);
		return;
	}

	const bool changed = begin_ramp(false, -1.0);
	// A disabled filter is not drawn at all, so stay enabled while the blur
	// ramps out; tick() switches the filter off when it reaches zero.
	if (level > 0.0 && this->level() > 0.0)
		set_enabled_quietly(true);
	if (changed)
		announce(false);
}

void RampBlur::tick(float seconds)
{
	// Keep our "enable" handler last in the list so tools that mirror the
	// filter state (obs-websocket, Stream Deck) see our correction after
	// the change that caused it. They connect when the filter is added.
	if (reconnect_enable_signal_.exchange(false)) {
		signal_handler_t *sh = obs_source_get_signal_handler(source_);
		signal_handler_disconnect(sh, "enable", enable_cb, this);
		signal_handler_connect(sh, "enable", enable_cb, this);
	}

	bool finished_off;
	{
		std::lock_guard lock(mutex_);
		if (!initialized_) {
			initialized_ = true;
			level_ = obs_source_enabled(source_) ? 1.0 : 0.0;
			target_active_ = level_ > 0.0;
			tween_.start(level_, level_, 0.0, EaseCurve::Linear, EaseMode::InOut);
		}
		// Back/Elastic curves overshoot; a negative blur makes no sense.
		level_ = std::max(0.0, tween_.advance(seconds));
		finished_off = !target_active_ && !tween_.running() && level_ <= 0.0;
	}
	if (finished_off)
		set_enabled_quietly(false);
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
	reconnect_enable_signal_ = true;
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

	case BlurType::Hexagon:
		gs_effect_set_float(p_strength_, (float)(1.0 + strength));
		pass(tr_a_, input, w, h, "Hexagon");
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
	obs_properties_t *look = obs_properties_create();

	obs_property_t *type = obs_properties_add_list(look, "blur_type", obs_module_text("RampBlur.Type"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Gaussian"), (int)BlurType::Gaussian);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Kawase"), (int)BlurType::Kawase);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Box"), (int)BlurType::Box);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Pixelate"), (int)BlurType::Pixelate);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Hexagon"), (int)BlurType::Hexagon);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Zoom"), (int)BlurType::Zoom);
	obs_property_list_add_int(type, obs_module_text("RampBlur.Type.Motion"), (int)BlurType::Motion);
	obs_property_set_modified_callback(type, type_modified);

	obs_properties_add_float_slider(look, "strength", obs_module_text("RampBlur.Strength"), 0.0, kMaxStrength, 0.5);
	obs_property_t *angle =
		obs_properties_add_float_slider(look, "angle", obs_module_text("RampBlur.Angle"), -180.0, 180.0, 1.0);
	obs_property_float_set_suffix(angle, "°");
	obs_property_t *cx =
		obs_properties_add_float_slider(look, "center_x", obs_module_text("RampBlur.CenterX"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cx, "%");
	obs_property_t *cy =
		obs_properties_add_float_slider(look, "center_y", obs_module_text("RampBlur.CenterY"), 0.0, 100.0, 0.5);
	obs_property_float_set_suffix(cy, "%");

	obs_properties_add_group(props, "look_group", obs_module_text("RampBlur.LookGroup"), OBS_GROUP_NORMAL, look);

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

	// One switch for several sources.
	obs_source_t *own_filter = data ? static_cast<RampBlur *>(data)->source() : nullptr;
	PickList also;
	also.setting = "also";
	also.title = "RampBlur.AlsoGroup";
	also.add = "RampBlur.AlsoPick";
	also.tip = "RampBlur.Also.Tip";
	also.choices = [own_filter](std::vector<std::string> &names) {
		auto gather = [](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (name && *name && (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
				static_cast<std::vector<std::string> *>(param)->push_back(name);
			return true;
		};
		obs_enum_sources(gather, &names);
		obs_enum_scenes(gather, &names);
		obs_source_t *parent = own_filter ? obs_filter_get_parent(own_filter) : nullptr;
		const char *own = parent ? obs_source_get_name(parent) : nullptr;
		if (own)
			names.erase(std::remove(names.begin(), names.end(), std::string(own)), names.end());
	};
	pick_list_add(props, own_filter, also);
	obs_property_t *live =
		obs_properties_add_bool(props, "also_captures", obs_module_text("RampBlur.AlsoCaptures"));
	obs_property_set_long_description(live, obs_module_text("RampBlur.AlsoCaptures.Tip"));

	obs_properties_t *keys = obs_properties_create();
	obs_property_t *in_all =
		obs_properties_add_bool(keys, "include_in_all", obs_module_text("RampBlur.IncludeInAll"));
	obs_property_set_long_description(in_all, obs_module_text("RampBlur.Help"));
	obs_properties_add_button2(
		keys, "btn_toggle", obs_module_text("RampBlur.Button.Toggle"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			auto *self = static_cast<RampBlur *>(d);
			self->set_active(!self->active(), -1.0);
			return false;
		},
		data);
	obs_properties_add_group(props, "keys_group", obs_module_text("RampBlur.KeysGroup"), OBS_GROUP_NORMAL, keys);

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
	obs_data_set_default_bool(settings, "include_in_all", true);
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
	register_timed(&info);
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

bool ramp_blur_set_all(BlurAllMode mode)
{
	bool any_active = false;
	if (mode == BlurAllMode::Toggle) {
		for_each_ramp_blur([&](RampBlur *blur, obs_source_t *filter) {
			if (blur->include_in_all() && obs_filter_get_parent(filter) && blur->active())
				any_active = true;
		});
	}
	const bool active = mode == BlurAllMode::On || (mode == BlurAllMode::Toggle && !any_active);
	for_each_ramp_blur([&](RampBlur *blur, obs_source_t *filter) {
		if (blur->include_in_all() && obs_filter_get_parent(filter))
			blur->set_active(active, -1.0);
	});
	return active;
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
	case BlurType::Hexagon:
		return "hexagon";
	case BlurType::Zoom:
		return "zoom";
	case BlurType::Motion:
		return "motion";
	}
	return "";
}

} // namespace stagekit
