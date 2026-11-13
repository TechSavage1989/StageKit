/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

namespace stagekit {

inline constexpr const char *kShowInScenesId = "stagekit_show_in_scenes";

// Registers the "Show in Scenes (StageKit)" filter: one switch that shows or
// hides sources in the scenes chosen, or in every scene they are in.
void register_show_in_scenes();

} // namespace stagekit
