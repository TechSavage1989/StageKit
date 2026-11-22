/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// obs-websocket vendor requests. Clients call them with:
//
//   { "op": 6, "d": { "requestType": "CallVendorRequest", "requestId": "1",
//     "requestData": { "vendorName": "StageKit", "requestType": "BlurToggle",
//                      "requestData": { "sourceName": "Game Capture" } } } }
//
// Every response carries "success" (bool) and, on failure, "error".

#include "vendor-api.hpp"
#include "../countdown/countdown.hpp"

#include "../blur/ramp-blur.hpp"
#include "../capture/auto-capture.hpp"
#include "../capture/standby.hpp"
#include "../dock/arrange-tools.hpp"
#include "../frame/auto-frame.hpp"
#include "../look/scene-blur.hpp"
#include "../placement/show-in-scenes.hpp"
#include "../placement/scene-modes.hpp"
#include "../third-party/obs-websocket-api.h"

#include <obs-module.h>
#include <plugin-support.h>
#include <callback/proc.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <mutex>
#include <map>

namespace stagekit {

namespace {

std::atomic<obs_websocket_vendor> g_vendor{nullptr};

const char *const kRequests[] = {"GetVersion",        "ListBlurFilters", "BlurOn",       "BlurOff",
				 "BlurToggle",        "SetBlur",         "BlurAll",      "HookFront",
				 "SetPictureShape",   "GetOptions",      "SetOption",    "Slide",
				 "GetSwitcherState",  "ListSceneModes",  "GetSceneMode", "SetSceneMode",
				 "ListSceneBlurs",    "SceneBlur",       "StandbyCover", "GetStandbyCover",
				 "ShowInScenesPress", "ListTimers",      "TimerControl", "AutoFrameCommand",
				 "GetAutoFrameState"};

void fail(obs_data_t *response, const char *message)
{
	obs_data_set_bool(response, "success", false);
	obs_data_set_string(response, "error", message);
}

// Finds the Ramp Blur named by "sourceName" and optional "filterName" (the
// first Ramp Blur on the source when omitted). Returns a strong reference
// to the filter that the caller must release, or nullptr after writing an
// error into the response.
obs_source_t *find_blur(obs_data_t *request, obs_data_t *response, RampBlur **out)
{
	const char *source_name = obs_data_get_string(request, "sourceName");
	const char *filter_name = obs_data_get_string(request, "filterName");
	if (!source_name || !*source_name) {
		fail(response, "sourceName is required");
		return nullptr;
	}

	obs_source_t *source = obs_get_source_by_name(source_name);
	if (!source) {
		fail(response, "No source with that sourceName");
		return nullptr;
	}

	obs_source_t *filter = nullptr;
	if (filter_name && *filter_name) {
		filter = obs_source_get_filter_by_name(source, filter_name);
	} else {
		obs_source_enum_filters(
			source,
			[](obs_source_t *, obs_source_t *child, void *param) {
				auto **found = static_cast<obs_source_t **>(param);
				if (!*found && ramp_blur_from_source(child))
					*found = obs_source_get_ref(child);
			},
			&filter);
	}
	obs_source_release(source);

	RampBlur *blur = ramp_blur_from_source(filter);
	if (!blur) {
		obs_source_release(filter);
		fail(response, filter_name && *filter_name ? "That filter is not a StageKit Ramp Blur"
							   : "Source has no StageKit Ramp Blur filter");
		return nullptr;
	}
	*out = blur;
	return filter;
}

void write_blur_state(obs_data_t *data, RampBlur *blur, obs_source_t *filter)
{
	obs_source_t *parent = obs_filter_get_parent(filter);
	obs_data_set_string(data, "sourceName", parent ? obs_source_get_name(parent) : "");
	obs_data_set_string(data, "filterName", obs_source_get_name(filter));
	obs_data_set_bool(data, "active", ramp_blur_is_active(blur));
	obs_data_set_double(data, "level", ramp_blur_level(blur));
	obs_data_set_string(data, "blurType", ramp_blur_type_name(blur));
}

void req_get_version(obs_data_t *, obs_data_t *response, void *)
{
	obs_data_set_bool(response, "success", true);
	obs_data_set_string(response, "version", PLUGIN_VERSION);
	obs_data_array_t *features = obs_data_array_create();
	obs_data_t *f = obs_data_create();
	obs_data_set_string(f, "name", "ramp-blur");
	obs_data_array_push_back(features, f);
	obs_data_release(f);
	obs_data_set_array(response, "features", features);
	obs_data_array_release(features);
}

void req_list_blur_filters(obs_data_t *, obs_data_t *response, void *)
{
	obs_data_array_t *list = obs_data_array_create();
	for_each_ramp_blur([&](RampBlur *blur, obs_source_t *filter) {
		if (!obs_filter_get_parent(filter))
			return;
		obs_data_t *item = obs_data_create();
		write_blur_state(item, blur, filter);
		obs_data_array_push_back(list, item);
		obs_data_release(item);
	});
	obs_data_set_array(response, "filters", list);
	obs_data_array_release(list);
	obs_data_set_bool(response, "success", true);
}

template<typename Action> void with_blur(obs_data_t *request, obs_data_t *response, Action action)
{
	RampBlur *blur = nullptr;
	obs_source_t *filter = find_blur(request, response, &blur);
	if (!filter)
		return;
	action(blur);
	write_blur_state(response, blur, filter);
	obs_data_set_bool(response, "success", true);
	obs_source_release(filter);
}

// Optional "durationMs" overrides the filter's own ramp time for one call.
double duration_from(obs_data_t *request)
{
	return obs_data_has_user_value(request, "durationMs") ? obs_data_get_double(request, "durationMs") : -1.0;
}

void req_blur_on(obs_data_t *request, obs_data_t *response, void *)
{
	with_blur(request, response, [&](RampBlur *b) { ramp_blur_set_active(b, true, duration_from(request)); });
}

void req_blur_off(obs_data_t *request, obs_data_t *response, void *)
{
	with_blur(request, response, [&](RampBlur *b) { ramp_blur_set_active(b, false, duration_from(request)); });
}

void req_blur_toggle(obs_data_t *request, obs_data_t *response, void *)
{
	with_blur(request, response,
		  [&](RampBlur *b) { ramp_blur_set_active(b, !ramp_blur_is_active(b), duration_from(request)); });
}

void req_set_blur(obs_data_t *request, obs_data_t *response, void *)
{
	if (!obs_data_has_user_value(request, "active")) {
		fail(response, "active (bool) is required");
		return;
	}
	const bool active = obs_data_get_bool(request, "active");
	with_blur(request, response, [&](RampBlur *b) { ramp_blur_set_active(b, active, duration_from(request)); });
}

// "mode": "on", "off" or "toggle" (default). Affects every Ramp Blur with
// "Include in Blur All" ticked.
void req_blur_all(obs_data_t *request, obs_data_t *response, void *)
{
	const char *mode = obs_data_get_string(request, "mode");
	BlurAllMode m = BlurAllMode::Toggle;
	if (strcmp(mode, "on") == 0)
		m = BlurAllMode::On;
	else if (strcmp(mode, "off") == 0)
		m = BlurAllMode::Off;
	else if (*mode && strcmp(mode, "toggle") != 0) {
		fail(response, "mode must be on, off or toggle");
		return;
	}
	obs_data_set_bool(response, "active", ramp_blur_set_all(m));
	obs_data_set_bool(response, "success", true);
}

// "sourceName", "shape" ("4:3", "8:7", a number, or "auto"), "untilGone"
// (bool, true unless given): the picture's shape, set by hand on the source's
// Auto Frame; with untilGone it is dropped when that game's capture ends.
void req_set_picture_shape(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "sourceName");
	obs_source_t *source = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!source) {
		fail(response, "No source with that sourceName");
		return;
	}
	const bool until_gone = !obs_data_has_user_value(request, "untilGone") ||
				obs_data_get_bool(request, "untilGone");
	std::string error;
	if (set_picture_shape_on(source, obs_data_get_string(request, "shape"), until_gone, error))
		obs_data_set_bool(response, "success", true);
	else
		fail(response, error.c_str());
	obs_source_release(source);
}

