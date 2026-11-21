/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "clock-picker.hpp"

#include <obs-module.h>
#include <QDial>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace stagekit {
namespace {

constexpr double pi = 3.14159265358979323846;
constexpr double face_size = 218.0;
constexpr double outer = 79.0, inner = 48.0;

QString label(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QPointF hand_point(int n, int steps, double radius)
{
	const double angle = n * 2.0 * pi / steps;
	return {std::sin(angle) * radius, -std::cos(angle) * radius};
}

QString digits(int value)
{
	return QString::number(value).rightJustified(2, QLatin1Char('0'));
}

} // namespace

class ClockFace : public QDial {
public:
	explicit ClockFace(QWidget *parent) : QDial(parent)
	{
		setObjectName("clockFace");
		setFocusPolicy(Qt::StrongFocus);
		setWrapping(true);
		setMinimumSize(184, 184);
		setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	}

	QSize sizeHint() const override { return {218, 218}; }

	void configure(bool hours, bool wall, int current, const QString &field)
	{
		hours_ = hours;
		two_rings_ = hours && !wall;
		const QSignalBlocker block(this);
		setRange(0, hours ? (two_rings_ ? 23 : 11) : 59);
		setPageStep(hours ? 1 : 5);
		setValue(current);
		setAccessibleName(field);
		setToolTip(label("Countdown.ClockHelp"));
		update();
	}

	std::function<void()> picked;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.translate(width() / 2.0, height() / 2.0);
		const double scale = std::min(width(), height()) / face_size;
		p.scale(scale, scale);
		const auto pal = palette();
		const auto accent = pal.color(QPalette::Highlight);
		const auto ink = pal.color(QPalette::Text);
		p.setPen(QPen(hasFocus() ? accent : pal.color(QPalette::Mid), hasFocus() ? 2.0 : 1.0));
		p.setBrush(pal.color(QPalette::Base));
		p.drawEllipse(QPointF(), 104.0, 104.0);
		for (int i = 0; i < 60; ++i) {
			p.setPen(QPen(i % 5 ? pal.color(QPalette::Mid) : ink, i % 5 ? 1.0 : 1.5));
			p.drawLine(hand_point(i, 60, i % 5 ? 97 : 94), hand_point(i, 60, 100));
		}
		const int steps = hours_ ? 12 : 60;
		const auto tip = hand_point(value() % steps, steps, two_rings_ && value() >= 12 ? inner : outer);
		p.setPen(QPen(accent, 2.5, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(QPointF(), tip);
		p.setPen(Qt::NoPen);
		p.setBrush(accent);
		p.drawEllipse(QPointF(), 3.5, 3.5);
		const double radius = hours_ || value() % 5 == 0 ? 12 : 4;
		p.drawEllipse(tip, radius, radius);
		for (int ring = 0; ring < (two_rings_ ? 2 : 1); ++ring) {
			QFont f = font();
			f.setPixelSize(ring ? 12 : 14);
			p.setFont(f);
			for (int i = 0; i < 12; ++i) {
				const int n = hours_ ? i + 12 * ring : i * 5;
				const QPointF at = hand_point(i, 12, ring ? inner : outer);
				p.setPen(n == value() ? pal.color(QPalette::HighlightedText) : ink);
				const QString text = hours_ ? QString::number(!two_rings_ && n == 0 ? 12 : n) : digits(n);
				p.drawText(QRectF(at.x() - 17, at.y() - 12, 34, 24), Qt::AlignCenter, text);
			}
		}
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		dragging_ = select_at(e->position());
		if (dragging_)
			setFocus(Qt::MouseFocusReason);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (dragging_)
			select_at(e->position());
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && dragging_) {
			const bool selected = select_at(e->position());
			dragging_ = false;
			if (selected && picked)
				picked();
		}
	}

private:
	bool select_at(QPointF at)
	{
		const double scale = std::min(width(), height()) / face_size;
		const auto v = (at - QPointF(width() / 2.0, height() / 2.0)) / scale;
		const double r = std::hypot(v.x(), v.y());
		if (r < 19 || r > 106)
			return false;
		const int steps = hours_ ? 12 : 60;
		const double a = std::atan2(v.x(), -v.y());
		int n = (int(std::lround(steps * a / (2.0 * pi))) + steps) % steps;
		if (two_rings_ && r < (outer + inner) / 2.0)
			n += 12;
		setValue(n);
		return true;
	}

