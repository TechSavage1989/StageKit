/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Transitions: one filter on a scene that says how every source in it
// shows and hides. It writes OBS's own item transitions (the ones in an
// item's right-click menu), so they hold for the eye, the item's hotkeys,
// Stream Deck Source Visibility keys and obs-websocket as well, and it is
// what "As the scene sets" (`kTransitionScene`) means in every other filter.
// The owner: "one place to set how every source in the scene shows and
// hides, instead of a transition setting inside each filter."

#pragma once

#include "../common/item-transition.hpp"
#include "../common/scene-filter-ids.hpp"

#include <obs.h>

namespace stagekit {

void register_scene_transitions();

// What the scene's Scene Transitions gives an item, if one covers it: the
// first enabled one in the scene's filter list. `scene` is the scene the item
// is in, or the one holding its group. Never leaves "As the scene sets" in
// `out`. Safe from any thread.
bool scene_transition_for(obs_source_t *scene, obs_sceneitem_t *item, ItemChange &out);

// Lets go of the signal it listens to for renamed sources (OBS closing).
void scene_transitions_stop();

} // namespace stagekit