// A measurement belongs to the explicitly selected Auto Frame filter.
obs_source_t *find_auto_frame(obs_data_t *request, obs_data_t *response)
{
	const char *source_name = obs_data_get_string(request, "sourceName");
	const char *filter_name = obs_data_get_string(request, "filterName");
	if (!*source_name || !*filter_name) {
		fail(response, "sourceName and filterName are required");
		return nullptr;
	}
	obs_source_t *source = obs_get_source_by_name(source_name);
	obs_source_t *filter = source ? obs_source_get_filter_by_name(source, filter_name) : nullptr;
	obs_source_release(source);
	const char *id = filter ? obs_source_get_unversioned_id(filter) : nullptr;
	if (!id || std::strcmp(id, kAutoFrameId) != 0) {
		obs_source_release(filter);
		fail(response, "No Auto Frame filter with that sourceName and filterName");
		return nullptr;
	}
	return filter;
}

void write_auto_frame_state(obs_data_t *response, const AutoFrameState &state)
{
	obs_data_set_bool(response, "success", state.success);
	obs_data_set_int(response, "requestId", state.request_id);
	obs_data_set_string(response, "status", state.status.c_str());
	obs_data_set_bool(response, "complete", state.complete);
	obs_data_set_string(response, "program", state.program.c_str());
	obs_data_set_string(response, "executable", state.executable.c_str());
	obs_data_set_string(response, "aspectRatio", state.aspect_ratio.c_str());
	obs_data_set_double(response, "ratio", state.ratio);
	obs_data_set_int(response, "width", state.width);
	obs_data_set_int(response, "height", state.height);
	obs_data_set_bool(response, "saved", state.saved);
	obs_data_set_bool(response, "supportsRememberBy", state.supports_remember_by);
	if (!state.remember_by.empty()) {
		obs_data_set_string(response, "rememberBy", state.remember_by.c_str());
		obs_data_set_string(response, "rememberTarget", state.remember_target.c_str());
	}
	if (!state.error.empty())
		obs_data_set_string(response, "error", state.error.c_str());
}

