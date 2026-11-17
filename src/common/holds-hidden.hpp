/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// One rule, asked by everything that shows a source: is this item held
// hidden by a mode of its scene that is not showing? Link Visibility's
// "Follow the first source" says so for its modes, Scene Modes for its own.
// Show in Scenes passes over such an item when it shows things, and a
// Standby filter does not stand in for it, so a camera kept out of every
// mode but one never shows in the others.

#pragma once

#include <obs.h>

namespace stagekit {

using HoldsHidden = bool (*)(obs_source_t *scene, obs_sceneitem_t *item);

// Adds a filter kind's two answers: whether it holds the item hidden, and
// whether it is on its way to showing or hiding it (a mode just changed and
// not yet followed). Either may be nullptr. Called once per kind, when it is
// registered; never taken back. Both must be safe from any thread.
void holds_hidden_register(HoldsHidden holds, HoldsHidden settling);

// True when any registered kind holds the item hidden. Safe from any thread.
bool mode_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item);

// True while any registered kind is settling a change for the item. Safe
// from any thread.
bool mode_settling(obs_source_t *scene, obs_sceneitem_t *item);

} // namespace stagekit
