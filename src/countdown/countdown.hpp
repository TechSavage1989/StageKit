/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "timer-source.hpp"
#include <obs.h>
#include <QString>
#include <QDateTime>

class QWidget;

namespace stagekit {

inline constexpr int kTimerBoxWidth = 800, kTimerBoxHeight = 120;
inline constexpr int kTimerMaxWidth = 4096, kTimerMaxHeight = 2160;

struct TimerDockStyle {
	int alignment = 0, pixels = 0;
	QString reference;
};

enum class TimerMode { Duration, Deadline, CountUp };
// These reserve the width of a hidden digit or clock separator in a visual readout.
inline constexpr QChar timer_digit_space{0x2007}, timer_colon_space{0x2008};
enum class TimerText { Plain, Layout, EndMessage };

enum TimerUnit { Days = 1, Hours = 2, Minutes = 4, Seconds = 8 };

struct TimerOptions {
	TimerMode mode = TimerMode::Duration;
	int64_t duration = 300000;
	int64_t deadline = 0;
	bool limit = false;
	bool on_program = false;
	bool on_stream = false;
	bool steady = false;
	bool date_up = false;
	bool leading_zero = true;
	bool format_output = false;
	bool use_message = false;
	bool switch_scene = false;
	QString output_format = QStringLiteral("%time%");
	int box_width = kTimerBoxWidth, box_height = kTimerBoxHeight;
	int alignment = 0, dock_pixels = 0;
	int units = 0;     // zero selects the automatic format
	int mark_from = 0; // MarkFrom, saved as mark_from
	QString message;
	QString scene;

	static TimerOptions read(obs_data_t *settings);
	void write(obs_data_t *settings) const;
	bool same_timing(const TimerOptions &other) const;
	bool counts_up() const { return mode == TimerMode::CountUp || (mode == TimerMode::Deadline && date_up); }
	QString clock_text(int64_t ms, int64_t range, bool layout = false) const;
	QString display_text(int64_t ms, int64_t range, TimerText purpose = TimerText::Plain) const;
	bool unlimited() const { return mode == TimerMode::CountUp && !limit; }
};

inline constexpr int kSavedTimer = -2, kNextQuarter = -1;
inline constexpr int timer_marks[]{kNextQuarter, 15, 30, 45, 0};
inline constexpr const char *timer_mark_labels[]{"Countdown.NextQuarter", "Countdown.Next15", "Countdown.Next30",
						 "Countdown.Next45", "Countdown.NextHour"};

inline QDateTime next_timer_mark(const QDateTime &now, int mark)
{
	if (!now.isValid() || (mark != kNextQuarter && mark != 0 && mark != 15 && mark != 30 && mark != 45))
		return {};
	// Advance real minutes so midnight and both sides of a daylight-saving change use the OBS computer's clock.
	auto next = now.addMSecs(60000 - now.time().second() * 1000 - now.time().msec());
	while (next.isValid() && (mark == kNextQuarter ? next.time().minute() % 15 != 0 : next.time().minute() != mark))
		next = next.addSecs(60);
	return next;
}
// Saved as mark_from, by number, append-only. An absent key reads as From now (the owner, 2026-11-24:
// "the new behavior as default"), so every timer saved before counts its marks from the press.
enum class MarkFrom { Now = 0, Clock = 1 };
// The request's override for one command: 0 follows the timer, 1 from now, 2 clock time.
inline constexpr int kMarkFromTimer = 0, kMarkFromNow = 1, kMarkFromClock = 2;
inline bool marks_from_now(obs_data_t *settings)
{
	return obs_data_get_int(settings, "mark_from") != int(MarkFrom::Clock);
}
// Minutes from the press a mark stands for when marks count from now: 15, 30, 45 and 60 for the hour.
inline int timer_mark_minutes(int mark)
{
	return mark == kNextQuarter ? 15 : mark == 0 ? 60 : mark;
}
inline QDateTime timer_mark_deadline(const QDateTime &now, int mark, bool from_now)
{
	if (!from_now)
		return next_timer_mark(now, mark);
	if (!now.isValid() || (mark != kNextQuarter && mark != 0 && mark != 15 && mark != 30 && mark != 45))
		return {};
	// Seconds are kept, so the countdown shows exactly 15:00 (and so on) at the press.
	return now.addSecs(int64_t(timer_mark_minutes(mark)) * 60);
}
inline const char *timer_mark_label(size_t index, bool from_now)
{
	static constexpr const char *from_now_labels[]{"Countdown.In15", "Countdown.In15", "Countdown.In30",
						       "Countdown.In45", "Countdown.In60"};
	return from_now ? from_now_labels[index] : timer_mark_labels[index];
}
inline int saved_timer_mark(obs_data_t *settings)
{
	return obs_data_has_user_value(settings, "clock_shortcut") ? int(obs_data_get_int(settings, "clock_shortcut"))
								   : kSavedTimer;
}
bool timer_command(obs_source_t *source, const char *operation, int mark = kSavedTimer, int from = kMarkFromTimer);
void timer_vendor_list(obs_data_t *request, obs_data_t *response, void *);
void timer_vendor_control(obs_data_t *request, obs_data_t *response, void *);

QString timer_readout(obs_source_t *source, bool *end_message = nullptr, QString *layout = nullptr,
		      TimerDockStyle *style = nullptr);
void edit_timer(QWidget *parent, obs_source_t *source);
void register_countdown();
void register_countdown_dock();
void unregister_countdown_dock();

} // namespace stagekit