// {sourceName, filterName, action: measure | remember | measure_and_remember}
void req_auto_frame_command(obs_data_t *request, obs_data_t *response, void *)
{
	const std::string action = obs_data_get_string(request, "action");
	if (action != "measure" && action != "remember" && action != "measure_and_remember") {
		fail(response, "action must be measure, remember or measure_and_remember");
		return;
	}
	std::string remember_by;
	if (obs_data_has_user_value(request, "rememberBy")) {
		obs_data_item_t *item = obs_data_item_byname(request, "rememberBy");
		const bool string = item && obs_data_item_gettype(item) == OBS_DATA_STRING;
		obs_data_item_release(&item);
		remember_by = obs_data_get_string(request, "rememberBy");
		if (!string || (remember_by != "application" && remember_by != "window")) {
			fail(response, "rememberBy must be application or window");
			return;
		}
	}
	int64_t timeout_ms = 4000;
	if (obs_data_has_user_value(request, "timeoutMs")) {
		obs_data_item_t *item = obs_data_item_byname(request, "timeoutMs");
		const bool integer = item && obs_data_item_gettype(item) == OBS_DATA_NUMBER &&
				     obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
		obs_data_item_release(&item);
		timeout_ms = obs_data_get_int(request, "timeoutMs");
		if (!integer || timeout_ms < 1 || timeout_ms > 4000) {
			fail(response, "timeoutMs must be an integer from 1 to 4000");
			return;
		}
	}
	obs_source_t *filter = find_auto_frame(request, response);
	if (!filter)
		return;
	AutoFrameState state;
	state.success = auto_frame_command(filter, action, state, remember_by, timeout_ms);
	write_auto_frame_state(response, state);
	obs_source_release(filter);
}

// A supplied ID must still identify this filter's command, never a replacement operation.
void req_get_auto_frame_state(obs_data_t *request, obs_data_t *response, void *)
{
	int64_t request_id = 0;
	if (obs_data_has_user_value(request, "requestId")) {
		obs_data_item_t *item = obs_data_item_byname(request, "requestId");
		const bool integer = item && obs_data_item_gettype(item) == OBS_DATA_NUMBER &&
				     obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
		obs_data_item_release(&item);
		request_id = obs_data_get_int(request, "requestId");
		if (!integer || request_id <= 0 || request_id > 9007199254740991LL) {
			fail(response, "requestId must be a positive safe integer");
			return;
		}
	}
	obs_source_t *filter = find_auto_frame(request, response);
	if (!filter)
		return;
	AutoFrameState state;
	state.success = auto_frame_state(filter, request_id, state);
	write_auto_frame_state(response, state);
	obs_source_release(filter);
}

// "sourceName": a Game Capture or a Window Capture. Aims it at the window in
// front right now and switches an Auto Capture on it off. "transition" (an
// OBS transition's id, or "none") and "transitionMs" make the change with a
// transition: out, aimed, back in.
void req_hook_front(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "sourceName");
	obs_source_t *source = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!source) {
		fail(response, "No source with that sourceName");
		return;
	}
	std::string what, error;
	const std::string transition = obs_data_get_string(request, "transition");
	const uint32_t transition_ms =
		(uint32_t)std::clamp<long long>(obs_data_get_int(request, "transitionMs"), 0, 10000);
	if (hook_front_window(source, what, error, transition, transition_ms)) {
		obs_data_set_bool(response, "success", true);
		obs_data_set_string(response, "executable", what.c_str());
	} else {
		fail(response, error.c_str());
	}
	obs_source_release(source);
}

