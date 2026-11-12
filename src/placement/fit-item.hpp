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

inline constexpr const char *kFitItemId = "stagekit_fit_item";

// A scene filter that keeps one item fitted in a box of the scene: filling it
// when the picture has the box's shape, floating centred inside a margin when
// it has not.
void register_fit_item();

} // namespace stagekit
