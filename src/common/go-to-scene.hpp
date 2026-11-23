/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// "Switch to scene": a press that shows something also changes OBS to a
// scene, with OBS's current transition (the preview in Studio Mode). Used by
// Show in Scenes and Scene Modes.

#pragma once

#include <obs.h>

#include <string>
#include <vector>

namespace stagekit {

struct GoTo {
	std::string kind = "Show in Scenes"; // for the log line: what asked for the change
	std::string filter;                  // the filter's name, for the log line
	std::string target;                  // the scene to go to, by name
	// No change while the scene on screen holds an item of one of these
	// (straight in it or in one of its groups).
	std::vector<std::string> sources;
	// No change while the scene on screen is one of these, by name.
	std::vector<std::string> stay_in;
	// Optional identity for a destination already resolved by a control press.
	// A removed scene must not be replaced by another with the same name.
	std::string target_uuid;
};

// Whether to change from `current` to `go.target_uuid` (when given), otherwise
// the scene named `go.target`: it must be
// a scene (not a group), not the one on screen, the one on screen must not be
// in `go.stay_in` nor have any of `go.sources` already. Gives the target,
// with a reference, or null.
obs_source_t *scene_to_go_to(obs_source_t *current, const GoTo &go);

// Changes scene, on the window's thread (OBS's frontend calls belong there),
// or notes it in the tests. Safe from any thread.
void go_to_scene(GoTo go);

// For the tests, which have no OBS window: "stagekit_test_scene_switch"
// (string program) stands in for the scene on screen, and from then on a
// change is noted instead of made; "stagekit_test_scene_switched" (out
// string scenes) gives the scenes asked for since it was last called,
// separated by '|', and forgets them. Adds the two global procs once;
// calling it again does nothing.
void go_to_scene_add_test_calls();

} // namespace stagekit