// The dock's switches a key can hold: its name here, and the option.
bool *option_named(ArrangeOptions &o, const std::string &name)
{
	if (name == "snap")
		return &o.live_snap;
	if (name == "guides")
		return &o.guides;
	if (name == "limit")
		return &o.limit_resize;
	if (name == "slide")
		return &o.send_slide;
	if (name == "everyScene")
		return &o.send_every_scene;
	if (name == "keepShape")
		return &o.keep_shape;
	return nullptr;
}

void write_options(obs_data_t *response)
{
	ArrangeOptions o = arrange_options();
	for (const char *name : {"snap", "guides", "limit", "slide", "everyScene", "keepShape"})
		obs_data_set_bool(response, name, *option_named(o, name));
}

// What a switcher said last, by filter (the key: its address).
std::mutex g_switch_mutex;
std::map<const void *, std::pair<bool, bool>> g_switch;

// {sourceName, filterName} -> {manual, hidden}: is the switcher on a manual
// window, and is its capture hidden.
void req_get_switcher_state(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	obs_source_t *filter =
		source ? obs_source_get_filter_by_name(source, obs_data_get_string(request, "filterName")) : nullptr;
	obs_source_release(source);
	if (!filter) {
		fail(response, "No such source and filter");
		return;
	}
	bool manual = false, hidden = false;
	{
		std::lock_guard lock(g_switch_mutex);
		const auto said = g_switch.find(filter);
		if (said != g_switch.end()) {
			manual = said->second.first;
			hidden = said->second.second;
		}
	}
	obs_source_release(filter);
	obs_data_set_bool(response, "manual", manual);
	obs_data_set_bool(response, "hidden", hidden);
	obs_data_set_bool(response, "success", true);
}

void req_get_options(obs_data_t *, obs_data_t *response, void *)
{
	write_options(response);
	obs_data_set_bool(response, "success", true);
}

// { option, mode?: "toggle" (as it comes) | "on" | "off" }
void req_set_option(obs_data_t *request, obs_data_t *response, void *)
{
	ArrangeOptions o = arrange_options();
	bool *option = option_named(o, obs_data_get_string(request, "option"));
	if (!option) {
		fail(response, "option must be snap, guides, limit, slide, everyScene or keepShape");
		return;
	}
	const std::string mode = obs_data_get_string(request, "mode");
	*option = mode == "on" ? true : mode == "off" ? false : !*option;
	const bool on = *option;
	set_arrange_options(o);
	obs_data_set_bool(response, "on", on);
	obs_data_set_bool(response, "success", true);
}

// { direction: "up", "down-left", "centre", ... }
void req_slide(obs_data_t *request, obs_data_t *response, void *)
{
	static const char *const kDirections[] = {"up-left", "up",        "up-right", "left",      "centre",
						  "right",   "down-left", "down",     "down-right"};
	const std::string wanted = obs_data_get_string(request, "direction");
	for (int i = 0; i < 9; i++) {
		if (wanted != kDirections[i])
			continue;
		if (request_slide(i))
			obs_data_set_bool(response, "success", true);
		else
			fail(response, "The StageKit dock is not there to slide with");
		return;
	}
	fail(response, "direction must be up, down, left, right, up-left, up-right, down-left, down-right or centre");
}

// ---- Scene Modes

void write_mode_list(obs_data_t *into, const SceneModesLook &look)
{
	obs_data_array_t *modes = obs_data_array_create();
	for (const SceneModeLook &mode : look.modes) {
		obs_data_t *m = obs_data_create();
		obs_data_set_string(m, "id", mode.id.c_str());
		obs_data_set_string(m, "name", mode.name.c_str());
		obs_data_set_string(m, "type", mode.type.c_str());
		obs_data_array_push_back(modes, m);
		obs_data_release(m);
	}
	obs_data_set_array(into, "modes", modes);
	obs_data_array_release(modes);
}

std::string mode_name(const SceneModesLook &look, const std::string &id)
{
	for (const SceneModeLook &mode : look.modes)
		if (mode.id == id)
			return mode.name;
	return {};
}

