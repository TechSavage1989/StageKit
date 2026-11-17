/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A Standby filter's stand-ins ("twins"): a private source drawn by a locked
// item straight under each item of a source it stands in for while hidden.
// Anything that groups, fits or shadows a scene's items treats a twin as the
// item it stands for, never as one more thing in the way.

#pragma once

#include <obs.h>

#include <cstdint>
#include <cstring>

namespace stagekit {

// The stand-in source's kind.
inline constexpr const char *kStandInId = "stagekit_stand_in";
// On a stand-in's scene item, in its private settings: the id of the item it
// stands in for.
inline constexpr const char *kStandInFor = "stagekit_stand_in_for";

// Whether a source is a Standby filter's stand-in.
inline bool is_stand_in(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	return id && std::strcmp(id, kStandInId) == 0;
}

// The id, in the same scene, of the item a twin stands in for; 0 when `twin`
// is not a stand-in's item.
inline int64_t stand_in_for_item(obs_sceneitem_t *twin)
{
	if (!twin || !is_stand_in(obs_sceneitem_get_source(twin)))
		return 0;
	obs_data_t *own = obs_sceneitem_get_private_settings(twin);
	const int64_t id = own ? obs_data_get_int(own, kStandInFor) : 0;
	obs_data_release(own);
	return id;
}

} // namespace stagekit
