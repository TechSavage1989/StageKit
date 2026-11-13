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

inline constexpr const char *kActiveAppId = "stagekit_active_app";

// A scene filter that shows one of a set of captures at a time: the one
// whose app is in use.
void register_active_app();

} // namespace stagekit