// { scene? } -> { filters: [{ scene, filter, enabled, current, currentName,
// allowNone, modes: [{ id, name, type }], scenes: [...] }] }
void req_list_scene_modes(obs_data_t *request, obs_data_t *response, void *)
{
	obs_data_array_t *filters = obs_data_array_create();
	for (const SceneModesLook &look : scene_modes_list(obs_data_get_string(request, "scene"))) {
		obs_data_t *f = obs_data_create();
		obs_data_set_string(f, "scene", look.scene.c_str());
		obs_data_set_string(f, "filter", look.filter.c_str());
		obs_data_set_bool(f, "enabled", look.enabled);
		obs_data_set_string(f, "current", look.current.c_str());
		obs_data_set_string(f, "currentName", mode_name(look, look.current).c_str());
		obs_data_set_bool(f, "allowNone", look.allow_none);
		write_mode_list(f, look);
		obs_data_array_t *scenes = obs_data_array_create();
		for (const std::string &name : look.scenes) {
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "name", name.c_str());
			obs_data_array_push_back(scenes, s);
			obs_data_release(s);
		}
		obs_data_set_array(f, "scenes", scenes);
		obs_data_array_release(scenes);
		obs_data_array_push_back(filters, f);
		obs_data_release(f);
	}
	obs_data_set_array(response, "filters", filters);
	obs_data_array_release(filters);
	obs_data_set_bool(response, "success", true);
}

// { scene, filter? } -> { current, currentName, previous, previousName,
// enabled, allowNone, modes }
void req_get_scene_mode(obs_data_t *request, obs_data_t *response, void *)
{
	const std::string scene = obs_data_get_string(request, "scene");
	const std::string filter = obs_data_get_string(request, "filter");
	if (scene.empty()) {
		fail(response, "scene is required");
		return;
	}
	for (const SceneModesLook &look : scene_modes_list(scene.c_str())) {
		if (!filter.empty() && look.filter != filter)
			continue;
		obs_data_set_string(response, "filter", look.filter.c_str());
		obs_data_set_string(response, "current", look.current.c_str());
		obs_data_set_string(response, "currentName", mode_name(look, look.current).c_str());
		obs_data_set_string(response, "previous", look.previous.c_str());
		obs_data_set_string(response, "previousName", mode_name(look, look.previous).c_str());
		obs_data_set_bool(response, "enabled", look.enabled);
		obs_data_set_bool(response, "allowNone", look.allow_none);
		write_mode_list(response, look);
		obs_data_set_bool(response, "success", true);
		return;
	}
	fail(response, filter.empty() ? "No Scene Modes on that scene" : "No Scene Modes with that name on that scene");
}

// { scene, filter?, mode (an id, or a name), how?: "on" | "toggle" | "next" | "none" }
void req_set_scene_mode(obs_data_t *request, obs_data_t *response, void *)
{
	std::string why;
	if (scene_modes_press(obs_data_get_string(request, "scene"), obs_data_get_string(request, "filter"),
			      obs_data_get_string(request, "mode"), obs_data_get_string(request, "how"), why))
		obs_data_set_bool(response, "success", true);
	else
		fail(response, why.c_str());
}

// ---- Scene Blur

void write_blur_state(obs_data_t *into, const SceneBlurState &state)
{
	obs_data_set_string(into, "scene", state.scene.c_str());
	obs_data_set_string(into, "filter", state.filter.c_str());
	obs_data_set_string(into, "type", state.type.c_str());
	obs_data_set_string(into, "style", state.style.c_str());
	obs_data_set_bool(into, "active", state.active);
	obs_data_set_bool(into, "enabled", state.enabled);
}

// { scene? } -> { filters: [{ filter, type, active, scenes: [{ scene, active }] }],
// blurs: [{ scene, filter, type, active, enabled }] }: each name once, with
// the scenes it is on, and each filter.
void req_list_scene_blurs(obs_data_t *request, obs_data_t *response, void *)
{
	const std::vector<SceneBlurState> all = scene_blur_list(obs_data_get_string(request, "scene"), "");
	obs_data_array_t *blurs = obs_data_array_create();
	std::vector<std::string> names;
	for (const SceneBlurState &state : all) {
		obs_data_t *b = obs_data_create();
		write_blur_state(b, state);
		obs_data_array_push_back(blurs, b);
		obs_data_release(b);
		if (std::find(names.begin(), names.end(), state.filter) == names.end())
			names.push_back(state.filter);
	}
	std::sort(names.begin(), names.end());
	obs_data_array_t *filters = obs_data_array_create();
	for (const std::string &name : names) {
		obs_data_t *f = obs_data_create();
		obs_data_set_string(f, "filter", name.c_str());
		obs_data_array_t *scenes = obs_data_array_create();
		bool any = false;
		std::string type, style;
		for (const SceneBlurState &state : all) {
			if (state.filter != name)
				continue;
			obs_data_t *s = obs_data_create();
			obs_data_set_string(s, "scene", state.scene.c_str());
			obs_data_set_bool(s, "active", state.active);
			obs_data_array_push_back(scenes, s);
			obs_data_release(s);
			any = any || state.active;
			if (type.empty()) {
				type = state.type;
				style = state.style;
			}
		}
		obs_data_set_string(f, "type", type.c_str());
		obs_data_set_string(f, "style", style.c_str());
		obs_data_set_bool(f, "active", any);
		obs_data_set_array(f, "scenes", scenes);
		obs_data_array_release(scenes);
		obs_data_array_push_back(filters, f);
		obs_data_release(f);
	}
	obs_data_set_array(response, "filters", filters);
	obs_data_set_array(response, "blurs", blurs);
	obs_data_array_release(filters);
	obs_data_array_release(blurs);
	obs_data_set_bool(response, "success", true);
}

