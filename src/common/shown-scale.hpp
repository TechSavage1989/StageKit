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

// How big a bounding box draws a source of the given size.
void bounds_scale(obs_bounds_type type, double bx, double by, double w, double h, double &sx, double &sy);

// How big a scene item draws its source (1 = the source's own size).
void item_draw_scale(obs_sceneitem_t *item, double &sx, double &sy);

// How big a source is drawn in the scene on screen, looking through groups
// and nested scenes; failing that, in any scene. False when it is in none.
bool shown_scale(obs_source_t *source, double &sx, double &sy);

} // namespace stagekit
