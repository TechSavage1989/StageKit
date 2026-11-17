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

inline constexpr const char *kMorphId = "stagekit_morph";
// Make Room: Morph with Make Room for whatever has no partner. As an item's
// own transition it never pairs: the item leaves by (or comes in from) an
// edge of the screen, away from what else changes in its scene.
inline constexpr const char *kMakeRoomId = "stagekit_make_room";

// A scene transition. Whatever shows the same source in both scenes glides
// from its place and size in the one to its place and size in the other;
// everything else is cross-faded where it is. Registers Make Room too.
void register_morph();

// Lets a scene with a drawing filter that `accepts` answers yes for still be
// morphed item by item (Scene Shadow, and Scene Blur while it blurs
// nothing). Several may; each is asked. nullptr clears them all.
void morph_accept_drawing_filter(bool (*accepts)(obs_source_t *filter));

// Whether a filter on a scene draws nothing (it only moves, shows and hides
// items, or handles sound), so the scene still looks as its items do.
bool morph_draws_nothing(obs_source_t *filter);

} // namespace stagekit