// { filter, scene? ("" or none: every scene with that filter), how?: "on" |
// "off" | "toggle" (none: only asks) } -> { active, blurs: [...] }
void req_scene_blur(obs_data_t *request, obs_data_t *response, void *)
{
	std::vector<SceneBlurState> out;
	bool active = false;
	std::string why;
	if (!scene_blur_press(obs_data_get_string(request, "scene"), obs_data_get_string(request, "filter"),
			      obs_data_get_string(request, "how"), out, active, why)) {
		fail(response, why.c_str());
		return;
	}
	obs_data_array_t *blurs = obs_data_array_create();
	for (const SceneBlurState &state : out) {
		obs_data_t *b = obs_data_create();
		write_blur_state(b, state);
		obs_data_array_push_back(blurs, b);
		obs_data_release(b);
	}
	obs_data_set_array(response, "blurs", blurs);
	obs_data_array_release(blurs);
	obs_data_set_bool(response, "active", active);
	obs_data_set_bool(response, "success", true);
}

// { sourceName, filterName? (none: the first enabled Standby Image), how?:
// "on" | "off" | "toggle" (none: only asks) } -> { filterName, covered,
// enabled }: the Standby Image shown over the source on purpose, or taken
// away, with that filter's own transition. For any source: a camera, a
// capture card, a display capture.
void standby_cover_request(obs_data_t *request, obs_data_t *response, const std::string &how)
{
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	std::string name, why;
	bool covering = false, enabled = false;
	const bool ok = standby_cover_press(source, obs_data_get_string(request, "filterName"), how, name, covering,
					    enabled, why);
	obs_source_release(source);
	if (!ok) {
		fail(response, why.c_str());
		return;
	}
	obs_data_set_string(response, "filterName", name.c_str());
	obs_data_set_bool(response, "covered", covering);
	obs_data_set_bool(response, "enabled", enabled);
	obs_data_set_bool(response, "success", true);
}

void req_standby_cover(obs_data_t *request, obs_data_t *response, void *)
{
	standby_cover_request(request, response, obs_data_get_string(request, "how"));
}

void req_get_standby_cover(obs_data_t *request, obs_data_t *response, void *)
{
	standby_cover_request(request, response, "");
}

// { sourceName, filterName, how: "toggle" | "on" | "off", hideAs?: "filter"
// (none) | "hide" | "cover" } -> { enabled }: a Show in Scenes switched as a
// key switches it, that press hiding its sources the way the key says.
void req_show_in_scenes_press(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *source = obs_get_source_by_name(obs_data_get_string(request, "sourceName"));
	bool enabled = false;
	std::string why;
	const bool ok = show_in_scenes_press(source, obs_data_get_string(request, "filterName"),
					     obs_data_get_string(request, "how"),
					     obs_data_get_string(request, "hideAs"), enabled, why);
	obs_source_release(source);
	if (!ok) {
		fail(response, why.c_str());
		return;
	}
	obs_data_set_bool(response, "enabled", enabled);
	obs_data_set_bool(response, "success", true);
}

// What was emitted, for the tests: one JSON object per line.
std::mutex g_events_mutex;
std::string g_events;
bool g_noting_events = false;

void emit_event(const char *name, obs_data_t *data)
{
	{
		std::lock_guard lock(g_events_mutex);
		if (g_noting_events) {
			obs_data_t *line = obs_data_create();
			obs_data_set_string(line, "event", name);
			obs_data_set_obj(line, "data", data);
			g_events += obs_data_get_json(line);
			g_events += "\n";
			obs_data_release(line);
		}
	}
	if (obs_websocket_vendor vendor = g_vendor.load())
		obs_websocket_vendor_emit_event(vendor, name, data);
}

} // namespace

