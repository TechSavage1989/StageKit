/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The ids of the scene filters: filters that sit on a scene and look after
// its sources together. Kept here, apart from the filters themselves, so
// that Morph and the dock's copy tools can know them without waiting for
// any one filter's files. An id never changes once it is in a collection.

#pragma once

namespace stagekit {

inline constexpr const char *kSceneModesId = "stagekit_scene_modes";
inline constexpr const char *kSceneTransitionsId = "stagekit_scene_transitions";
inline constexpr const char *kSceneFitId = "stagekit_scene_fit";
inline constexpr const char *kSceneStandbyId = "stagekit_scene_standby";
inline constexpr const char *kSceneCornersId = "stagekit_scene_corners";
inline constexpr const char *kSceneShadowId = "stagekit_scene_shadow";
inline constexpr const char *kSceneBlurId = "stagekit_scene_blur";

// The per-source filters that Scene Standby and Scene Corners add and keep.
inline constexpr const char *kStandbyFilterId = "stagekit_standby";
inline constexpr const char *kCornersFilterId = "stagekit_corners";

} // namespace stagekit
