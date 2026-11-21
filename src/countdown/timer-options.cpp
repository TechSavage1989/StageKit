/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "countdown.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QStringList>
#include <QSpinBox>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace stagekit {
namespace {

constexpr std::array<int64_t, 4> sizes{86400, 3600, 60, 1};
constexpr std::array<int, 4> bits{Days, Hours, Minutes, Seconds};
constexpr std::array<const char *, 4> keys{"days", "hours", "minutes", "seconds"};
constexpr std::array<const char *, 4> names{"Countdown.Days", "Countdown.Hours", "Countdown.Minutes", "Countdown.Seconds"};
constexpr std::array<int, 4> max_values{9999, 23, 59, 59};

QString label(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

int fields_for(int units, int64_t range)
{
	if (units)
		return units;
	int mask = Minutes | Seconds;
	if (range >= 3600000)
		mask |= Hours;
	if (range >= 86400000)
		mask |= Days;
	return mask;
}

class TimerEditor : public QDialog {
public:
	TimerEditor(QWidget *parent, const TimerOptions &o) : QDialog(parent)
	{
		setObjectName("stagekitTimerEditor");
		setWindowTitle(label("Countdown.Settings"));
		auto *layout = new QVBoxLayout(this);
		auto *timing = new QFormLayout;
		mode_ = new QComboBox(this);
		mode_->setObjectName("timerMode");
		for (const char *key : {"Countdown.ModeDown", "Countdown.ModeDate", "Countdown.ModeUp"})
			mode_->addItem(label(key));
		mode_->setCurrentIndex(int(o.mode));
		timing->addRow(label("Countdown.Mode"), mode_);
		limit_ = new QCheckBox(label("Countdown.Limit"), this);
		limit_->setObjectName("timerLimit");
		limit_->setChecked(o.limit);
		timing->addRow(limit_);
		duration_ = new QWidget(this);
		auto *duration_layout = new QHBoxLayout(duration_);
		duration_layout->setContentsMargins(0, 0, 0, 0);
		int64_t duration = o.duration / 1000;
		for (size_t i = 0; i < sizes.size(); ++i) {
			auto *column = new QVBoxLayout;
			auto *field = new QSpinBox(duration_);
			values_[i] = field;
			field->setObjectName(QString::fromLatin1(keys[i]));
			field->setAccessibleName(label(names[i]));
			field->setRange(0, max_values[i]);
			field->setValue(int(duration / sizes[i]));
			duration %= sizes[i];
			column->addWidget(new QLabel(label(names[i]), duration_));
			column->addWidget(field);
			duration_layout->addLayout(column);
		}
		timing->addRow(duration_);
		const auto target = o.deadline > 0 ? QDateTime::fromMSecsSinceEpoch(o.deadline) : QDateTime::currentDateTime().addSecs(3600);
		date_ = new QDateEdit(target.date(), this);
		date_->setObjectName("timerDate");
		date_->setCalendarPopup(true);
		date_->setDisplayFormat("ddd, MMM d, yyyy");
		time_ = new QTimeEdit(target.time(), this);
		time_->setObjectName("timerTime");
		const bool am_pm = QLocale().timeFormat(QLocale::ShortFormat).contains("AP", Qt::CaseInsensitive);
		time_->setDisplayFormat(am_pm ? "h:mm:ss AP" : "HH:mm:ss");
		timing->addRow(label("Countdown.Date"), date_);
		timing->addRow(label("Countdown.LocalTime"), time_);
		active_ = new QCheckBox(label("Countdown.Auto"), this);
		active_->setChecked(o.on_program);
		timing->addRow(active_);
		layout->addLayout(timing);

		auto *format = new QGroupBox(label("Countdown.Format"), this);
		auto *format_layout = new QVBoxLayout(format);
		automatic_ = new QCheckBox(label("Countdown.Automatic"), format);
		automatic_->setObjectName("timerAutomatic");
		automatic_->setChecked(o.units == 0);
		format_layout->addWidget(automatic_);
		auto *units_row = new QHBoxLayout;
		const auto initial_units = fields_for(o.units, o.mode == TimerMode::Deadline ? std::max<int64_t>(0, o.deadline - QDateTime::currentMSecsSinceEpoch()) : o.duration);
		for (size_t i = 0; i < units_.size(); ++i) {
			units_[i] = new QCheckBox(label(names[i]), format);
			units_[i]->setObjectName(QStringLiteral("unit_") + QString::fromLatin1(keys[i]));
			units_[i]->setChecked((initial_units & bits[i]) != 0);
			units_row->addWidget(units_[i]);
		}
		format_layout->addLayout(units_row);
		example_ = new QLabel(format);
		example_->setObjectName("timerExample");
		format_layout->addWidget(example_);
		layout->addWidget(format);

		end_ = new QGroupBox(label("Countdown.AtEnd"), this);
		auto *end_layout = new QFormLayout(end_);
		message_ = new QLineEdit(o.message, end_);
		message_->setObjectName("timerMessage");
		message_->setPlaceholderText(label("Countdown.KeepTime"));
		end_layout->addRow(label("Countdown.Message"), message_);
		scene_ = new QComboBox(end_);
		scene_->setObjectName("timerEndScene");
		scene_->addItem(label("Countdown.KeepScene"), QString());
		obs_frontend_source_list scenes{};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; ++i) {
			auto *source = scenes.sources.array[i];
			scene_->addItem(QString::fromUtf8(obs_source_get_name(source)), QString::fromUtf8(obs_source_get_uuid(source)));
		}
		obs_frontend_source_list_free(&scenes);
		scene_->setCurrentIndex(std::max(0, scene_->findData(o.scene)));
		end_layout->addRow(label("Countdown.SwitchScene"), scene_);
		layout->addWidget(end_);

		buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons_);
		auto refresh = [this, timing] {
			const auto options = read();
			const bool target_mode = options.mode == TimerMode::Deadline;
			limit_->setVisible(options.mode == TimerMode::CountUp);
			duration_->setVisible(!target_mode && !options.unlimited());
			for (QWidget *field : {static_cast<QWidget *>(date_), static_cast<QWidget *>(time_)}) {
				field->setVisible(target_mode);
				timing->labelForField(field)->setVisible(target_mode);
			}
			end_->setVisible(!options.unlimited());
			for (auto *unit : units_)
				unit->setEnabled(!automatic_->isChecked());
			const int64_t remaining = target_mode ? std::max<int64_t>(0, options.deadline - QDateTime::currentMSecsSinceEpoch()) : options.duration;
			const auto value = options.mode == TimerMode::CountUp ? 0 : remaining;
			example_->setText(label("Countdown.Example").arg(timer_text(value, options.units, options.mode != TimerMode::CountUp, options.unlimited() ? 0 : remaining)));
			const bool some_unit = automatic_->isChecked() || std::any_of(units_.begin(), units_.end(), [](const auto *u) { return u->isChecked(); });
			buttons_->button(QDialogButtonBox::Ok)->setEnabled(some_unit && (!target_mode || options.deadline > 0));
		};
		connect(mode_, &QComboBox::currentIndexChanged, this, refresh);
		for (auto *box : units_)
			connect(box, &QCheckBox::toggled, this, refresh);
		for (auto *box : {limit_, automatic_})
			connect(box, &QCheckBox::toggled, this, refresh);
		for (auto *field : values_)
			connect(field, &QSpinBox::valueChanged, this, refresh);
		connect(date_, &QDateEdit::dateChanged, this, refresh);
		connect(time_, &QTimeEdit::timeChanged, this, refresh);
		refresh();
	}

