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

#include "../common/filter-input.hpp"
#include "ramp-blur.hpp"
#include "blur-pipeline.hpp"
#include "../common/timing.hpp"

#include "../api/vendor-api.hpp"
#include "../common/easing.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/tooltip.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace stagekit {

namespace {

struct Settings {
	BlurLook look;
	BlurRamp ramp;
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
		return settings_.look.type;
	}
	PixelShape pixel_shape() const
	{
		std::lock_guard lock(mutex_);
		return settings_.look.pixel_shape;
	}
	bool include_in_all() const
	{
		std::lock_guard lock(mutex_);
		return settings_.include_in_all;
	}
	obs_source_t *source() const { return source_; }

private:
	bool capture_input(uint32_t w, uint32_t h);

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

	// The blur itself, shared with Scene Blur.
	std::unique_ptr<BlurPipeline> blur_;
	gs_texrender_t *tr_input_ = nullptr;
};

// ------------------------------------------------------------------ setup

RampBlur::RampBlur(obs_data_t *settings, obs_source_t *source) : source_(source)
{
	blur_ = std::make_unique<BlurPipeline>();
	obs_enter_graphics();
	tr_input_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	obs_leave_graphics();

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

	blur_.reset();
	obs_enter_graphics();
	gs_texrender_destroy(tr_input_);
	obs_leave_graphics();
}

void RampBlur::update(obs_data_t *settings)
{
	Settings s;
	s.look = read_blur_look(settings);
	s.ramp = read_blur_ramp(settings);
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
					       : (active ? settings_.ramp.in_s : settings_.ramp.out_s);
	// Reversing part-way through a ramp only takes the remaining distance,
	// so the speed stays consistent.
	const double distance = std::clamp(std::fabs(target - level_), 0.0, 1.0);
	tween_.start(level_, target, full * distance, settings_.ramp.curve, settings_.ramp.mode);
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
bool find_live(obs_scene_t *, obs_sceneitem_t *item, void *param);

// The live captures one source shows (see live_captures_in).
void collect_live(obs_source_t *source, LiveSearch *search)
{
	if (obs_scene_t *inner = scene_of(source)) {
		if (search->depth < 8) {
			LiveSearch deeper{search->found, search->depth + 1};
			obs_scene_enum_items(inner, find_live, &deeper);
		}
	} else if (is_capture_source(source) && obs_source_get_width(source) > 0 &&
		   std::find(search->found->begin(), search->found->end(), source) == search->found->end()) {
		search->found->push_back(source);
	} else if (const char *id = obs_source_get_unversioned_id(source);
		   id && strcmp(id, "stagekit_cropped_view") == 0) {
		// A capture on screen by way of a Cropped View (the owner's camera
		// is only ever shown through its crop) is a live capture too: it is
		// the capture that is blurred, and the view draws it blurred.
		obs_data_t *view = obs_source_get_settings(source);
		obs_source_t *shown = obs_get_source_by_name(obs_data_get_string(view, "source"));
		obs_data_release(view);
		// (Held by the scene's view for as long as this search is used.)
		if (shown && is_capture_source(shown) && obs_source_get_width(shown) > 0 &&
		    std::find(search->found->begin(), search->found->end(), shown) == search->found->end())
			search->found->push_back(shown);
		// (Or a scene or group holding it.)
		if (obs_scene_t *inner = shown ? scene_of(shown) : nullptr; inner && search->depth < 8) {
			LiveSearch deeper{search->found, search->depth + 1};
			obs_scene_enum_items(inner, find_live, &deeper);
		}
		obs_source_release(shown);
	}
}

bool find_live(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	obs_source_t *source = obs_sceneitem_get_source(item);
	if (source && obs_sceneitem_visible(item))
		collect_live(source, static_cast<LiveSearch *>(param));
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
	// Captures that "And live captures on screen" is to leave alone: a
	// camera, say. The owner: "there should be an exclusion option ... so
	// cameras for example can be intentionally excluded from blur".
	std::vector<std::string> never = pick_list_read(settings, "never", false);
	// A Cropped View (a camera's crop, say) is blurred by way of the source it
	// shows: leaving the view alone means leaving that source alone too.
	for (size_t i = 0, n = never.size(); i < n; i++) {
		obs_source_t *named = obs_get_source_by_name(never[i].c_str());
		const char *id = named ? obs_source_get_unversioned_id(named) : nullptr;
		if (id && strcmp(id, "stagekit_cropped_view") == 0) {
			obs_data_t *view = obs_source_get_settings(named);
			const std::string shows = obs_data_get_string(view, "source");
			obs_data_release(view);
			if (!shows.empty())
				never.push_back(shows);
		}
		obs_source_release(named);
	}
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
			const char *name = obs_source_get_name(source);
			if (source == parent || (name && std::find(never.begin(), never.end(), name) != never.end()))
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
			for (const char *own : {"also", "also_pick", "also_captures", "never"})
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
	blur_->frame_passed(seconds);
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
	// (Afresh at each drawing: see filter-input.hpp.)
	return draw_filter_input(source_, tr_input_, w, h);
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

	double strength, mix;
	blur_at_level(s.look, s.ramp.style, level, strength, mix);

	// Fully off costs nothing: hand rendering straight back to OBS.
	if (!blur_->ready() || level <= 0.0005 || strength <= 0.01 || mix <= 0.0005) {
		blur_->forget_trail(); // a trail starts afresh when the blur comes back
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

	const BlurPipeline::Result blurred = blur_->run(s.look, input, w, h, strength, mix);
	blur_->composite(input, blurred, w, h, mix);
}

// ------------------------------------------------------------- properties

namespace {

obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	add_blur_look_properties(props);
	add_blur_ramp_properties(props);

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
	set_tip(live, obs_module_text("RampBlur.AlsoCaptures.Tip"));
	// The captures that tick box is to leave alone.
	PickList never;
	never.setting = "never";
	never.title = "RampBlur.Never";
	never.add = "RampBlur.NeverPick";
	never.tip = "RampBlur.Never.Tip";
	never.choices = [own_filter](std::vector<std::string> &names) {
		// Every source with a picture (not only the kinds counted as
		// captures): a camera may come from a plugin of its own, and a
		// Cropped View of it is how it is often shown.
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				const char *name = obs_source_get_name(source);
				const char *id = obs_source_get_unversioned_id(source);
				const bool video = (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) != 0;
				if (name && *name && video && id && strcmp(id, "scene") != 0 &&
				    strcmp(id, "group") != 0)
					static_cast<std::vector<std::string> *>(param)->push_back(name);
				return true;
			},
			&names);
		std::sort(names.begin(), names.end());
		obs_source_t *parent = own_filter ? obs_filter_get_parent(own_filter) : nullptr;
		const char *own = parent ? obs_source_get_name(parent) : nullptr;
		if (own)
			names.erase(std::remove(names.begin(), names.end(), std::string(own)), names.end());
	};
	pick_list_add(props, own_filter, never);

	obs_properties_t *keys = obs_properties_create();
	obs_property_t *in_all =
		obs_properties_add_bool(keys, "include_in_all", obs_module_text("RampBlur.IncludeInAll"));
	set_tip(in_all, obs_module_text("RampBlur.Help"));
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
	blur_look_defaults(settings);
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

void live_captures_in(obs_source_t *source, std::vector<obs_source_t *> &found)
{
	if (!source)
		return;
	LiveSearch search{&found, 0};
	collect_live(source, &search);
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
	return blur_type_name(blur->type(), blur->pixel_shape());
}

} // namespace stagekit
