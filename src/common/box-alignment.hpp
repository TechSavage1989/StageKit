/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

namespace stagekit::fit {
// Saved Fit to Space positions in OBS grid order, starting at the top left.
inline constexpr int kTowardX[9] = {0, 0, 0, -1, 1, -1, 1, -1, 1};
inline constexpr int kTowardY[9] = {0, -1, 1, 0, 0, -1, -1, 1, 1};
inline constexpr int kTowardGrid[9] = {5, 1, 6, 3, 0, 4, 7, 2, 8};
} // namespace stagekit::fit
