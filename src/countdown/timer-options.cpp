/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "countdown.hpp"
#include "clock-picker.hpp"

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
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace stagekit {
namespace {

constexpr std::array<int64_t, 4> sizes{86400, 3600, 60, 1};
constexpr std::array<int, 4> bits{Days, Hours, Minutes, Seconds};
constexpr std::array<const char *, 4> keys{"days", "hours", "minutes", "seconds"};
constexpr std::array<const char *, 4> names{"Countdown.Days", "Countdown.Hours", "Countdown.Minutes",
					    "Countdown.Seconds"};
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
		const auto target = o.deadline > 0 ? QDateTime::fromMSecsSinceEpoch(o.deadline)
						   : QDateTime::currentDateTime().addSecs(3600);
		date_ = new QDateEdit(target.date(), this);
		date_->setObjectName("timerDate");
		date_->setCalendarPopup(true);
		date_->setDisplayFormat("ddd, MMM d, yyyy");
		timing->addRow(label("Countdown.Date"), date_);
		layout->addLayout(timing);

		auto *body = new QHBoxLayout;
		clock_ = new ClockPicker(o.duration, target.time(), this);
		exact_deadline_ = target;
		clock_->deadline_picked = [this](const QDateTime &instant) {
			exact_deadline_ = instant;
			date_->setDate(instant.date());
		};
		body->addWidget(clock_, 0, Qt::AlignTop);
		auto *options_layout = new QVBoxLayout;
		body->addLayout(options_layout, 1);
		layout->addLayout(body);
		auto option = [this](const char *key, const char *id, bool checked) {
			auto *box = new QCheckBox(label(key), this);
			box->setObjectName(QString::fromLatin1(id));
			box->setChecked(checked);
			return box;
		};
		auto *start_group = new QGroupBox(label("Countdown.StartSettings"), this);
		auto *start_layout = new QVBoxLayout(start_group);
		stream_ = option("Countdown.OnStream", "timerOnStream", o.on_stream);
		start_layout->addWidget(stream_);
		active_ = new QCheckBox(label("Countdown.Auto"), this);
		active_->setChecked(o.on_program);
		start_layout->addWidget(active_);
		options_layout->addWidget(start_group);
		date_up_ = option("Countdown.DateUp", "timerDateUp", o.date_up);
		timing->addRow(date_up_);
		steady_ = option("Countdown.Steady", "timerSteady", o.steady);
		steady_->setToolTip(label("Countdown.SteadyHelp"));
		timing->addRow(steady_);

		auto *format = new QGroupBox(label("Countdown.Format"), this);
		auto *format_layout = new QVBoxLayout(format);
		automatic_ = new QCheckBox(label("Countdown.Automatic"), format);
		automatic_->setObjectName("timerAutomatic");
		automatic_->setChecked(o.units == 0);
		format_layout->addWidget(automatic_);
		auto *units_row = new QHBoxLayout;
		const auto initial_units = fields_for(
			o.units, o.mode == TimerMode::Deadline
					 ? std::max<int64_t>(0, o.deadline - QDateTime::currentMSecsSinceEpoch())
					 : o.duration);
		for (size_t i = 0; i < units_.size(); ++i) {
			units_[i] = new QCheckBox(label(names[i]), format);
			units_[i]->setObjectName(QStringLiteral("unit_") + QString::fromLatin1(keys[i]));
			units_[i]->setChecked((initial_units & bits[i]) != 0);
			units_row->addWidget(units_[i]);
		}
		format_layout->addLayout(units_row);
		leading_ = option("Countdown.LeadingZero", "timerLeadingZero", o.leading_zero);
		format_layout->addWidget(leading_);
		auto *output_row = new QHBoxLayout;
		format_ = option("Countdown.FormatOutput", "timerFormatOutput", o.format_output);
		output_ = new QLineEdit(o.output_format, format);
		output_->setObjectName("timerOutputPattern");
		output_->setAccessibleName(label("Countdown.FormatOutput"));
		output_->setPlaceholderText(QStringLiteral("%time%"));
		output_->setToolTip(label("Countdown.PatternHelp"));
		format_->setToolTip(output_->toolTip());
		output_row->addWidget(format_);
		output_row->addWidget(output_, 1);
		format_layout->addLayout(output_row);
		example_ = new QLabel(format);
		example_->setTextFormat(Qt::PlainText);
		example_->setWordWrap(true);
		example_->setObjectName("timerExample");
		format_layout->addWidget(example_);
		options_layout->addWidget(format);

		end_ = new QGroupBox(label("Countdown.AtEnd"), this);
		auto *end_layout = new QFormLayout(end_);
		message_ = new QLineEdit(o.message, end_);
		message_->setObjectName("timerMessage");
		message_->setAccessibleName(label("Countdown.Message"));
		message_->setPlaceholderText(label("Countdown.KeepTime"));
		message_on_ = option("Countdown.Message", "timerMessageEnabled", o.use_message);
		message_->setToolTip(label("Countdown.PatternHelp"));
		end_layout->addRow(message_on_, message_);
		scene_ = new QComboBox(end_);
		scene_->setObjectName("timerEndScene");
		scene_->setAccessibleName(label("Countdown.SwitchScene"));
		scene_->addItem(label("Countdown.KeepScene"), QString());
		obs_frontend_source_list scenes{};
		obs_frontend_get_scenes(&scenes);
		for (size_t i = 0; i < scenes.sources.num; ++i) {
			auto *source = scenes.sources.array[i];
			scene_->addItem(QString::fromUtf8(obs_source_get_name(source)),
					QString::fromUtf8(obs_source_get_uuid(source)));
		}
		obs_frontend_source_list_free(&scenes);
		scene_->setCurrentIndex(std::max(0, scene_->findData(o.scene)));
		scene_on_ = option("Countdown.SwitchScene", "timerSceneEnabled", o.switch_scene);
		end_layout->addRow(scene_on_, scene_);
		options_layout->addWidget(end_);
		options_layout->addStretch();

		buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(buttons_);
		auto refresh = [this, timing] {
			const auto options = read();
			const bool target_mode = options.mode == TimerMode::Deadline;
			limit_->setVisible(options.mode == TimerMode::CountUp);
			clock_->set_wall_time(target_mode);
			clock_->setVisible(!options.unlimited());
			date_->setVisible(target_mode);
			date_up_->setVisible(target_mode);
			steady_->setVisible(!target_mode);
			output_->setEnabled(format_->isChecked());
			message_->setEnabled(message_on_->isChecked());
			scene_->setEnabled(scene_on_->isChecked());
			timing->labelForField(date_)->setVisible(target_mode);
			end_->setVisible(!options.unlimited());
			for (auto *unit : units_)
				unit->setEnabled(!automatic_->isChecked());
			const int64_t remaining =
				target_mode
					? std::max<int64_t>(0, options.deadline - QDateTime::currentMSecsSinceEpoch())
					: options.duration;
			const auto value = options.counts_up() ? 0 : remaining;
			example_->setText(
				label("Countdown.Example")
					.arg(options.display_text(value, options.unlimited() ? 0 : remaining)));
			const bool some_unit =
				automatic_->isChecked() ||
				std::any_of(units_.begin(), units_.end(), [](const auto *u) { return u->isChecked(); });
			buttons_->button(QDialogButtonBox::Ok)
				->setEnabled(some_unit && (!target_mode || options.deadline > 0));
		};
		connect(mode_, &QComboBox::currentIndexChanged, this, refresh);
		for (auto *box : units_)
			connect(box, &QCheckBox::toggled, this, refresh);
		for (auto *box : {limit_, automatic_, date_up_, leading_, format_, message_on_, scene_on_})
			connect(box, &QCheckBox::toggled, this, refresh);
		connect(output_, &QLineEdit::textChanged, this, refresh);
		clock_->changed = refresh;
		connect(date_, &QDateEdit::dateChanged, this, refresh);
		refresh();
	}

	TimerOptions read() const
	{
		TimerOptions o;
		o.mode = static_cast<TimerMode>(mode_->currentIndex());
		o.duration = clock_->duration_ms();
		for (size_t i = 0; i < units_.size(); ++i)
			if (!automatic_->isChecked() && units_[i]->isChecked())
				o.units |= bits[i];
		// Keep the selected occurrence when local clocks repeat an hour.
		const QDateTime chosen = exact_deadline_.date() == date_->date() &&
							 exact_deadline_.time() == clock_->local_time()
						 ? exact_deadline_
						 : QDateTime(date_->date(), clock_->local_time());
		o.deadline = chosen.isValid() ? chosen.toMSecsSinceEpoch() : 0;
		o.limit = limit_->isChecked();
		o.on_program = active_->isChecked();
		o.on_stream = stream_->isChecked();
		o.steady = steady_->isChecked();
		o.date_up = date_up_->isChecked();
		o.leading_zero = leading_->isChecked();
		o.format_output = format_->isChecked();
		o.output_format = output_->text();
		o.use_message = message_on_->isChecked();
		o.switch_scene = scene_on_->isChecked();
		o.message = message_->text();
		o.scene = scene_->currentData().toString();
		return o;
	}

	void set_private(bool private_timer) { active_->setVisible(!private_timer); }

private:
	QComboBox *mode_, *scene_;
	QDateEdit *date_;
	QDateTime exact_deadline_;
	ClockPicker *clock_;
	QLineEdit *message_, *output_;
	QCheckBox *limit_, *active_, *automatic_, *stream_, *steady_, *date_up_;
	QCheckBox *leading_, *format_, *message_on_, *scene_on_;
	QGroupBox *end_;
	QLabel *example_;
	QDialogButtonBox *buttons_;
	std::array<QCheckBox *, 4> units_{};
};

} // namespace