	TimerOptions read() const
	{
		TimerOptions o;
		o.mode = static_cast<TimerMode>(mode_->currentIndex());
		o.duration = 0;
		for (size_t i = 0; i < sizes.size(); ++i) {
			o.duration += values_[i]->value() * sizes[i] * 1000;
			if (!automatic_->isChecked() && units_[i]->isChecked())
				o.units |= bits[i];
		}
		const QDateTime chosen(date_->date(), time_->time());
		o.deadline = chosen.isValid() ? chosen.toMSecsSinceEpoch() : 0;
		o.limit = limit_->isChecked();
		o.on_program = active_->isChecked();
		o.message = message_->text();
		o.scene = scene_->currentData().toString();
		return o;
	}

	void set_private(bool private_timer) { active_->setVisible(!private_timer); }

private:
	QComboBox *mode_, *scene_;
	QDateEdit *date_;
	QTimeEdit *time_;
	QLineEdit *message_;
	QCheckBox *limit_, *active_, *automatic_;
	QWidget *duration_;
	QGroupBox *end_;
	QLabel *example_;
	QDialogButtonBox *buttons_;
	std::array<QSpinBox *, 4> values_{};
	std::array<QCheckBox *, 4> units_{};
};

} // namespace

QString timer_text(int64_t milliseconds, int units, bool round_up, int64_t range)
{
	const int mask = fields_for(units, std::max(range, milliseconds));
	milliseconds = std::max<int64_t>(0, milliseconds);
	int64_t quantum = 1;
	for (size_t i = 0; i < bits.size(); ++i)
		if (mask & bits[i])
			quantum = sizes[i];
	const auto quantum_ms = quantum * 1000;
	int64_t remaining = ((milliseconds + (round_up ? quantum_ms - 1 : 0)) / quantum_ms) * quantum;
	QStringList fields;
	for (size_t i = 0; i < sizes.size(); ++i) {
		if (!(mask & bits[i]))
			continue;
		fields << QString::number(remaining / sizes[i]).rightJustified(2, QLatin1Char('0'));
		remaining %= sizes[i];
	}
	return fields.join(QLatin1Char(':'));
}

