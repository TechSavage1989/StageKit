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
// Direct scene targets use kind="scene" and sceneUuid for the scene selector.
// Scene pins and ordinary scene-item visibility also default to hidden.
// The call reads current state, changes nothing, and installs no polling.
void view_controls_list(obs_data_t *response);

// Re-resolves the supplied stable target ID against a fresh inventory before
// acting. Stale labels/parameters cannot redirect it to a different source.
// Missing, disabled or invalid targets fail without changing the scene. Real
// rules retain their explicitly configured destinations and transitions.
// The panel can request show_scene: modes and combined views display their explicit
// destination or owning scene, even when already active. The native request
// defaults stay unchanged for other callers. Direct visibility controls keep
// their original behavior, including Choose one and Show together groups.
// A direct scene target always switches explicitly, independent of show_scene.
bool view_control_press(obs_data_t *target, std::string &why, bool show_scene = false);

} // namespace stagekit
