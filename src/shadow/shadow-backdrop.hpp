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

inline constexpr const char *kShadowBackdropId = "stagekit_shadow_backdrop";

void register_shadow_backdrop();

// The room the shadow filters on a source take on each side, in the source's
// pixels: left, top, right, bottom.
void shadow_room(obs_source_t *source, double room[4]);

} // namespace stagekit