void vendor_api_register()
{
	if (obs_websocket_get_api_version() == 0) {
		obs_log(LOG_INFO, "obs-websocket not found; Stream Deck / remote control requests are disabled");
		return;
	}
	obs_websocket_vendor vendor = obs_websocket_register_vendor("StageKit");
	if (!vendor) {
		obs_log(LOG_WARNING, "Could not register the StageKit obs-websocket vendor");
		return;
	}

	struct {
		const char *name;
		obs_websocket_request_callback_function fn;
	} requests[] = {
		{"GetVersion", req_get_version},
		{"ListBlurFilters", req_list_blur_filters},
		{"BlurOn", req_blur_on},
		{"BlurOff", req_blur_off},
		{"BlurToggle", req_blur_toggle},
		{"SetBlur", req_set_blur},
		{"BlurAll", req_blur_all},
		{"HookFront", req_hook_front},
		{"SetPictureShape", req_set_picture_shape},
		{"AutoFrameCommand", req_auto_frame_command},
		{"GetAutoFrameState", req_get_auto_frame_state},
		{"GetOptions", req_get_options},
		{"SetOption", req_set_option},
		{"Slide", req_slide},
		{"GetSwitcherState", req_get_switcher_state},
		{"ListSceneModes", req_list_scene_modes},
		{"GetSceneMode", req_get_scene_mode},
		{"SetSceneMode", req_set_scene_mode},
		{"ListSceneBlurs", req_list_scene_blurs},
		{"SceneBlur", req_scene_blur},
		{"StandbyCover", req_standby_cover},
		{"GetStandbyCover", req_get_standby_cover},
		{"ShowInScenesPress", req_show_in_scenes_press},
		{"ListTimers", timer_vendor_list},
		{"TimerControl", timer_vendor_control},
	};
	for (const auto &r : requests) {
		if (!obs_websocket_vendor_register_request(vendor, r.name, r.fn, nullptr))
			obs_log(LOG_WARNING, "Could not register vendor request %s", r.name);
	}
	g_vendor = vendor;
	obs_log(LOG_INFO, "obs-websocket vendor 'StageKit' registered");
}

void vendor_api_unregister()
{
	obs_websocket_vendor vendor = g_vendor.exchange(nullptr);
	if (!vendor)
		return;
	for (const char *name : kRequests)
		obs_websocket_vendor_unregister_request(vendor, name);
}

void vendor_emit_blur_changed(obs_source_t *filter, bool active)
{
	obs_websocket_vendor vendor = g_vendor.load();
	if (!vendor || !filter)
		return;
	obs_data_t *data = obs_data_create();
	obs_source_t *parent = obs_filter_get_parent(filter);
	obs_data_set_string(data, "sourceName", parent ? obs_source_get_name(parent) : "");
	obs_data_set_string(data, "filterName", obs_source_get_name(filter));
	obs_data_set_bool(data, "active", active);
	obs_websocket_vendor_emit_event(vendor, "BlurChanged", data);
	obs_data_release(data);
}

void vendor_switch_state(obs_source_t *filter, bool manual, bool hidden)
{
	if (!filter)
		return;
	{
		std::lock_guard lock(g_switch_mutex);
		auto &said = g_switch[filter];
		const bool known = g_switch.size() && said.first == manual && said.second == hidden;
		// (A new entry starts at false, false: that is "nothing changed" too.)
		if (known)
			return;
		said = {manual, hidden};
	}
	obs_websocket_vendor vendor = g_vendor.load();
	if (!vendor)
		return;
	obs_data_t *data = obs_data_create();
	obs_source_t *parent = obs_filter_get_parent(filter);
	obs_data_set_string(data, "sourceName", parent ? obs_source_get_name(parent) : "");
	obs_data_set_string(data, "filterName", obs_source_get_name(filter));
	obs_data_set_bool(data, "manual", manual);
	obs_data_set_bool(data, "hidden", hidden);
	obs_websocket_vendor_emit_event(vendor, "SwitcherChanged", data);
	obs_data_release(data);
}

void vendor_switch_forget(obs_source_t *filter)
{
	std::lock_guard lock(g_switch_mutex);
	g_switch.erase(filter);
}

