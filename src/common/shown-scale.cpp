/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "shown-scale.hpp"

#include "scene-util.hpp"

#include <graphics/vec2.h>

#include <algorithm>
#include <cmath>

namespace stagekit {

// How big a bounding box draws a source of the given size.
void bounds_scale(obs_bounds_type type, double bx, double by, double w, double h, double &sx, double &sy)
{
	const double rx = bx / w, ry = by / h;
	switch (type) {
	case OBS_BOUNDS_STRETCH:
		sx = rx;
		sy = ry;
		return;
	case OBS_BOUNDS_SCALE_OUTER:
		sx = sy = std::max(rx, ry);
		return;
	case OBS_BOUNDS_SCALE_TO_WIDTH:
		sx = sy = rx;
		return;
	case OBS_BOUNDS_SCALE_TO_HEIGHT:
		sx = sy = ry;
		return;
	case OBS_BOUNDS_MAX_ONLY:
		sx = sy = std::min(1.0, std::min(rx, ry));
		return;
	default:
		sx = sy = std::min(rx, ry);
	}
}

void item_draw_scale(obs_sceneitem_t *item, double &sx, double &sy)
{
	vec2 scale, bounds;
	obs_sceneitem_get_scale(item, &scale);
	obs_sceneitem_get_bounds(item, &bounds);
	sx = scale.x;
	sy = scale.y;
	const obs_bounds_type type = obs_sceneitem_get_bounds_type(item);
	obs_source_t *source = obs_sceneitem_get_source(item);
	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	const double w = (double)obs_source_get_width(source) - crop.left - crop.right;
	const double h = (double)obs_source_get_height(source) - crop.top - crop.bottom;
	if (type != OBS_BOUNDS_NONE && bounds.x > 0.0f && bounds.y > 0.0f && w > 0.0 && h > 0.0)
		bounds_scale(type, bounds.x, bounds.y, w, h, sx, sy);
}

namespace {

struct ShownScale {
	obs_source_t *wanted;
	double sx, sy; // scale gathered on the way down
	double *out_x, *out_y;
	bool *found;
	int depth;
	bool hidden_too; // also an item that is hidden
};

bool find_shown_scale(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *f = static_cast<ShownScale *>(param);
	if (*f->found || (!f->hidden_too && !obs_sceneitem_visible(item)))
		return true;
	obs_source_t *source = obs_sceneitem_get_source(item);
	// Only the item wanted, a group or a nested scene needs its scale (a
	// source's size goes through all its filters: not free).
	const bool group = obs_sceneitem_is_group(item);
	obs_scene_t *nested = group || source == f->wanted ? nullptr : obs_scene_from_source(source);
	if (source != f->wanted && !group && !nested)
		return true;
	double sx = 1.0, sy = 1.0;
	item_draw_scale(item, sx, sy);
	if (source == f->wanted) {
		*f->out_x = std::fabs(f->sx * sx);
		*f->out_y = std::fabs(f->sy * sy);
		*f->found = true;
		return true;
	}
	if (f->depth >= 8)
		return true;
	ShownScale inner{f->wanted, f->sx * sx, f->sy * sy, f->out_x, f->out_y, f->found, f->depth + 1, f->hidden_too};
	if (group)
		obs_sceneitem_group_enum_items(item, find_shown_scale, &inner);
	else if (nested)
		obs_scene_enum_items(nested, find_shown_scale, &inner);
	return true;
}

} // namespace

// Where to look, in turn: the scene on screen, then the others; in each, an
// item that is showing before one that is hidden. A hidden item counts
// because it is the size it will be shown at. Passed over (up to beta113), a
// hidden item's source went by a copy in another scene, or by no scale at
// all, until a moment after it was shown: the owner saw a shadow and an
// outline come in thinner while the item slid in and snap to their thickness
// part-way, and the items fitted round it move twice.
bool shown_scale(obs_source_t *source, double &sx, double &sy)
{
	bool found = false;
	obs_source_t *on_screen = scene_on_screen();
	for (const bool hidden_too : {false, true}) {
		ShownScale look{source, 1.0, 1.0, &sx, &sy, &found, 0, hidden_too};
		if (obs_scene_t *s = on_screen ? obs_scene_from_source(on_screen) : nullptr)
			obs_scene_enum_items(s, find_shown_scale, &look);
		if (found)
			break;
	}
	obs_source_release(on_screen);
	for (const bool hidden_too : {false, true}) {
		if (found)
			break;
		ShownScale look{source, 1.0, 1.0, &sx, &sy, &found, 0, hidden_too};
		obs_enum_scenes(
			[](void *param, obs_source_t *scene) {
				if (obs_scene_t *s = obs_scene_from_source(scene))
					obs_scene_enum_items(s, find_shown_scale, param);
				return !*static_cast<ShownScale *>(param)->found;
			},
			&look);
	}
	return found;
}

} // namespace stagekit
