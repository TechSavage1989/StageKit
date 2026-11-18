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
#include "../shape/corners.hpp"

#include <obs.h>

namespace stagekit {

// Registers "Scene Corners (StageKit)": a filter on a scene that keeps a
// Corners & Edges filter on each of its sources (all but the background, or
// the ones chosen), set once from the scene filter. It draws nothing itself:
// Corners & Edges never changes a source's size, so each source keeps its
// place, its shadow and Morph. With *Corners on: The whole scene* it puts no
// filter on its sources and cuts the scene's own image to the shape instead.
void register_scene_corners();

// Whether a filter is a Scene Corners drawing the whole scene's shape now
// (*Corners on: The whole scene*): then it draws, and Morph and the other
// scene filters must count it as a filter that draws.
bool scene_corners_draws(obs_source_t *filter);

// For Morph: the shape the scene's enabled Scene Corners gives the whole
// scene, if one does. A change of scene then draws the morph and cuts it to
// that shape, part-way between the two scenes' (lerp_corner_shape).
bool scene_corners_now(obs_source_t *scene, CornerShape &out);

} // namespace stagekit
