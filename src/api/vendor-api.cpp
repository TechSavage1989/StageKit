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
#include "../third-party/obs-websocket-api.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <cstring>
#include <string>

namespace stagekit {

namespace {

std::atomic<obs_websocket_vendor> g_vendor{nullptr};

const char *const kRequests[] = {"GetVersion", "ListBlurFilters", "BlurOn",  "BlurOff",
				 "BlurToggle", "SetBlur",         "BlurAll", "HookFront"};

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

// "sourceName": a Game Capture or a Window Capture. Aims it at the window in
// front right now and switches an Auto Capture on it off.
void req_hook_front(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "sourceName");
	obs_source_t *source = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!source) {
		fail(response, "No source with that sourceName");
		return;
	}
	std::string what, error;
	if (hook_front_window(source, what, error)) {
		obs_data_set_bool(response, "success", true);
		obs_data_set_string(response, "executable", what.c_str());
	} else {
		fail(response, error.c_str());
	}
	obs_source_release(source);
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
		{"GetVersion", req_get_version}, {"ListBlurFilters", req_list_blur_filters},
		{"BlurOn", req_blur_on},         {"BlurOff", req_blur_off},
		{"BlurToggle", req_blur_toggle}, {"SetBlur", req_set_blur},
		{"BlurAll", req_blur_all},       {"HookFront", req_hook_front},
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

} // namespace stagekit
