/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The outline designs filled with one of Ambient Background's patterns: the
// pattern is drawn small into a picture of its own, by the patterns' own
// shader file, and the line's shaders only read that picture for the line's
// colour. So nothing of the patterns is in the outline's shader files, and a
// compiler that refuses the patterns takes only these designs with it (the
// line is then the outline colour, solid).

#pragma once

#include "look-paint.hpp"

namespace stagekit {

// The pattern of a pattern-fill design, drawn for a picture `width` by
// `height` pixels at this video frame's time: one picture per pattern,
// colours, size and moment, shared by every line that asks for the same in
// a frame. `size` multiplies the design's own Pattern size (the per-source
// filter's source is not the screen). Null when the design is no pattern
// fill or the patterns' shaders are not there (logged once). To be called
// in a graphics context; the texture holds until the next frame.
gs_texture_t *outline_pattern_texture(const OutlineDesign &design, uint32_t outline_color, uint32_t width,
				      uint32_t height, double size = 1.0);

// Everything let go of (the module is unloaded).
void outline_pattern_stop();

// The tests': another file in the patterns' place ("" the real one), and
// how many pictures are held and were drawn.
void outline_pattern_test_file(const char *file);
void outline_pattern_add_test_calls();

} // namespace stagekit
