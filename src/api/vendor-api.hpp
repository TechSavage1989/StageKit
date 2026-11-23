/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

#include <string>

namespace stagekit {

// Registers the "StageKit" vendor with obs-websocket so external tools
// (the StageKit Stream Deck plugin, Touch Portal, scripts) can drive
// StageKit features through CallVendorRequest. Call from post_load.
void vendor_api_register();
void vendor_api_unregister();

// Emits a "BlurChanged" vendor event. Safe to call from any thread and a
// no-op when obs-websocket is not installed.
void vendor_emit_blur_changed(obs_source_t *filter, bool active);

// Emits "OptionsChanged" when the dock's arrange options change, so a key
// showing one can follow. Safe from any thread.
void vendor_emit_options_changed();
void vendor_emit_timer_changed(obs_source_t *timer);

// Shared controller state changed. Emits SceneControllerChanged {controllerId}
// and the native stagekit_scene_controller_changed signal for the Views dock.
// Call only after releasing rule locks. The emitter never reads snapshots.
void vendor_emit_scene_controller_changed(const char *controller_id);

// A window or source switcher (or Auto Capture) says whether it is on a
// manual window and whether its capture is hidden. Emits "SwitcherChanged"
// when that differs from what it said last, so a Stream Deck key showing it
// can follow. Safe from any thread; call as often as you like.
void vendor_switch_state(obs_source_t *filter, bool manual, bool hidden);
// Forgets a filter that is going away.
void vendor_switch_forget(obs_source_t *filter);

// A Scene Modes filter changed its mode (ids; "" for none), or its list of
// modes changed (then `mode` and `previous` are both the mode on). Emits
// "SceneModeChanged" { scene, filter, mode, modeName, previous,
// previousName }. Safe from any thread.
void vendor_emit_scene_mode_changed(obs_source_t *filter, const std::string &mode, const std::string &previous);

// A Scene Blur was switched on or off (by anything: its eye, a hotkey, a
// key). Emits "SceneBlurChanged" { scene, filter, active }. Safe from any
// thread.
void vendor_emit_scene_blur_changed(obs_source_t *filter, bool active);

// A Standby Image filter's image was shown over its source on purpose, or
// taken away (its hotkey, its button, a window switcher's or Show in
// Scenes' hide, a key). Emits "StandbyCoverChanged" { sourceName,
// filterName, covered }. Safe from any thread.
void vendor_emit_standby_cover_changed(obs_source_t *filter, bool covered);

// For the tests, which have no obs-websocket: "stagekit_vendor_call" (in
// string request, in string data, out string response) runs one of the
// vendor requests with its data and answer as JSON, and
// "stagekit_vendor_events" (out string events) gives the events emitted
// since it was last called, one JSON object per line. Adds them once.
void vendor_api_add_test_calls();

} // namespace stagekit
