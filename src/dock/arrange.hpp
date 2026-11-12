/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "../common/scene-util.hpp"

#include <obs.h>

#include <vector>

namespace stagekit {

// Lining items up, spacing them out and snapping them together, measured
// either by an item's box or by what is actually visible in it.

struct Measure {
	bool visible_pixels = true;    // the drawn picture, not the transparent room around it
	bool leave_out_shadows = true; // a Shadow & Backdrop's shadow does not count as picture
};

enum class Edge { Left, Center, Right, Top, Middle, Bottom };
enum class Against { Screen, Selection };

// Where an item is on its scene's canvas, by its box or by what shows.
Box measure_item(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how);

// The items selected in a scene, bottom-most first, items in groups included.
std::vector<obs_sceneitem_t *> selected_items(obs_source_t *scene);

// Moves an item so the measured picture shifts by (dx, dy) canvas pixels.
void shift_item(obs_source_t *scene, obs_sceneitem_t *item, double dx, double dy);

// Lines items up on one edge or centre line. Returns how many moved.
int align_items(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, Edge edge, Against against,
		const Measure &how);

// Spaces three or more items so the gaps between them are equal.
int distribute_items(obs_source_t *scene, std::vector<obs_sceneitem_t *> items, bool across, const Measure &how);

// Makes the other items as wide and/or as tall as the top-most one looks.
// keep_shape scales both directions alike; without it only the directions
// asked for change, which stretches the picture.
int match_size(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, bool width, bool height,
	       bool keep_shape, const Measure &how);

// Moves an item the last few pixels so an edge or centre meets the screen's
// or another item's. Returns true when it moved.
bool snap_item(obs_source_t *scene, obs_sceneitem_t *item, double within, const Measure &how);

} // namespace stagekit
