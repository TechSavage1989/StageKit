/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The finished picture of each Ambient Background, for whoever else wants to
// show it (the outline's Ambient Background fill): every background says
// here which picture it drew last and in which video frame, so nobody has to
// draw it a second time. Nothing here holds a source: a background is known
// by a number of its own (never used twice) and held weakly, and it takes
// itself out as it is destroyed.

#pragma once

#include <obs.h>

#include <cstdint>

namespace stagekit {

struct AmbientFrame {
	gs_texture_t *texture = nullptr; // raw (non-linear) colour values, the background's own size
	uint32_t width = 0, height = 0;
	uint64_t frame = 0; // obs_get_video_frame_time() when it was drawn
};

// A background joins when it is made (its number is the answer) and leaves
// as it is destroyed; `leave` in a graphics context, so nobody is reading
// its picture meanwhile.
uint32_t ambient_frame_join(obs_source_t *source);
void ambient_frame_leave(uint32_t id);
// From the background's drawing: the picture it has just finished.
void ambient_frame_publish(uint32_t id, gs_texture_t *texture, uint32_t width, uint32_t height);

// The picture of background `id`, in a graphics context; it holds until the
// background is drawn again or the context is left. A background not drawn
// in the last two frames (it is in no scene on show) is drawn once here
// first, for all who ask in that frame. False when there is none.
bool ambient_frame_get(uint32_t id, AmbientFrame &out);

// The number of the background a source is (0: it is none), of the first
// one there is (0: none), and a count that changes whenever one joins or
// leaves. From any thread; no source is touched.
uint32_t ambient_frame_of(const obs_source_t *source);
uint32_t ambient_frame_first();
uint64_t ambient_frame_revision();

// Everything let go of (the module is unloaded).
void ambient_frame_stop();

// The tests': how many backgrounds are known, and how often one was drawn
// here because nothing else had drawn it.
void ambient_frame_add_test_calls();

} // namespace stagekit
