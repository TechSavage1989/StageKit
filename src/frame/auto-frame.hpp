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

#include <string>

namespace stagekit {

inline constexpr const char *kAutoFrameId = "stagekit_auto_frame";
inline constexpr const char *kPictureShapeId = "stagekit_picture_shape";

void register_auto_frame();

// A measurement belongs to one filter instance and one captured window. Request
// IDs are unique across Auto Frame instances for the lifetime of the plugin.
struct AutoFrameState {
	bool success = true;
	int64_t request_id = 0;
	std::string status = "idle";
	bool complete = true;
	std::string program;
	std::string executable;
	std::string aspect_ratio;
	double ratio = 0.0;
	uint32_t width = 0, height = 0;
	bool saved = false;
	std::string error;
};

// Actions: measure, remember, measure_and_remember. Measuring is asynchronous;
// poll with its request_id. A combined action saves only its own fresh result.
bool auto_frame_command(obs_source_t *filter, const std::string &action, AutoFrameState &state);
bool auto_frame_state(obs_source_t *filter, int64_t request_id, AutoFrameState &state);

// Where the game picture sits in a source that has an Auto Frame switched on,
// in the source's own pixels, allowing for the room a Shadow & Backdrop after
// it adds. Leaves the values alone and returns false when there is none.
bool auto_frame_picture(obs_source_t *source, double &x, double &y, double &width, double &height);

// Sets the picture's shape by hand on a source's Auto Frame: `shape` is "4:3",
// "8:7", a number, or "auto" to hand it back. With `until_gone` the shape is
// kept for the game being captured and dropped when that capture has ended.
// False, with a reason, when the source has no Auto Frame or the shape is
// not one.
bool set_picture_shape_on(obs_source_t *source, const std::string &shape, bool until_gone, std::string &error);

} // namespace stagekit
