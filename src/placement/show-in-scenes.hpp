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

inline constexpr const char *kShowInScenesId = "stagekit_show_in_scenes";

// Registers the "Show in Scenes (StageKit)" filter: one switch that shows or
// hides sources in the scenes chosen, or in every scene they are in.
void register_show_in_scenes();

// True when a Show in Scenes switch covers `source` in `scene` (its listed
// sources, or the source it is on when none are listed, and bring-along
// items when that is ticked) and its state now means hidden. Link
// Visibility's "Follow the first" leaves such a source hidden when it shows
// a mode. Safe from any thread; call it with none of your own locks held.
bool show_in_scenes_holds_off(obs_source_t *scene, obs_source_t *source);

} // namespace stagekit
