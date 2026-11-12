/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "easing.hpp"

#include <obs.h>

#include <atomic>
#include <mutex>

namespace stagekit {

// Makes a filter's own on/off switch drive a smooth 0..1 level.
//
// Switching the filter on ramps the level up. Switching it off keeps the
// filter drawn while the level ramps down, then really disables it. That
// lets the eye icon, a Stream Deck OBS plugin "Filter" key and
// obs-websocket's SetSourceFilterEnabled all produce animated effects.
class SwitchRamp {
public:
	explicit SwitchRamp(obs_source_t *filter);
	~SwitchRamp();
	SwitchRamp(const SwitchRamp &) = delete;
	SwitchRamp &operator=(const SwitchRamp &) = delete;

	void configure(double in_seconds, double out_seconds, EaseCurve curve, EaseMode mode);

	// Call from filter_add and video_tick.
	void attached() { reconnect_ = true; }
	void tick(float seconds);

	// For hotkeys and scripts. Returns true if the on/off target changed.
	bool set_active(bool active);
	bool active() const;
	double level() const;

private:
	bool begin(bool active);
	void set_enabled_quietly(bool enabled);
	void on_enabled_changed(bool enabled);
	static void enable_cb(void *data, calldata_t *cd);

	obs_source_t *filter_;
	mutable std::mutex mutex_;
	double in_s_ = 0.4, out_s_ = 0.4;
	EaseCurve curve_ = EaseCurve::Cubic;
	EaseMode mode_ = EaseMode::InOut;
	Tween tween_;
	bool target_active_ = false;
	double level_ = 0.0;
	bool initialized_ = false;
	std::atomic<bool> reconnect_{false};
};

} // namespace stagekit
