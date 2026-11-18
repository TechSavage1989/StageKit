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

namespace stagekit {

inline constexpr const char *kShowInScenesId = "stagekit_show_in_scenes";

// Registers the "Show in Scenes (StageKit)" filter: one switch that shows or
// hides sources in the scenes chosen, or in every scene they are in.
void register_show_in_scenes();

// A Stream Deck key's press of the Show in Scenes filter named on `source`:
// `how` "toggle", "on" or "off" switches it as any key does, and `hide_as`
// says how that press hides: "hide", "cover" (with each source's Standby
// Image), or "filter" / "" (as its "Hide shows Standby Image" says). A
// show undoes what the hide did. Sets `enabled` (after); false with `why`
// when there is no such filter. For the "ShowInScenesPress" request.
bool show_in_scenes_press(obs_source_t *source, const std::string &filter, const std::string &how,
			  const std::string &hide_as, bool &enabled, std::string &why);

} // namespace stagekit
