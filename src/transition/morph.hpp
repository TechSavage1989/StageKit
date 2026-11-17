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

// A scene transition. Whatever shows the same source in both scenes glides
// from its place and size in the one to its place and size in the other;
// everything else is cross-faded where it is.
void register_morph();

// Lets a scene with a drawing filter that `accepts` answers yes for still be
// morphed item by item (Scene Shadow). One at a time; nullptr clears it.
void morph_accept_drawing_filter(bool (*accepts)(obs_source_t *filter));

} // namespace stagekit
