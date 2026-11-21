/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QTime>
#include <QWidget>
#include <array>
#include <cstdint>
#include <functional>

class QLabel;
class QPushButton;
class QSpinBox;

namespace stagekit {

class ClockFace;

class ClockPicker : public QWidget {
public:
	ClockPicker(int64_t duration_ms, QTime target, QWidget *parent);
	void set_wall_time(bool wall);
	int64_t duration_ms() const;
	QTime local_time() const { return target_; }
	std::function<void()> changed;

private:
	void select_unit(int unit);
	void select_value(int value);
	void sync();
	void notify();

	ClockFace *face_;
	QLabel *time_label_;
	std::array<QPushButton *, 3> units_{};
	QWidget *duration_row_, *period_row_;
	QSpinBox *days_;
	QPushButton *am_, *pm_;
	QTime duration_, target_;
	int unit_ = 1;
	bool wall_ = false;
};

} // namespace stagekit
