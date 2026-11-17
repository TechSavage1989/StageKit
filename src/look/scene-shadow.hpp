/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "look-paint.hpp"

#include <obs.h>

#include <cstdint>
#include <unordered_map>

namespace stagekit {

// Scene Shadow: one shadow, glow or outline for a whole scene, on a scene.
void register_scene_shadow();

// What a scene's shadow does with each of its items, by the item's id.
enum ShadowFlag : uint8_t {
	kShadowBackground = 1, // drawn as it is: casts nothing, takes the shadow
	kShadowCasts = 2,      // its shape throws the shadow and has the outline
	kShadowMasked = 4,     // the shadow is kept off it (every source that is not background)
};
using ShadowFlags = std::unordered_map<int64_t, uint8_t>;

// A scene's shadow as it is now: the look (faded by its switch) and what
// each item does. A copy, for Morph to draw a change of scene with.
struct SceneShadowNow {
	ShadowLook look;
	ShadowFlags flags;
};

// False when the scene has no Scene Shadow drawing now (none, switched off,
// or standing aside).
bool scene_shadow_now(obs_source_t *scene, SceneShadowNow &out);

// What each item would do under a Scene Shadow as it comes (the full-screen
// sources at the bottom are background, the rest cast), for a scene with
// none: Morph fades the shadow out over such a scene.
void scene_shadow_default_flags(obs_source_t *scene, ShadowFlags &out);

// Whether a filter is a Scene Shadow.
bool is_scene_shadow(obs_source_t *filter);

// For Scene Blur's "Each source by itself", on the drawing thread: the shadow
// the Scene Shadow `filter` drew this frame laid again over `picture` (the
// scene drawn again, cx by cy), the same shadow from the same shapes. Its
// picture, kept until the next call, or nullptr when it drew nothing this
// frame.
gs_texture_t *scene_shadow_lay_over(obs_source_t *filter, gs_texture_t *picture, uint32_t cx, uint32_t cy);

} // namespace stagekit