TimerOptions TimerOptions::read(obs_data_t *s)
{
	TimerOptions o;
	o.mode = static_cast<TimerMode>(std::clamp<long long>(obs_data_get_int(s, "timer_mode"), 0, 2));
	o.duration = 0;
	for (size_t i = 0; i < sizes.size(); ++i)
		o.duration += std::clamp<long long>(obs_data_get_int(s, keys[i]), 0, max_values[i]) * sizes[i] * 1000;
	o.deadline = obs_data_get_int(s, "deadline_ms");
	o.limit = obs_data_get_bool(s, "limit_up");
	o.on_program = obs_data_get_bool(s, "restart_when_active");
	o.units = int(obs_data_get_int(s, "display_units")) & 15;
	o.message = QString::fromUtf8(obs_data_get_string(s, "end_message"));
	o.scene = QString::fromUtf8(obs_data_get_string(s, "end_scene_uuid"));
	return o;
}

void TimerOptions::write(obs_data_t *s) const
{
	int64_t seconds = duration / 1000;
	for (size_t i = 0; i < sizes.size(); ++i) {
		obs_data_set_int(s, keys[i], seconds / sizes[i]);
		seconds %= sizes[i];
	}
	obs_data_set_int(s, "timer_mode", int(mode));
	obs_data_set_int(s, "deadline_ms", deadline);
	obs_data_set_bool(s, "limit_up", limit);
	obs_data_set_bool(s, "restart_when_active", on_program);
	obs_data_set_int(s, "display_units", units);
	obs_data_set_string(s, "end_message", message.toUtf8().constData());
	obs_data_set_string(s, "end_scene_uuid", scene.toUtf8().constData());
}

bool TimerOptions::same_timing(const TimerOptions &o) const
{
	if (mode != o.mode)
		return false;
	if (mode == TimerMode::Deadline)
		return deadline == o.deadline;
	if (mode == TimerMode::CountUp && limit != o.limit)
		return false;
	return unlimited() || duration == o.duration;
}

void edit_timer(QWidget *parent, obs_source_t *source)
{
	if (!source)
		return;
	OBSSourceAutoRelease keep = obs_source_get_ref(source);
	OBSDataAutoRelease current = obs_source_get_settings(source);
	TimerEditor dialog(parent, TimerOptions::read(current));
	dialog.set_private(obs_obj_is_private(source));
	if (dialog.exec() != QDialog::Accepted || obs_source_removed(source))
		return;
	OBSDataAutoRelease changes = obs_data_create();
	dialog.read().write(changes);
	obs_source_update(source, changes);
	obs_frontend_save();
}

QString timer_readout(obs_source_t *source)
{
	calldata_t cd{};
	proc_handler_call(obs_source_get_proc_handler(source), "timer_readout", &cd);
	const QString value = QString::fromUtf8(calldata_string(&cd, "text"));
	calldata_free(&cd);
	return value;
}

} // namespace stagekit
