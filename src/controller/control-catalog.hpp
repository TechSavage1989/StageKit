/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <obs.h>
#include <string>

namespace stagekit {

// Frontend-thread-only inventory for the always-visible control panel.
// Writes controls [{id,name,section,active,available,reason,defaultVisible,
//                   target:{id,...}}]. Targets are opaque to the UI: persist
// them unchanged and pass them to view_control_press. IDs use real UUIDs and
// stable choice IDs, never editable display names. Imported legacy aliases
// remain listed with defaultVisible=false so existing pins keep working.
// Ordinary scene-item visibility is optional and also defaults to hidden.
// The call reads current state, changes nothing, and installs no polling.
void view_controls_list(obs_data_t *response);

// Re-resolves the supplied stable target ID against a fresh inventory before
// acting. Stale labels/parameters cannot redirect it to a different source.
// Missing, disabled or invalid targets fail without changing the scene. Real
// rules retain their explicitly configured destinations and transitions.
bool view_control_press(obs_data_t *target, std::string &why);

} // namespace stagekit
