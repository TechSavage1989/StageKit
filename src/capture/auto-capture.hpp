/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <string>
#include <vector>

namespace stagekit {

inline constexpr const char *kAutoCaptureId = "stagekit_auto_capture";

void register_auto_capture();

// The file names of the programs with a window open now, everyday programs
// (OBS, browsers, launchers) left out. Empty on systems other than Windows.
std::vector<std::string> open_program_names();

} // namespace stagekit
