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

inline constexpr const char *kVisibilityLinkId = "stagekit_visibility_link";

// A scene filter that ties the eye icons of several items together:
// "show only one of these" or "show and hide these together".
void register_visibility_link();

// True when an enabled Link Visibility in `scene` set to "Follow the first"
// lists the item's source after its first entry, and every such filter that
// lists it has its first source hidden: the item belongs to a mode that is
// not showing. Show in Scenes passes over such an item when it shows things.
// Safe from any thread.
bool link_visibility_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item);

// True while such a filter has not yet followed a change of its first
// source: the items it leads are on their way to being shown or hidden.
// Show in Scenes' "Follow what is shown" does not read the switch meanwhile.
// Safe from any thread.
bool link_visibility_settling(obs_source_t *scene, obs_sceneitem_t *item);

} // namespace stagekit
