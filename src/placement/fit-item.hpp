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

inline constexpr const char *kFitItemId = "stagekit_fit_item";

// A scene filter that keeps one item fitted in a box of the scene: filling it
// when the picture has the box's shape, floating centred inside a margin when
// it has not.
void register_fit_item();

// Tells any Fit to Space that looks after the item of this name (in `scene`,
// or in any scene when that is null) that it has just been placed on
// purpose, by a copy tool say: where it is now is where its space is looked
// for, and for an item that keeps its size, the size it has now is the one
// kept. Without it the filter puts the item straight back.
void fit_item_placed(obs_source_t *scene, const char *item_name);

} // namespace stagekit