QString TimerOptions::clock_text(int64_t milliseconds, int64_t range) const
{
	const int mask = fields_for(units, std::max(range, milliseconds));
	milliseconds = std::max<int64_t>(0, milliseconds);
	int64_t quantum = 1;
	for (size_t i = 0; i < bits.size(); ++i)
		if (mask & bits[i])
			quantum = sizes[i];
	const bool round_up = !counts_up();
	const auto quantum_ms = quantum * 1000;
	int64_t remaining = ((milliseconds + (round_up ? quantum_ms - 1 : 0)) / quantum_ms) * quantum;
	QStringList fields;
	for (size_t i = 0; i < sizes.size(); ++i) {
		if (!(mask & bits[i]))
			continue;
		const int digits = fields.isEmpty() && !leading_zero ? 1 : 2;
		fields << QString::number(remaining / sizes[i]).rightJustified(digits, QLatin1Char('0'));
		remaining %= sizes[i];
	}
	return fields.join(QLatin1Char(':'));
}

QString TimerOptions::display_text(int64_t milliseconds, int64_t range, bool at_end) const
{
	const QString time = clock_text(milliseconds, range);
	if (!at_end && !format_output)
		return time;
	QString pattern = at_end ? message : output_format;
	pattern.replace(QStringLiteral("%time%"), time);
	if (!pattern.contains(QLatin1Char('%')))
		return pattern;
	int64_t parts = (std::max<int64_t>(0, milliseconds) + (counts_up() ? 0 : 999)) / 1000;
	for (size_t i = 0; i < keys.size(); ++i) {
		const auto key = QString::fromLatin1(keys[i]);
		const auto value = QString::number(parts / sizes[i]);
		const auto padded = value.rightJustified(2, QLatin1Char('0'));
		pattern.replace(QLatin1Char('%') + key + QLatin1Char('%'), value);
		pattern.replace(QLatin1Char('%') + key + QStringLiteral("2%"), padded);
		parts %= sizes[i];
	}
	return pattern;
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
	o.on_stream = obs_data_get_bool(s, "start_on_stream");
	o.steady = obs_data_get_bool(s, "steady_seconds");
	o.date_up = obs_data_get_bool(s, "count_up_to_date");
	o.leading_zero = !obs_data_has_user_value(s, "leading_zero") || obs_data_get_bool(s, "leading_zero");
	o.format_output = obs_data_get_bool(s, "format_output");
	if (obs_data_has_user_value(s, "output_format"))
		o.output_format = QString::fromUtf8(obs_data_get_string(s, "output_format"));
	o.units = int(obs_data_get_int(s, "display_units")) & 15;
	o.message = QString::fromUtf8(obs_data_get_string(s, "end_message"));
	o.scene = QString::fromUtf8(obs_data_get_string(s, "end_scene_uuid"));
	// Older collections enabled an action by giving it a message or scene.
	o.use_message = obs_data_has_user_value(s, "use_end_message") ? obs_data_get_bool(s, "use_end_message")
								      : !o.message.isEmpty();
	o.switch_scene = obs_data_has_user_value(s, "switch_end_scene") ? obs_data_get_bool(s, "switch_end_scene")
									: !o.scene.isEmpty();
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
	obs_data_set_bool(s, "start_on_stream", on_stream);
	obs_data_set_bool(s, "steady_seconds", steady);
	obs_data_set_bool(s, "count_up_to_date", date_up);
	obs_data_set_bool(s, "leading_zero", leading_zero);
	obs_data_set_bool(s, "format_output", format_output);
	obs_data_set_string(s, "output_format", output_format.toUtf8().constData());
	obs_data_set_bool(s, "use_end_message", use_message);
	obs_data_set_bool(s, "switch_end_scene", switch_scene);
	obs_data_set_int(s, "display_units", units);
	obs_data_set_string(s, "end_message", message.toUtf8().constData());
	obs_data_set_string(s, "end_scene_uuid", scene.toUtf8().constData());
}

bool TimerOptions::same_timing(const TimerOptions &o) const
{
	if (mode != o.mode)
		return false;
	if (mode == TimerMode::Deadline)
		return deadline == o.deadline && date_up == o.date_up;
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

QString timer_readout(obs_source_t *source, bool *end_message)
{
	calldata_t cd{};
	proc_handler_call(obs_source_get_proc_handler(source), "timer_readout", &cd);
	const QString value = QString::fromUtf8(calldata_string(&cd, "text"));
	if (end_message)
		*end_message = calldata_bool(&cd, "end_message");
	calldata_free(&cd);
	return value;
}

} // namespace stagekit
