/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Where a pattern lies in its picture: Position X, Position Y and Rotation,
// for Ambient Background's patterns and the outline's pattern fills. The
// patterns' vertex shader turns the picture's uv into the pattern's own
// (`frame`, `frame_shift` in the three stagekit-patterns files); the pattern
// is drawn as for a screen that holds the turned picture, so its uv stays
// within 0..1 and the features keep their size. Sine and cosine are worked
// out here, never in a shader.

#pragma once

#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <cmath>
#include <cstdint>

namespace stagekit {

struct PatternPose {
	double x = 0.0, y = 0.0; // shares of the picture's width and height
	double degrees = 0.0;    // clockwise
	bool none() const { return x == 0.0 && y == 0.0 && degrees == 0.0; }
	bool operator==(const PatternPose &o) const { return x == o.x && y == o.y && degrees == o.degrees; }
};

struct PatternFrame {
	vec4 frame;  // a 2x2 matrix, row by row
	vec2 shift;  // the Position, in uv
	vec2 aspect; // the patterns' `aspect`
	float scale; // the patterns' `scale`
};

// For a picture w by h pixels and a pattern size of `scale`. With no pose
// the values are exactly what the patterns were given before there was one.
inline PatternFrame pattern_frame(uint32_t w, uint32_t h, double scale, const PatternPose &pose)
{
	PatternFrame f;
	vec4_set(&f.frame, 1.0f, 0.0f, 0.0f, 1.0f);
	vec2_set(&f.shift, (float)pose.x, (float)pose.y);
	vec2_set(&f.aspect, (float)w / (float)h, 1.0f);
	f.scale = (float)scale;
	if (pose.degrees == 0.0)
		return f;
	const double a = (double)w / (double)h;
	const double r = pose.degrees * 3.14159265358979323846 / 180.0;
	const double c = std::cos(r), s = std::sin(r);
	// The screen the turned picture fits in, in picture heights.
	const double bx = std::fabs(c) * a + std::fabs(s), by = std::fabs(s) * a + std::fabs(c);
	vec4_set(&f.frame, (float)(c * a / bx), (float)(s / bx), (float)(-s * a / by), (float)(c / by));
	vec2_set(&f.aspect, (float)(bx / by), 1.0f);
	f.scale = (float)(scale / by);
	return f;
}

} // namespace stagekit
