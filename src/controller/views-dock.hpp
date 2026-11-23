/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

namespace stagekit {

void register_views_dock();
void unregister_views_dock();

// Select this controller in StageKit Views and open its configuration.
// Safe to call from a properties button or another thread.
void show_views_controller(const char *controller_id);

} // namespace stagekit
