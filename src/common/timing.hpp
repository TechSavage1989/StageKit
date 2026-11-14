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

namespace stagekit {

// Registers a filter or source like obs_register_source, and keeps time of
// its tick and its drawing. Twice a minute, whatever took long is written to
// OBS's log (nothing is written while all is quick), so a log from a session
// with dropped frames says which part of StageKit to look at.
void register_timed(obs_source_info *info);

} // namespace stagekit
