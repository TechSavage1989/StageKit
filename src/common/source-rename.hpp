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

// Follows public source and scene renames in saved references owned by StageKit.
void source_references_renamed(void *, calldata_t *cd);

// Apply the same reference and per-source transition rules to an owned child
// settings object. The caller persists it; this never updates an OBS source.
bool source_settings_renamed(const char *kind, obs_data_t *settings, const char *was, const char *now, bool scene);

} // namespace stagekit
