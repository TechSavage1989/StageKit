/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Modes (StageKit): a filter on a scene that names its modes and the
// sources of each. One mode is on at a time (or none, when allowed): turning
// a mode on shows its sources and hides the other modes' sources. Sources in
// no mode are never touched.

#pragma once

#include "../common/scene-filter-ids.hpp"

#include <obs.h>

#include <string>
#include <vector>

namespace stagekit {

void register_scene_modes();

// One mode, as the Stream Deck plugin and the tests see it. A mode is known
// by its id, which never changes; its name and type can.
struct SceneModeLook {
	std::string id, name, type;
};

// One Scene Modes filter, as the vendor requests answer for it.
struct SceneModesLook {
	std::string scene, filter, current, previous; // current and previous are mode ids, "" for none
	bool allow_none = false, enabled = false;
	std::vector<SceneModeLook> modes;
	std::vector<std::string> scenes; // "Same modes in scenes"
};

// Every Scene Modes filter, or those on the scene named (null or empty: all).
std::vector<SceneModesLook> scene_modes_list(const char *scene);

// A press, as a key or a hotkey makes it. `how` is "on" (as it comes),
// "toggle", "next" or "none"; `mode` is a mode's id, or its name (the first
// with that name), and is not needed for "next" and "none". `filter` may be
// empty for the first Scene Modes on the scene. Queued and acted on in the
// filter's next tick. Says false, with the reason in `why`, when the scene,
// the filter or the mode is not there, the filter is switched off, or no
// mode is not allowed.
bool scene_modes_press(const char *scene, const char *filter, const char *mode, const char *how, std::string &why);

// True when an enabled Scene Modes covering `scene` (its own scene, or one
// listed under "Same modes in scenes") has the item's source in one of its
// modes and the mode on now does not have it (or no mode is on). `scene` is
// the scene holding the item (the one holding its group, for an item in a
// group). Safe from any thread; call with none of your own locks held.
// Registered with holds_hidden_register, so `mode_holds_hidden` asks it.
bool scene_modes_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item);

// True for the tick after a mode change, before the filter has looked again.
bool scene_modes_settling(obs_source_t *scene, obs_sceneitem_t *item);

} // namespace stagekit
