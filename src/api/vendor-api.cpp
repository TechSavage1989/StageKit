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

#include "../blur/ramp-blur.hpp"
#include "../capture/auto-capture.hpp"
#include "../dock/arrange-tools.hpp"
#include "../frame/auto-frame.hpp"
#include "../third-party/obs-websocket-api.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

namespace stagekit {

namespace {

std::atomic<obs_websocket_vendor> g_vendor{nullptr};

const char *const kRequests[] = {"GetVersion",      "ListBlurFilters", "BlurOn",    "BlurOff",
				 "BlurToggle",      "SetBlur",         "BlurAll",   "HookFront",
				 "SetPictureShape", "GetOptions",      "SetOption", "Slide"};

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
		{"GetOptions", req_get_options},
		{"SetOption", req_set_option},
		{"Slide", req_slide},
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
