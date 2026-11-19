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

// Scene Fit: one filter on a scene that fits several of its sources, each
// with Fit to Space's settings, and works out where they all go together in
// one tick, so after a change each glides once, all at the same moment.
void register_scene_fit();
// At unload.
void scene_fit_stop();

// Whether a Scene Fit filter fits a source: listed, or, with All sources,
// one of the rest it fits.
bool scene_fit_covers(obs_source_t *filter, const std::string &source);
// Whether that source's entry is switched on (`fit|<source>|enabled`, on as
// it comes), and switching it.
bool scene_fit_entry_on(obs_source_t *filter, const std::string &source);
void scene_fit_set_entry_on(obs_source_t *filter, const std::string &source, bool on);

} // namespace stagekit
