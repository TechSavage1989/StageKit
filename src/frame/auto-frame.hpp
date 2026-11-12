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

inline constexpr const char *kAutoFrameId = "stagekit_auto_frame";
inline constexpr const char *kPictureShapeId = "stagekit_picture_shape";

void register_auto_frame();

} // namespace stagekit
