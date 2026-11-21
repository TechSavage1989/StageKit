/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <obs.h>
#include <cstring>

namespace stagekit {
inline constexpr char kCountdownId[] = "stagekit_countdown";

inline bool is_countdown(obs_source_t *source)
{
	return source && std::strcmp(obs_source_get_id(source), kCountdownId) == 0;
}
} // namespace stagekit
