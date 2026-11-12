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

inline constexpr const char *kCroppedViewId = "stagekit_cropped_view";

// A source that shows part of another source. It has filters of its own, so
// a shadow can follow the crop without a wrapper scene.
void register_cropped_view();

} // namespace stagekit
