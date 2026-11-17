/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A number that goes up whenever anything in any scene or group changes in a
// way that can change what shows where: an item added, removed, shown,
// hidden, moved, scaled or reordered, or a scene refreshed. A filter that
// looks at scenes now and then compares it with the number its last look saw
// and skips the look when nothing changed (with a slow look anyway, for
// whatever no signal tells of).
//
// The scenes are watched from the first call on (every scene and group there
// is, and each made after, from its creation). The signals only count; no
// work is done in them and nothing is held.
#pragma once

#include <cstdint>

namespace stagekit {

uint64_t scene_revision();

// For a change that no scene signal tells of (a filter that decides what
// shows switched on or off): the next look is a full one.
void scene_revision_bump();

// How many times scenes have sent this signal (item_add, item_remove,
// item_visible, item_transform, reorder or refresh) since watching began.
uint64_t scene_signals_seen(const char *signal);

// While on, on this thread, scene signals are not counted (`refresh_item`,
// which makes OBS signal a transform that may not have changed).
void scene_signals_quiet(bool on);

// At module unload.
void scene_revision_stop();

} // namespace stagekit
