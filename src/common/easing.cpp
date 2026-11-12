/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "easing.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace stagekit {

namespace {

constexpr double kPi = 3.14159265358979323846;

// All curves below are the "ease in" form; Out and InOut are derived.
double ease_in(EaseCurve curve, double t)
{
	switch (curve) {
	case EaseCurve::Linear:
		return t;
	case EaseCurve::Sine:
		return 1.0 - std::cos(t * kPi / 2.0);
	case EaseCurve::Quad:
		return t * t;
	case EaseCurve::Cubic:
		return t * t * t;
	case EaseCurve::Quart:
		return t * t * t * t;
	case EaseCurve::Quint:
		return t * t * t * t * t;
	case EaseCurve::Expo:
		return t <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * t - 10.0);
	case EaseCurve::Circ:
		return 1.0 - std::sqrt(std::max(0.0, 1.0 - t * t));
	case EaseCurve::Back: {
		const double c1 = 1.70158;
		const double c3 = c1 + 1.0;
		return c3 * t * t * t - c1 * t * t;
	}
	case EaseCurve::Elastic: {
		if (t <= 0.0 || t >= 1.0)
			return t;
		const double c4 = (2.0 * kPi) / 3.0;
		return -std::pow(2.0, 10.0 * t - 10.0) * std::sin((t * 10.0 - 10.75) * c4);
	}
	case EaseCurve::Bounce: {
		// Bounce is naturally defined as ease-out; mirror it for ease-in.
		double u = 1.0 - t;
		const double n1 = 7.5625;
		const double d1 = 2.75;
		double out;
		if (u < 1.0 / d1) {
			out = n1 * u * u;
		} else if (u < 2.0 / d1) {
			u -= 1.5 / d1;
			out = n1 * u * u + 0.75;
		} else if (u < 2.5 / d1) {
			u -= 2.25 / d1;
			out = n1 * u * u + 0.9375;
		} else {
			u -= 2.625 / d1;
			out = n1 * u * u + 0.984375;
		}
		return 1.0 - out;
	}
	}
	return t;
}

} // namespace

double ease(EaseCurve curve, EaseMode mode, double t)
{
	t = std::clamp(t, 0.0, 1.0);
	switch (mode) {
	case EaseMode::In:
		return ease_in(curve, t);
	case EaseMode::Out:
		return 1.0 - ease_in(curve, 1.0 - t);
	case EaseMode::InOut:
	default:
		if (t < 0.5)
			return ease_in(curve, t * 2.0) / 2.0;
		return 1.0 - ease_in(curve, (1.0 - t) * 2.0) / 2.0;
	}
}

void add_easing_properties(obs_properties_t *props, const char *prefix)
{
	const std::string curve_key = std::string(prefix) + "ease_curve";
	const std::string mode_key = std::string(prefix) + "ease_mode";

	obs_property_t *curve = obs_properties_add_list(props, curve_key.c_str(), obs_module_text("Easing.Curve"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	struct {
		EaseCurve value;
		const char *text;
	} curves[] = {
		{EaseCurve::Linear, "Easing.Linear"}, {EaseCurve::Sine, "Easing.Sine"},
		{EaseCurve::Quad, "Easing.Quad"},     {EaseCurve::Cubic, "Easing.Cubic"},
		{EaseCurve::Quart, "Easing.Quart"},   {EaseCurve::Quint, "Easing.Quint"},
		{EaseCurve::Expo, "Easing.Expo"},     {EaseCurve::Circ, "Easing.Circ"},
		{EaseCurve::Back, "Easing.Back"},     {EaseCurve::Elastic, "Easing.Elastic"},
		{EaseCurve::Bounce, "Easing.Bounce"},
	};
	for (const auto &c : curves)
		obs_property_list_add_int(curve, obs_module_text(c.text), static_cast<long long>(c.value));

	obs_property_t *mode = obs_properties_add_list(props, mode_key.c_str(), obs_module_text("Easing.Mode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Easing.InOut"), static_cast<long long>(EaseMode::InOut));
	obs_property_list_add_int(mode, obs_module_text("Easing.In"), static_cast<long long>(EaseMode::In));
	obs_property_list_add_int(mode, obs_module_text("Easing.Out"), static_cast<long long>(EaseMode::Out));
}

void Tween::start(double from_value, double to_value, double seconds, EaseCurve c, EaseMode m)
{
	from = from_value;
	to = to_value;
	elapsed = 0.0;
	duration = std::max(0.0, seconds);
	curve = c;
	mode = m;
}

double Tween::advance(double seconds)
{
	if (running())
		elapsed = std::min(duration, elapsed + seconds);
	return value();
}

double Tween::value() const
{
	if (duration <= 0.0 || elapsed >= duration)
		return to;
	return from + (to - from) * ease(curve, mode, elapsed / duration);
}

} // namespace stagekit
