/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Finding filters on a source, and where they come in the order it applies
// them.

#pragma once

#include <obs.h>

#include <cstring>

namespace stagekit {

// Where a filter comes in the order its source applies them, counted from
// the one applied first, or -1.
inline int applied_index(obs_source_t *parent, obs_source_t *filter)
{
	struct Find {
		obs_source_t *wanted;
		int at, found;
	} find{filter, 0, -1};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			if (f == x->wanted)
				x->found = x->at;
			x->at++;
		},
		&find);
	return find.found;
}

// The first filter of a kind on a source, in applied order; `enabled_only`
// passes over switched-off ones. Not a new reference.
inline obs_source_t *first_filter_of_kind(obs_source_t *parent, const char *id, bool enabled_only = false)
{
	struct Find {
		const char *id;
		bool enabled_only;
		obs_source_t *found;
	} find{id, enabled_only, nullptr};
	obs_source_enum_filters(
		parent,
		[](obs_source_t *, obs_source_t *f, void *param) {
			auto *x = static_cast<Find *>(param);
			const char *fid = obs_source_get_unversioned_id(f);
			if (!x->found && fid && std::strcmp(fid, x->id) == 0 &&
			    (!x->enabled_only || obs_source_enabled(f)))
				x->found = f;
		},
		&find);
	return find.found;
}

// The first enabled filter of a kind on a source, in applied order. Not a
// new reference.
inline obs_source_t *first_filter(obs_source_t *parent, const char *id)
{
	return first_filter_of_kind(parent, id, true);
}

} // namespace stagekit
