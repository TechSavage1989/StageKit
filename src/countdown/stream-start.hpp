// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "countdown.hpp"
#include <obs.hpp>
#include <cstring>
#include <vector>

namespace stagekit {

inline void start_stream_timers(obs_source_t *private_timer)
{
	std::vector<OBSSource> timers;
	obs_enum_sources(
		[](void *data, obs_source_t *s) {
			if (!obs_obj_is_private(s) && std::strcmp(obs_source_get_id(s), kCountdownId) == 0)
				static_cast<std::vector<OBSSource> *>(data)->emplace_back(s);
			return true;
		},
		&timers);
	if (private_timer)
		timers.emplace_back(private_timer);
	for (const auto &source : timers) {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		if (!obs_source_removed(source) && obs_data_get_bool(settings, "start_on_stream") &&
		    obs_source_media_get_state(source) != OBS_MEDIA_STATE_PLAYING)
			obs_source_media_play_pause(source, false);
	}
}

} // namespace stagekit
