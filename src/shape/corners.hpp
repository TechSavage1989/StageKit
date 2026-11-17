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

inline constexpr const char *kCornersId = "stagekit_corners";

void register_corners();

// The look's controls and their defaults (style, size, the four corners,
// edges and line, sizes in screen pixels), for Scene Corners, which sets the
// same settings on the Corners & Edges filters it makes.
void corners_add_look_properties(obs_properties_t *props);
void corners_look_defaults(obs_data_t *settings);

} // namespace stagekit
