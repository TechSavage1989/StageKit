/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The outline design filled with an Ambient Background: the line shows the
// background's finished picture (its pattern, image or live capture, with
// its blur, mosaic, bloom and transitions), lifted so it reads. The picture
// is the one the background drew for the frame (ambient-frame.hpp), laid out
// for the line by a small shader file of its own; the line's shaders only
// read the result, as they read a pattern fill's. Without that file, or
// without a background, the line is its outline colour.

#pragma once

#include "look-paint.hpp"

namespace stagekit {

// A filter's choice of background (`outline_ambient`: "" Automatic, or a
// source's name) as a number an OutlineDesign carries. From any thread.
uint32_t outline_ambient_choice(const char *name);

// The background that choice means now (ambient-frame.hpp's number; 0:
// there is none). Automatic: the first Ambient Background in `scene` (a
// scene's source, or nullptr: no scene of its own), then in the scene on
// screen, then any. A named one that is gone: as Automatic. Only the answer
// kept is given here, so it is safe from any drawing: the lookup is made in
// a tick, again when a background comes or goes and four times a second (a
// choice asked for the first time has 0 until the next tick). The scene is
// held weakly; a choice not asked for in five seconds is forgotten.
uint32_t outline_ambient_resolve(uint32_t choice, obs_source_t *scene);

// From a filter's tick (never from a drawing): a choice not looked up yet is
// looked up now, so the first drawing has its answer.
void outline_ambient_warm(uint32_t choice, obs_source_t *scene);

// What share of the background's width and height the line's picture covers
// at Scale 100 % (1, 1: the picture is the canvas, and the line shows the
// background where the canvas shows it).
struct AmbientCover {
	double x = 1.0, y = 1.0;
};

// The design's fill for a picture `width` by `height` pixels: one picture
// per background, size and layout, shared by every line that asks for the
// same in a frame. Null when the design is not this fill, there is no
// background or the shader is not there (logged once). In a graphics
// context; the texture holds until the next frame.
gs_texture_t *outline_ambient_texture(const OutlineDesign &design, uint32_t width, uint32_t height,
				      const AmbientCover &cover = AmbientCover());

// A list of the choices for a filter's window, and following a renamed
// background in the filters that name it.
void add_outline_ambient_property(obs_properties_t *outline);

void outline_ambient_start();
// Everything let go of (the module is unloaded).
void outline_ambient_stop();

// The tests': another file in the shader's place ("-" the real one), how
// many pictures are held and were drawn.
void outline_ambient_add_test_calls();

} // namespace stagekit