void vendor_emit_timer_changed(obs_source_t *timer)
{
	if (!timer || !g_vendor.load())
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "timer", obs_obj_is_private(timer) ? "private" : obs_source_get_uuid(timer));
	emit_event("TimerChanged", data);
	obs_data_release(data);
}

void vendor_emit_options_changed()
{
	obs_websocket_vendor vendor = g_vendor.load();
	if (!vendor)
		return;
	obs_data_t *data = obs_data_create();
	write_options(data);
	obs_websocket_vendor_emit_event(vendor, "OptionsChanged", data);
	obs_data_release(data);
}

} // namespace stagekit

namespace stagekit {

void vendor_emit_scene_mode_changed(obs_source_t *filter, const std::string &mode, const std::string &previous)
{
	if (!filter)
		return;
	obs_source_t *parent = obs_filter_get_parent(filter);
	const std::string scene = parent && obs_source_get_name(parent) ? obs_source_get_name(parent) : "";
	std::string mode_text, previous_text;
	for (const SceneModesLook &look : scene_modes_list(scene.c_str()))
		if (look.filter == obs_source_get_name(filter)) {
			mode_text = mode_name(look, mode);
			previous_text = mode_name(look, previous);
		}
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "scene", scene.c_str());
	obs_data_set_string(data, "filter", obs_source_get_name(filter));
	obs_data_set_string(data, "mode", mode.c_str());
	obs_data_set_string(data, "modeName", mode_text.c_str());
	obs_data_set_string(data, "previous", previous.c_str());
	obs_data_set_string(data, "previousName", previous_text.c_str());
	emit_event("SceneModeChanged", data);
	obs_data_release(data);
}

void vendor_emit_scene_blur_changed(obs_source_t *filter, bool active)
{
	if (!filter)
		return;
	obs_source_t *parent = obs_filter_get_parent(filter);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "scene", parent && obs_source_get_name(parent) ? obs_source_get_name(parent) : "");
	obs_data_set_string(data, "filter", obs_source_get_name(filter));
	obs_data_set_bool(data, "active", active);
	emit_event("SceneBlurChanged", data);
	obs_data_release(data);
}

void vendor_emit_standby_cover_changed(obs_source_t *filter, bool covered)
{
	if (!filter)
		return;
	obs_source_t *parent = obs_filter_get_parent(filter);
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "sourceName",
			    parent && obs_source_get_name(parent) ? obs_source_get_name(parent) : "");
	obs_data_set_string(data, "filterName", obs_source_get_name(filter));
	obs_data_set_bool(data, "covered", covered);
	emit_event("StandbyCoverChanged", data);
	obs_data_release(data);
}

void vendor_api_add_test_calls()
{
	static std::atomic<bool> added{false};
	if (added.exchange(true))
		return;
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_vendor_call(in string request, in string data, out string response)",
		[](void *, calldata_t *cd) {
			const char *name = calldata_string(cd, "request");
			const char *json = calldata_string(cd, "data");
			static const struct {
				const char *name;
				obs_websocket_request_callback_function fn;
			} kKnown[] = {
				{"ListSceneModes", req_list_scene_modes},
				{"GetSceneMode", req_get_scene_mode},
				{"SetSceneMode", req_set_scene_mode},
				{"ListSceneBlurs", req_list_scene_blurs},
				{"SceneBlur", req_scene_blur},
				{"GetSwitcherState", req_get_switcher_state},
				{"StandbyCover", req_standby_cover},
				{"GetStandbyCover", req_get_standby_cover},
				{"ShowInScenesPress", req_show_in_scenes_press},
				{"ListTimers", timer_vendor_list},
				{"TimerControl", timer_vendor_control},
				{"AutoFrameCommand", req_auto_frame_command},
				{"GetAutoFrameState", req_get_auto_frame_state},
			};
			obs_data_t *request = json && *json ? obs_data_create_from_json(json) : obs_data_create();
			if (!request)
				request = obs_data_create();
			obs_data_t *response = obs_data_create();
			bool known = false;
			for (const auto &k : kKnown)
				if (name && strcmp(name, k.name) == 0) {
					k.fn(request, response, nullptr);
					known = true;
				}
			if (!known)
				fail(response, "No such request here");
			calldata_set_string(cd, "response", obs_data_get_json(response));
			obs_data_release(response);
			obs_data_release(request);
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_vendor_events(out string events)",
		[](void *, calldata_t *cd) {
			std::lock_guard lock(g_events_mutex);
			g_noting_events = true;
			calldata_set_string(cd, "events", g_events.c_str());
			g_events.clear();
		},
		nullptr);
}

} // namespace stagekit
