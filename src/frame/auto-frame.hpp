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

inline constexpr const char *kAutoFrameId = "stagekit_auto_frame";
inline constexpr const char *kPictureShapeId = "stagekit_picture_shape";

void register_auto_frame();

// Where the game picture sits in a source that has an Auto Frame switched on,
// in the source's own pixels, allowing for the room a Shadow & Backdrop after
// it adds. Leaves the values alone and returns false when there is none.
bool auto_frame_picture(obs_source_t *source, double &x, double &y, double &width, double &height);

} // namespace stagekit
