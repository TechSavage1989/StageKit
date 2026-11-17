/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "../common/scene-filter-ids.hpp"

namespace stagekit {

// Registers "Scene Corners (StageKit)": a filter on a scene that keeps a
// Corners & Edges filter on each of its sources (all but the background, or
// the ones chosen), set once from the scene filter. It draws nothing itself:
// Corners & Edges never changes a source's size, so each source keeps its
// place, its shadow and Morph.
void register_scene_corners();

} // namespace stagekit
