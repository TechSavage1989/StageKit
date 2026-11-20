/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-properties.h>

namespace stagekit {

// Shape of an animation curve. Values are persisted in settings, so only
// ever append new entries.
enum class EaseCurve : int {
	Linear = 0,
	Sine = 1,
	Quad = 2,
	Cubic = 3,
	Quart = 4,
	Quint = 5,
	Expo = 6,
	Circ = 7,
	Back = 8,
	Elastic = 9,
	Bounce = 10,
};

// Which end(s) of the animation the curve shapes. Persisted; append only.
enum class EaseMode : int {
	InOut = 0,
	In = 1,
	Out = 2,
};

// Maps linear progress t in [0, 1] to eased progress. Back and Elastic
// curves deliberately overshoot outside [0, 1].
double ease(EaseCurve curve, EaseMode mode, double t);

// Adds "<prefix>ease_curve" and "<prefix>ease_mode" list properties.
void add_easing_properties(obs_properties_t *props, const char *prefix);

// The curves and directions in the order every list shows them: a saved
// number and the key of its text. The filters' lists and the dock's both
// come from these.
struct EaseChoice {
	int value;
	const char *text;
};
inline constexpr EaseChoice kEaseCurves[] = {
	{0, "Easing.Linear"}, {1, "Easing.Sine"},    {2, "Easing.Quad"},    {3, "Easing.Cubic"},
	{4, "Easing.Quart"},  {5, "Easing.Quint"},   {6, "Easing.Expo"},    {7, "Easing.Circ"},
	{8, "Easing.Back"},   {9, "Easing.Elastic"}, {10, "Easing.Bounce"},
};
inline constexpr EaseChoice kEaseModes[] = {{0, "Easing.InOut"}, {1, "Easing.In"}, {2, "Easing.Out"}};

// An easing curve list on `key`, with its tooltip.
obs_property_t *add_ease_curve_list(obs_properties_t *props, const char *key);

// A value animating from one number to another over a fixed duration.
struct Tween {
	double from = 0.0;
	double to = 0.0;
	double elapsed = 0.0;
	double duration = 0.0;
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;

	bool running() const { return elapsed < duration; }
	void start(double from_value, double to_value, double seconds, EaseCurve c, EaseMode m);
	// Advances time and returns the current value.
	double advance(double seconds);
	double value() const;
};

} // namespace stagekit
