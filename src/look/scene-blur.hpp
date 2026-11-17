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

#include <string>
#include <vector>

namespace stagekit {

// Scene Blur: a blur on a scene, of all its sources, its live and capture
// sources or the sources chosen, in that scene only.
void register_scene_blur();

// Whether a filter is a Scene Blur.
bool is_scene_blur(obs_source_t *filter);

// A Scene Blur as a key or a request sees it.
struct SceneBlurState {
	std::string scene;
	std::string filter;
	std::string type;     // the blur's look: "gaussian", "pixelate" ... (blur_type_name)
	bool active = false;  // switched on (or on its way on)
	bool enabled = false; // the filter's own switch (on while it blurs out)
};

// Every Scene Blur, on the scene named ("" for every scene) and with the
// name given ("" for any), scenes in OBS's order. Safe from any thread.
std::vector<SceneBlurState> scene_blur_list(const std::string &scene, const std::string &filter);

// Switches every Scene Blur named `filter` on the scene named (or on every
// scene, for ""): `how` "on", "off" or "toggle" (all off when any is on,
// else all on), or "" to only ask. `out` is each one's state afterwards and
// `active` whether they are on. False, with `why`, when there is none.
bool scene_blur_press(const std::string &scene, const std::string &filter, const std::string &how,
		      std::vector<SceneBlurState> &out, bool &active, std::string &why);

} // namespace stagekit
