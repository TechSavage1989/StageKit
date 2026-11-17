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

// Scene Fit: one filter on a scene that fits several of its sources, each
// with Fit to Space's settings, and works out where they all go together in
// one tick, so after a change each glides once, all at the same moment.
void register_scene_fit();
// At unload.
void scene_fit_stop();

} // namespace stagekit
