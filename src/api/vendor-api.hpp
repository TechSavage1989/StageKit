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

namespace stagekit {

// Registers the "StageKit" vendor with obs-websocket so external tools
// (the StageKit Stream Deck plugin, Touch Portal, scripts) can drive
// StageKit features through CallVendorRequest. Call from post_load.
void vendor_api_register();
void vendor_api_unregister();

// Emits a "BlurChanged" vendor event. Safe to call from any thread and a
// no-op when obs-websocket is not installed.
void vendor_emit_blur_changed(obs_source_t *filter, bool active);

} // namespace stagekit
