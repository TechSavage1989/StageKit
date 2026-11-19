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

#include <string>

namespace stagekit {

inline constexpr const char *kFitItemId = "stagekit_fit_item";

// A scene filter that keeps one item fitted in a box of the scene: filling it
// when the picture has the box's shape, floating centred inside a margin when
// it has not.
void register_fit_item();
// At unload.
void fit_item_stop();

// Tells any Fit to Space that looks after the item of this name (in `scene`,
// or in any scene when that is null) that it has just been placed on
// purpose, by a copy tool say: where it is now is where its space is looked
// for, and for an item that keeps its size, the size it has now is the one
// kept. Without it the filter puts the item straight back.
void fit_item_placed(obs_source_t *scene, const char *item_name);

// Tells the fit that looks after the item of this name in `scene` and
// resizes it (Resize to fit on, switched on) to fit it now: the dock's Fit to
// space was pressed on it. That fit drops whatever it is waiting for (a move
// or a resize by hand being followed to its end, a home that keeps a size
// made by hand), looks for the item's space round where it is now and glides
// it there from where it is. `filter`: that filter's name. False when no
// such fit is there: nothing is done.
bool fit_item_refit(obs_source_t *scene, const char *item_name, std::string *filter = nullptr);

// Tells the Fit to Space that looks after the item of this name in `scene`
// that another scene's filter has just given the item its own place and size
// ("Same place in other scenes"). The filter leaves the item there while its
// scene is not on screen, takes the size for the one kept, and takes `home`
// (x, y on the screen; null for none) as the point its space is looked for
// around, so a move made on purpose in one scene carries over. When its scene
// comes into view it fits the item to that scene, from that point.
void fit_item_shared(obs_source_t *scene, const char *item_name, const double *home);

} // namespace stagekit
