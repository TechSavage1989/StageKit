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

inline constexpr const char *kProgramTriggerId = "stagekit_program_trigger";

// A filter that shows items or switches filters on while a program has a
// window open, and undoes it when the program goes away.
void register_program_trigger();

} // namespace stagekit