	bool hours_ = false, two_rings_ = false, dragging_ = false;
};

ClockPicker::ClockPicker(int64_t duration_ms, QTime target, QWidget *parent) : QWidget(parent), target_(target)
{
	const int64_t total = duration_ms / 1000;
	duration_ = QTime(0, 0).addSecs(int(total % 86400));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	time_label_ = new QLabel(label("Countdown.LocalTime"), this);
	layout->addWidget(time_label_, 0, Qt::AlignHCenter);
	auto *fields = new QHBoxLayout;
	const char *ids[] = {"clockHour", "clockMinute", "clockSecond"};
	const char *names[] = {"Countdown.Hours", "Countdown.Minutes", "Countdown.Seconds"};
	for (int i = 0; i < 3; ++i) {
		auto *col = new QVBoxLayout;
		col->setSpacing(2);
		auto *title = new QLabel(label(names[i]), this);
		title->setAlignment(Qt::AlignCenter);
		auto *b = new QPushButton(this);
		units_[i] = b;
		b->setObjectName(ids[i]);
		b->setAccessibleName(label(names[i]));
		b->setCheckable(true);
		b->setAutoDefault(false);
		connect(b, &QPushButton::clicked, this, [this, i] { select_unit(i); });
		col->addWidget(title);
		col->addWidget(b);
		fields->addLayout(col);
	}
	layout->addLayout(fields);
	face_ = new ClockFace(this);
	layout->addWidget(face_, 0, Qt::AlignHCenter);
	connect(face_, &QDial::valueChanged, this, [this](int n) { select_value(n); });
	face_->picked = [this] {
		if (unit_ == 0)
			select_unit(1);
	};

	period_row_ = new QWidget(this);
	auto *period = new QHBoxLayout(period_row_);
	period->setContentsMargins(0, 0, 0, 0);
	auto period_button = [&](bool pm) {
		auto *b = new QPushButton(pm ? QLocale().pmText() : QLocale().amText(), period_row_);
		b->setObjectName(pm ? "clockPM" : "clockAM");
		b->setCheckable(true);
		b->setAutoDefault(false);
		period->addWidget(b);
		connect(b, &QPushButton::clicked, this, [this, pm] {
			target_.setHMS(target_.hour() % 12 + (pm ? 12 : 0), target_.minute(), target_.second());
			notify();
		});
		return b;
	};
	am_ = period_button(false);
	pm_ = period_button(true);
	layout->addWidget(period_row_);

	duration_row_ = new QWidget(this);
	auto *duration_layout = new QVBoxLayout(duration_row_);
	duration_layout->setContentsMargins(0, 0, 0, 0);
	auto *presets = new QHBoxLayout;
	for (int mins : {1, 5, 15, 30, 60}) {
		auto *b = new QPushButton(mins == 60 ? label("Countdown.OneHour") : label("Countdown.Mins").arg(mins), duration_row_);
		b->setObjectName(QStringLiteral("clockPreset%1").arg(mins));
		b->setAutoDefault(false);
		presets->addWidget(b);
		connect(b, &QPushButton::clicked, this, [this, mins] {
			const QSignalBlocker block(days_);
			days_->setValue(0);
			duration_ = QTime(0, 0).addSecs(mins * 60);
			unit_ = 1;
			notify();
		});
	}
	duration_layout->addLayout(presets);
	auto *days_row = new QHBoxLayout;
	days_row->addWidget(new QLabel(label("Countdown.Days"), duration_row_));
	days_ = new QSpinBox(duration_row_);
	days_->setObjectName("clockDays");
	days_->setAccessibleName(label("Countdown.Days"));
	days_->setRange(0, 9999);
	days_->setValue(int(total / 86400));
	days_row->addWidget(days_);
	duration_layout->addLayout(days_row);
	layout->addWidget(duration_row_);
	connect(days_, &QSpinBox::valueChanged, this, [this] { notify(); });
	sync();
}

void ClockPicker::set_wall_time(bool wall)
{
	if (wall_ == wall)
		return;
	wall_ = wall;
	select_unit(wall ? 0 : 1);
}

void ClockPicker::select_unit(int unit)
{
	unit_ = unit;
	sync();
}

void ClockPicker::select_value(int value)
{
	auto &t = wall_ ? target_ : duration_;
	if (unit_ == 0)
		t.setHMS(value + (wall_ && t.hour() >= 12 ? 12 : 0), t.minute(), t.second());
	else if (unit_ == 1)
		t.setHMS(t.hour(), value, t.second());
	else
		t.setHMS(t.hour(), t.minute(), value);
	notify();
}

void ClockPicker::notify()
{
	sync();
	if (changed)
		changed();
}

void ClockPicker::sync()
{
	const auto t = wall_ ? target_ : duration_;
	const int h = wall_ ? t.hour() % 12 : t.hour();
	const int values[] = {h, t.minute(), t.second()};
	for (int i = 0; i < 3; ++i) {
		units_[i]->setText(digits(wall_ && !i && h == 0 ? 12 : values[i]));
		units_[i]->setChecked(i == unit_);
	}
	face_->configure(unit_ == 0, wall_, values[unit_], units_[unit_]->accessibleName());
	time_label_->setVisible(wall_);
	period_row_->setVisible(wall_);
	duration_row_->setVisible(!wall_);
	am_->setChecked(t.hour() < 12);
	pm_->setChecked(t.hour() >= 12);
}

int64_t ClockPicker::duration_ms() const
{
	return (int64_t(days_->value()) * 86400 + QTime(0, 0).secsTo(duration_)) * 1000;
}

} // namespace stagekit
