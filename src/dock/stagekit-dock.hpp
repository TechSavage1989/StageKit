/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

namespace stagekit {

// Adds the "StageKit" dock to OBS (View → Docks). Call after all modules
// have loaded. Also registers a "stagekit_create_dock" procedure on the
// global OBS proc handler so the headless tests can build the same widget
// for a named scene.
void register_dock();

} // namespace stagekit
