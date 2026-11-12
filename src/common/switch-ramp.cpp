/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "switch-ramp.hpp"

#include <algorithm>
#include <cmath>

namespace stagekit {

namespace {
// Set while this thread changes a filter's enabled state itself, so the
// resulting "enable" signal is not mistaken for the user flipping the switch.
thread_local bool t_changing_enabled = false;
} // namespace

SwitchRamp::SwitchRamp(obs_source_t *filter) : filter_(filter)
{
	signal_handler_connect(obs_source_get_signal_handler(filter_), "enable", enable_cb, this);
}

SwitchRamp::~SwitchRamp()
{
	signal_handler_disconnect(obs_source_get_signal_handler(filter_), "enable", enable_cb, this);
}

void SwitchRamp::configure(double in_seconds, double out_seconds, EaseCurve curve, EaseMode mode)
{
	std::lock_guard lock(mutex_);
	in_s_ = std::max(0.0, in_seconds);
	out_s_ = std::max(0.0, out_seconds);
	curve_ = curve;
	mode_ = mode;
}

bool SwitchRamp::active() const
{
	std::lock_guard lock(mutex_);
	return target_active_;
}

double SwitchRamp::level() const
{
	std::lock_guard lock(mutex_);
	return level_;
}

bool SwitchRamp::begin(bool active)
{
	std::lock_guard lock(mutex_);
	if (!initialized_) {
		initialized_ = true;
		level_ = obs_source_enabled(filter_) ? 1.0 : 0.0;
		target_active_ = level_ > 0.0;
	}
	const bool changed = active != target_active_;
	target_active_ = active;
	const double target = active ? 1.0 : 0.0;
	// Reversing part-way only takes the remaining distance.
	const double distance = std::clamp(std::fabs(target - level_), 0.0, 1.0);
	tween_.start(level_, target, (active ? in_s_ : out_s_) * distance, curve_, mode_);
	if (tween_.duration <= 0.0)
		level_ = target;
	return changed;
}

void SwitchRamp::set_enabled_quietly(bool enabled)
{
	if (obs_source_enabled(filter_) == enabled)
		return;
	t_changing_enabled = true;
	obs_source_set_enabled(filter_, enabled);
	t_changing_enabled = false;
}

bool SwitchRamp::set_active(bool active)
{
	const bool changed = begin(active);
	if (active)
		set_enabled_quietly(true);
	return changed;
}

void SwitchRamp::enable_cb(void *data, calldata_t *cd)
{
	if (!t_changing_enabled)
		static_cast<SwitchRamp *>(data)->on_enabled_changed(calldata_bool(cd, "enabled"));
}

void SwitchRamp::on_enabled_changed(bool enabled)
{
	bool closing;
	double level;
	{
		std::lock_guard lock(mutex_);
		if (!initialized_)
			return; // the first tick picks the state up
		closing = !target_active_ && level_ > 0.0;
		level = level_;
	}
	if (enabled) {
		begin(true);
		return;
	}
	if (closing) {
		// "Off" again while already ramping down: the key was pressed a
		// second time, so come back up.
		begin(true);
		set_enabled_quietly(true);
		return;
	}
	begin(false);
	// A disabled filter is not drawn, so stay enabled until the ramp ends.
	if (level > 0.0 && this->level() > 0.0)
		set_enabled_quietly(true);
}

void SwitchRamp::tick(float seconds)
{
	// Keep this handler last so tools mirroring the filter state see our
	// correction after the change that caused it.
	if (reconnect_.exchange(false)) {
		signal_handler_t *sh = obs_source_get_signal_handler(filter_);
		signal_handler_disconnect(sh, "enable", enable_cb, this);
		signal_handler_connect(sh, "enable", enable_cb, this);
	}

	bool finished_off;
	{
		std::lock_guard lock(mutex_);
		if (!initialized_) {
			initialized_ = true;
			level_ = obs_source_enabled(filter_) ? 1.0 : 0.0;
			target_active_ = level_ > 0.0;
			tween_.start(level_, level_, 0.0, EaseCurve::Linear, EaseMode::InOut);
		}
		level_ = std::max(0.0, tween_.advance(seconds));
		finished_off = !target_active_ && !tween_.running() && level_ <= 0.0;
	}
	if (finished_off)
		set_enabled_quietly(false);
}

} // namespace stagekit
