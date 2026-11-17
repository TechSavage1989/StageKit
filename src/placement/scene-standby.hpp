/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "../common/item-transition.hpp"
#include "../common/managed-filter.hpp"
#include "../common/scene-filter-ids.hpp"

#include <obs.h>

namespace stagekit {

// Registers "Scene Standby (StageKit)": a filter on a scene that keeps a
// Standby Image filter on each capture of the scene (or each chosen source),
// set from the scene filter, so an empty capture shows the standby image
// without a filter set up by hand on each. It draws nothing itself.
void register_scene_standby();

// Does an enabled Scene Standby in `scene` list `source` under "Show when
// hidden"? `change` (may be null) gets the change to use for it there. Safe
// from any thread.
bool scene_standby_hidden(obs_source_t *scene, obs_source_t *source, ItemChange *change);

// Does an enabled Scene Standby in any scene list `source` under "Show when
// hidden"? Safe from any thread.
bool scene_standby_any_hidden(obs_source_t *source);

// What the Scene Standby filters say about a source with a Standby Image
// filter they made, which names `managed_by`. Safe from any thread.
ManagedState scene_standby_state(obs_source_t *source, const char *managed_by);

} // namespace stagekit
