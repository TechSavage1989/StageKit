/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <obs.h>
#include <QString>

class QWidget;

namespace stagekit {

inline constexpr char kCountdownId[] = "stagekit_countdown";

enum class TimerMode { Duration, Deadline, CountUp };
enum TimerUnit { Days = 1, Hours = 2, Minutes = 4, Seconds = 8 };

struct TimerOptions {
	TimerMode mode = TimerMode::Duration;
	int64_t duration = 300000;
	int64_t deadline = 0;
	bool limit = false;
	bool on_program = false;
	int units = 0; // zero selects the automatic format
	QString message;
	QString scene;

	static TimerOptions read(obs_data_t *settings);
	void write(obs_data_t *settings) const;
	bool same_timing(const TimerOptions &other) const;
	bool unlimited() const { return mode == TimerMode::CountUp && !limit; }
};

QString timer_text(int64_t milliseconds, int units, bool round_up, int64_t range);
QString timer_readout(obs_source_t *source);
void edit_timer(QWidget *parent, obs_source_t *source);
void register_countdown();
void register_countdown_dock();
void unregister_countdown_dock();

} // namespace stagekit
