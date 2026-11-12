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
#include <graphics/matrix4.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <string>

namespace stagekit {

struct Box {
	double x = 0, y = 0, w = 0, h = 0;
};

// The item's bounding box on the scene canvas, whatever its alignment,
// crop, bounds or rotation.
inline Box item_box(obs_sceneitem_t *item)
{
	matrix4 transform;
	obs_sceneitem_get_box_transform(item, &transform);
	double min_x = 1e18, min_y = 1e18, max_x = -1e18, max_y = -1e18;
	for (int i = 0; i < 4; i++) {
		vec3 corner, out;
		vec3_set(&corner, (i & 1) ? 1.0f : 0.0f, (i & 2) ? 1.0f : 0.0f, 0.0f);
		vec3_transform(&out, &corner, &transform);
		min_x = std::min(min_x, (double)out.x);
		min_y = std::min(min_y, (double)out.y);
		max_x = std::max(max_x, (double)out.x);
		max_y = std::max(max_y, (double)out.y);
	}
	return {min_x, min_y, max_x - min_x, max_y - min_y};
}

// The scene (or group) behind a source, or nullptr.
inline obs_scene_t *scene_of(obs_source_t *source)
{
	if (!source)
		return nullptr;
	obs_scene_t *scene = obs_scene_from_source(source);
	return scene ? scene : obs_group_from_source(source);
}

inline obs_sceneitem_t *find_item(obs_source_t *scene_source, const std::string &name)
{
	obs_scene_t *scene = scene_of(scene_source);
	return scene && !name.empty() ? obs_scene_find_source(scene, name.c_str()) : nullptr;
}

// Fills a list property with the names of the items in a scene.
inline void add_scene_items_to_list(obs_source_t *scene_source, obs_property_t *list)
{
	obs_scene_t *scene = scene_of(scene_source);
	if (!scene)
		return;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		list);
}

} // namespace stagekit
