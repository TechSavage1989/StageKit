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

inline constexpr const char *kShadowBackdropId = "stagekit_shadow_backdrop";

void register_shadow_backdrop();

// The room the shadow filters on a source take on each side, in the source's
// pixels: left, top, right, bottom.
void shadow_room(obs_source_t *source, double room[4]);

// What draws a source's picture without its shadows, glows and outlines, and
// where that picture sits inside the whole source.
struct ShadowFree {
	obs_source_t *draw = nullptr; // a new reference: the filter the first shadow draws from, or the source
	bool bare = false;            // `draw` is the source itself, to be drawn without any of its filters
	double left = 0.0, top = 0.0; // where its picture starts in the whole source, in source pixels
	// The room keeps its size on the screen whatever size the item is drawn
	// at (the shadow's sizes are in screen pixels), rather than growing and
	// shrinking with the item.
	bool fixed_on_screen = false;
};
// False when the source has no Shadow, Glow & Outline switched on.
bool shadow_free(obs_source_t *source, ShadowFree &out);

} // namespace stagekit
