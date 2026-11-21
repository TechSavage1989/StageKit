#pragma once

#include "countdown-test.hpp"
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDateEdit>
#include <QDial>
#include <QMouseEvent>
#include <cmath>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QTimer>
#include <QDockWidget>
#include <QLabel>
#include <QImage>
#include <QLayout>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <graphics/matrix4.h>

namespace countdown_dock_test {

template<typename Predicate> bool until(Predicate predicate)
{
	return countdown_test::wait_for([&] {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
		return predicate();
	});
}

// Drive the actual clock with a press, drag and release between two marks.
inline bool clock_pick(QDialog *dialog, const char *field, int tick, int divisions, bool inner = false)
{
	auto *button = dialog->findChild<QPushButton *>(field);
	auto *face = dialog->findChild<QDial *>("clockFace");
	if (!button || !face || !face->isVisible())
		return false;
	button->click();
	const QPointF centre(face->width() / 2.0, face->height() / 2.0);
	const double radius = std::min(face->width(), face->height()) * (inner ? 0.22 : 0.36);
	auto point = [&](int n) {
		const double a = n * 6.283185307179586 / divisions;
		return centre + QPointF(std::sin(a) * radius, -std::cos(a) * radius);
	};
	auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton b, Qt::MouseButtons held) {
		QMouseEvent event(type, at, face->mapToGlobal(at), b, held, Qt::NoModifier);
		QApplication::sendEvent(face, &event);
	};
	send(QEvent::MouseButtonPress, point(tick + 1), Qt::LeftButton, Qt::LeftButton);
	send(QEvent::MouseMove, point(tick), Qt::NoButton, Qt::LeftButton);
	send(QEvent::MouseButtonRelease, point(tick), Qt::LeftButton, Qt::NoButton);
	return true;
}

// Measure the glyphs OBS actually painted, including its current stylesheet.
inline int ink_height(QLabel *label)
{
	const QImage image = label->grab().toImage().convertToFormat(QImage::Format_RGB32);
	int first = image.height(), last = -1;
	if (image.width() < 5)
		return 0;
	for (int y = 1; y + 1 < image.height(); ++y) {
		const int background = qGray(image.pixel(1, y));
		for (int x = 2; x + 2 < image.width(); ++x)
			if (std::abs(qGray(image.pixel(x, y)) - background) > 72) {
				first = std::min(first, y);
				last = y;
				break;
			}
	}
	return last < first ? 0 : int((last - first + 1) / image.devicePixelRatio());
}

inline OBSDataAutoRelease vendor(const char *name, obs_data_t *request)
{
	calldata_t call{};
	calldata_set_string(&call, "request", name);
	calldata_set_string(&call, "data", obs_data_get_json(request));
	proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &call);
	const char *json = calldata_string(&call, "response");
	OBSDataAutoRelease result = json ? obs_data_create_from_json(json) : obs_data_create();
	calldata_free(&call);
	return result;
}

// Open OBS's own window from the Place dock and apply a position and scale to this timer.
inline bool edit_transform(QMainWindow *main, obs_sceneitem_t *item)
{
	auto *picker = main->findChild<QComboBox *>("item");
	auto *button = main->findChild<QPushButton *>("editTransform");
	const QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
	if (!picker || !button || !until([&] { return picker->findText(name) >= 0; }))
		return false;
	picker->setCurrentIndex(picker->findText(name));
	button->click();
	QDialog *dialog = nullptr;
	const bool opened = until([&] {
		dialog = main->findChild<QDialog *>("OBSBasicTransform");
		return dialog && dialog->isVisible() && dialog->windowTitle().contains(name);
	});
	if (!dialog)
		return false;
	auto *x = dialog->findChild<QDoubleSpinBox *>("positionX");
	auto *y = dialog->findChild<QDoubleSpinBox *>("positionY");
	auto *width = dialog->findChild<QDoubleSpinBox *>("sizeX");
	auto *height = dialog->findChild<QDoubleSpinBox *>("sizeY");
	const bool ready = opened && x && y && width && height;
	if (ready) {
		x->setValue(64);
		y->setValue(36);
		width->setValue(480);
		height->setValue(135);
	}
	dialog->close();
	obs_transform_info result{};
	obs_sceneitem_get_info2(item, &result);
	const bool placed = std::abs(result.pos.x - 64) < 0.01f && std::abs(result.pos.y - 36) < 0.01f;
	const bool scaled = std::abs(result.scale.x - 0.75f) < 0.001f && std::abs(result.scale.y - 0.75f) < 0.001f;
	return ready && placed && scaled;
}

inline void check(QStringList &issues, bool restart)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = main->findChild<QWidget *>("stagekitCountdownControls");
	if (!panel) {
		issues.append("Missing Countdown Timer dock");
		return;
	}
	for (auto *p = panel->parentWidget(); p; p = p->parentWidget())
		if (auto *dock = qobject_cast<QDockWidget *>(p)) {
			dock->setFloating(true);
			dock->resize(276, 255);
			dock->show();
			dock->raise();
			break;
		}
	auto *targets = panel->findChild<QComboBox *>("countdownTarget");
	auto *readout = panel->findChild<QLabel *>("countdownReadout");
	auto *status = panel->findChild<QLabel *>("countdownStatus");
	auto *settings = panel->findChild<QPushButton *>("countdownSettings");
	auto *start = panel->findChild<QPushButton *>("countdownStart");
	auto *pause = panel->findChild<QPushButton *>("countdownPause");
	auto *reset = panel->findChild<QPushButton *>("countdownReset");
	if (!targets || !readout || !status || !settings || !start || !pause || !reset) {
		issues.append("Countdown dock is missing operator controls");
		return;
	}
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("Countdown dock: ") + QString::fromLatin1(message));
	};
	auto choose = [&](const QString &name) {
		if (!until([&] { return targets->findText(name) >= 0; }))
			return false;
		targets->setCurrentIndex(targets->findText(name));
		return true;
	};
	auto edit = [&](auto action) {
		bool opened = false;
		QObject owner;
		QTimer::singleShot(50, &owner, [&] {
			auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
			if (dialog && dialog->objectName() == "stagekitTimerEditor") {
				opened = true;
				action(dialog);
				auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
				if (ok->isEnabled())
					ok->click();
				else
					dialog->reject();
			}
		});
		QTimer::singleShot(2400, &owner, [] {
			if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
				dialog->reject();
		});
		settings->click();
		expect(opened, "the timer settings dialog did not open");
	};
	const QString shared_name = QStringLiteral("Operator timer renamed");
	if (restart) {
		expect(until([&] { return targets->currentText() == shared_name && readout->text() == "01:13"; }),
		       "the selected scene timer and its preset were not restored after restarting OBS");
		edit([&](QDialog *d) {
			expect(d->findChild<QSpinBox *>("timerBoxWidth")->value() == 640 &&
				       d->findChild<QSpinBox *>("timerBoxHeight")->value() == 180 &&
				       d->findChild<QCheckBox *>("timerOnStream")->isChecked() &&
				       !d->findChild<QCheckBox *>("timerFormatOutput")->isChecked() &&
				       d->findChild<QLineEdit *>("timerOutputPattern")->text() == "IN %time%",
			       "timer options did not survive a restart");
		});
		choose(QStringLiteral("Private timer"));
		expect(until([&] { return readout->text() == "00:07" && status->text() == "Ready"; }),
		       "the private preset was not restored in a stopped state");
		return;
	}
	expect(choose(QStringLiteral("Private timer")), "private timer missing from the selector");
	auto *mark = panel->findChild<QComboBox *>("countdownMark");
	auto *set_start = panel->findChild<QPushButton *>("countdownSetStart");
	OBSDataAutoRelease request = obs_data_create();
	OBSDataAutoRelease found = vendor("ListTimers", request);
	OBSDataArrayAutoRelease listed = obs_data_get_array(found, "timers");
	expect(obs_data_get_bool(found, "success") && obs_data_array_count(listed) > 0, "Stream Deck timer list");
	const auto press = [&](const char *id, const char *op, int minute) {
		obs_data_set_string(request, "timer", id);
		obs_data_set_string(request, "operation", op);
		obs_data_set_int(request, "mark", minute);
		return vendor("TimerControl", request);
	};
	if (mark && set_start) {
		mark->setCurrentIndex(mark->findData(45));
		set_start->click();
		expect(until([&] { return status->text() == "Running"; }),
		       "dock clock shortcut did not start the private timer");
		OBSDataAutoRelease stopped = press("private", "toggle", 45);
		expect(obs_data_get_bool(stopped, "success") && !obs_data_get_bool(stopped, "running"),
		       "Stream Deck toggle did not stop the private timer");
	} else {
		expect(false, "dock clock shortcuts missing");
	}
	OBSDataAutoRelease wrong = press("missing-timer", "setStart", 15);
	expect(!obs_data_get_bool(wrong, "success"), "a missing Stream Deck target controlled a different timer");
	edit([&](QDialog *d) {
		d->findChild<QComboBox *>("timerMode")->setCurrentIndex(0);
		auto *days = d->findChild<QSpinBox *>("clockDays");
		auto *preset = d->findChild<QPushButton *>("clockPreset15");
		expect(days && preset, "duration presets or day picker missing");
		if (!days || !preset)
			return;
		days->setValue(2);
		expect(clock_pick(d, "clockHour", 11, 12, true) &&
			       d->findChild<QPushButton *>("clockHour")->text() == "23",
		       "second hour ring did not select 23 hours");
		preset->click();
		expect(days->value() == 0 && d->findChild<QPushButton *>("clockMinute")->text() == "15" &&
			       d->findChild<QPushButton *>("clockHour")->text() == "00",
		       "duration preset");
		expect(clock_pick(d, "clockMinute", 0, 60) && clock_pick(d, "clockSecond", 7, 60),
		       "private timer clock did not accept exact seconds");
	});
	expect(until([&] { return readout->text() == "00:07"; }), "the private duration picker was not applied");
	start->click();
	expect(until([&] { return status->text() == "Running" && readout->text() == "00:06"; }),
	       "the private timer did not run");
	pause->click();
	expect(until([&] { return status->text() == "Paused"; }), "the private timer did not pause");
	const auto held = readout->text();
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_int(data, "minutes", 1);
	obs_data_set_int(data, "seconds", 13);
	OBSSourceAutoRelease source = obs_source_create("stagekit_countdown", "Operator timer", data, nullptr);
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	if (!source || !scene || !obs_scene_add(obs_scene_from_source(scene), source)) {
		issues.append("Could not place the countdown test source in the fixture scene");
		return;
	}
	expect(choose(QStringLiteral("Operator timer")) && until([&] { return readout->text() == "01:13"; }),
	       "the scene timer was not available in the dock");
	edit([&](QDialog *d) {
		auto *width = d->findChild<QSpinBox *>("timerBoxWidth");
		auto *height = d->findChild<QSpinBox *>("timerBoxHeight");
		auto *square = d->findChild<QToolButton *>("timerAlign3");
		expect(width && height && square, "timer box controls missing");
		if (width && height && square) {
			width->setValue(640);
			height->setValue(180);
			square->click();
		}
	});
	expect(until([&] { return obs_source_get_width(source) == 640 && obs_source_get_height(source) == 180; }),
	       "editing the timer box from its dock");
	{
		OBSSourceAutoRelease editing = obs_frontend_preview_program_mode_active()
						       ? obs_frontend_get_current_preview_scene()
						       : obs_frontend_get_current_scene();
		auto *editing_scene = obs_scene_from_source(editing);
		auto *item = editing_scene ? obs_scene_find_source(editing_scene, "Operator timer") : nullptr;
		const bool added = !item && editing_scene;
		if (added)
			item = obs_scene_add(editing_scene, source);
		auto *grid = main->findChild<QWidget *>("fitAlign");
		auto *fit_button = main->findChild<QPushButton *>("arrange_fit_space");
		expect(item && grid && fit_button, "existing Place controls for the scene timer");
		if (item && grid && fit_button) {
			obs_scene_enum_items(
				editing_scene,
				[](obs_scene_t *, obs_sceneitem_t *other, void *chosen) {
					obs_sceneitem_select(other, other == chosen);
					return true;
				},
				item);
			obs_transform_info before{};
			obs_sceneitem_get_info2(item, &before);
			constexpr int order[]{5, 1, 6, 3, 0, 4, 7, 2, 8};
			for (int cell = 0; cell < 9; ++cell) {
				auto *square = grid->findChild<QToolButton *>(QStringLiteral("fitAlign%1").arg(cell));
				if (square)
					square->click();
				expect(square && until([&] {
					       OBSDataAutoRelease current = obs_source_get_settings(source);
					       return obs_data_get_int(current, "box_alignment") == order[cell];
				       }),
				       "clicking the existing Fit to Space grid did not align the timer");
			}
			obs_transform_info after{};
			obs_sceneitem_get_info2(item, &after);
			expect(before.pos.x == after.pos.x && before.pos.y == after.pos.y &&
				       before.scale.x == after.scale.x && before.scale.y == after.scale.y,
			       "text alignment moved the scene item");
			expect(edit_transform(main, item), "Edit Transform did not move and scale the timer box");
			OBSDataAutoRelease fit_settings = obs_data_create();
			obs_data_set_string(fit_settings, "item", obs_source_get_name(source));
			obs_data_set_int(fit_settings, "box", 1);
			obs_data_set_double(fit_settings, "box_x", 40);
			obs_data_set_double(fit_settings, "box_y", 25);
			obs_data_set_double(fit_settings, "box_width", 320);
			obs_data_set_double(fit_settings, "box_height", 90);
			obs_data_set_double(fit_settings, "margin", 0);
			obs_data_set_int(fit_settings, "glide_ms", 0);
			obs_data_set_bool(fit_settings, "visible_pixels", true);
			OBSSourceAutoRelease fit =
				obs_source_create_private("stagekit_fit_item", "Fit the timer box", fit_settings);
			if (fit) {
				obs_source_filter_add(editing, fit);
				fit_button->click();
				auto placed = [&] {
					matrix4 transform;
					obs_sceneitem_force_update_transform(item);
					obs_sceneitem_get_box_transform(item, &transform);
					return std::abs(transform.x.x - 320) < 2 && std::abs(transform.y.y - 90) < 2 &&
					       std::abs(transform.t.x - 40) < 2 && std::abs(transform.t.y - 25) < 2;
				};
				expect(until(placed), "Fit to Space did not use the whole 640 by 180 timer box");
				obs_source_media_set_time(source, 61000);
				expect(until([&] { return countdown_test::readout(source) == "00:12"; }) && placed(),
				       "Fit to Space shifted the frame for shorter digits");
				obs_source_filter_remove(editing, fit);
				obs_source_media_stop(source);
			} else {
				expect(false, "creating the Fit to Space filter");
			}
			obs_sceneitem_set_info2(item, &before);
			OBSDataAutoRelease restore = obs_data_create();
			obs_data_set_int(restore, "box_alignment", 0);
			obs_source_update(source, restore);
			if (added)
				obs_sceneitem_remove(item);
		}
	}
	blog(LOG_INFO, "StageKit timer controls: checking scene timer clock marks");
	const char *id = obs_source_get_uuid(source);
	for (const int minute : {15, 30, 45, 0, 15}) {
		OBSDataAutoRelease cycled = press(id, "cycle", -2);
		expect(obs_data_get_bool(cycled, "success") && !obs_data_get_bool(cycled, "running") &&
			       obs_data_get_int(cycled, "mark") == minute,
		       "Stream Deck cycle order or idle state");
	}
	OBSDataAutoRelease held_start = press(id, "cycleToggle", -2);
	expect(obs_data_get_bool(held_start, "running") && obs_data_get_int(held_start, "mark") == 15,
	       "hold did not start the displayed mark");
	OBSDataAutoRelease live_cycle = press(id, "cycle", -2);
	expect(obs_data_get_bool(live_cycle, "running") && obs_data_get_int(live_cycle, "mark") == 30,
	       "cycling a running timer stopped it");
	OBSDataAutoRelease held_stop = press(id, "cycleToggle", -2);
	expect(obs_data_get_bool(held_stop, "success") && !obs_data_get_bool(held_stop, "running") &&
		       obs_data_get_int(held_stop, "mark") == 30,
	       "hold did not stop without advancing");
	OBSDataAutoRelease set = press(id, "set", 30);
	OBSDataAutoRelease saved = obs_source_get_settings(source);
	const auto deadline = QDateTime::fromMSecsSinceEpoch(obs_data_get_int(saved, "deadline_ms"));
	expect(obs_data_get_bool(set, "success") && !obs_data_get_bool(set, "running") &&
		       deadline.time().minute() == 30 && deadline.time().second() == 0,
	       "Stream Deck set-only deadline");
	OBSDataAutoRelease started = press(id, "setStart", 15);
	expect(obs_data_get_bool(started, "running"), "Stream Deck set-and-start");
	OBSDataAutoRelease stopped = press(id, "stop", -2);
	expect(obs_data_get_bool(stopped, "success") && !obs_data_get_bool(stopped, "running"),
	       "dedicated Stream Deck stop");
	blog(LOG_INFO, "StageKit timer controls: cycle, hold, set, start and stop checked");
	obs_data_set_int(data, "timer_mode", 0);
	obs_source_update(source, data);
	expect(until([&] {
		       return obs_source_media_get_duration(source) == 73000 && readout->text() == "01:13" &&
			      status->text() == "Ready" && start->isEnabled();
	       }),
	       "return to saved duration after a clock shortcut");
	obs_source_set_name(source, shared_name.toUtf8().constData());
	expect(until([&] { return targets->currentText() == shared_name; }),
	       "renaming a linked timer broke the selection");
	start->click();
	expect(until([&] {
		       return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING && pause->isEnabled();
	       }),
	       "the dock did not start the scene source");
	pause->click();
	expect(until([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED; }),
	       "the dock did not pause the scene source");
	obs_source_media_set_time(source, 30000);
	expect(until([&] { return readout->text() == "00:43" && status->text() == "Paused"; }),
	       "a change from OBS media controls did not appear in the dock");
	const int rendered = ink_height(readout);
	const int caption = ink_height(status);
	blog(LOG_INFO, "StageKit timer readout: %s, state %d, duration %lld, widget %dx%d, start %d, pause %d",
	     readout->text().toUtf8().constData(), int(obs_source_media_get_state(source)),
	     (long long)obs_source_media_get_duration(source), readout->width(), readout->height(), start->isEnabled(),
	     pause->isEnabled());
	blog(LOG_INFO, "StageKit timer dock: %d logical pixels wide, digits %d pixels high, status %d",
	     readout->width(), rendered, caption);
	expect(readout->width() <= 276 && rendered >= 36 && caption > 0 && rendered >= 3 * caption,
	       "the narrow dock did not paint substantially larger timer digits");
	const QString capture = qEnvironmentVariable("STAGEKIT_COUNTDOWN_CAPTURE");
	if (!capture.isEmpty()) {
		expect(panel->grab().save(capture), "could not capture the actual dock");
	}
	// The dock keeps the same prefix and suffix pixels across an unpadded minute rollover.
	OBSDataAutoRelease stable_settings = obs_data_create();
	obs_data_set_int(stable_settings, "minutes", 10);
	obs_data_set_int(stable_settings, "seconds", 11);
	obs_data_set_bool(stable_settings, "leading_zero", false);
	obs_data_set_bool(stable_settings, "format_output", true);
	obs_data_set_string(stable_settings, "output_format", "IN %time%");
	OBSSourceAutoRelease stable =
		obs_source_create("stagekit_countdown", "Dock rollover", stable_settings, nullptr);
	if (stable && choose(QStringLiteral("Dock rollover"))) {
		expect(until([&] { return readout->text() == "IN 10:11"; }), "first rollover reading");
		const QRect rect = readout->geometry();
		const QImage a = readout->grab().toImage();
		obs_source_media_set_time(stable, 60000);
		expect(until([&] { return readout->text() == "IN 9:11"; }), "second rollover reading");
		const QImage b = readout->grab().toImage();
		const QRect left(0, 0, a.width() / 3, a.height());
		const QRect right(2 * a.width() / 3, 0, a.width() / 3, a.height());
		expect(rect == readout->geometry() && a != b && a.copy(left) == b.copy(left) &&
			       a.copy(right) == b.copy(right),
		       "dock clock moved or resized when a digit disappeared");
	} else {
		expect(false, "dock rollover timer missing");
	}
	if (stable)
		obs_source_remove(stable);
	expect(choose(shared_name), "restore selected timer after testing the dock rollover");
	reset->click();
	expect(until([&] {
		       return obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED &&
			      readout->text() == "01:13";
	       }),
	       "the dock did not reset the scene source");
	expect(choose(QStringLiteral("Private timer")) &&
		       until([&] { return readout->text() == held && status->text() == "Paused"; }),
	       "operating the scene timer changed the private timer");
	choose(shared_name);
	OBSSceneAutoRelease destination = obs_scene_create("Timer finish fixture");
	const QString dest_id = QString::fromUtf8(obs_source_get_uuid(obs_scene_get_source(destination)));
	const QDate date = QDate::currentDate().addDays(2);
	const QTime at(19, 30, 0);
	edit([&](QDialog *d) {
		d->findChild<QComboBox *>("timerMode")->setCurrentIndex(1);
		auto *quarter = d->findChild<QPushButton *>("clockMark-1");
		expect(quarter && quarter->isVisible(), "next-quarter shortcut in the date picker");
		if (quarter) {
			quarter->click();
			expect(d->findChild<QPushButton *>("clockMinute")->text().toInt() % 15 == 0 &&
				       d->findChild<QPushButton *>("clockSecond")->text() == "00",
			       "clock shortcut did not select a quarter hour");
		}

		auto *calendar = d->findChild<QDateEdit *>("timerDate");
		expect(calendar->calendarPopup() && calendar->isVisible(), "calendar picker missing in date mode");
		calendar->setDate(date);
		auto *am = d->findChild<QPushButton *>("clockAM");
		auto *pm = d->findChild<QPushButton *>("clockPM");
		expect(am && pm && pm->isVisible(), "local time has no AM/PM switch");
		if (!am || !pm)
			return;
		am->click();
		expect(clock_pick(d, "clockHour", 0, 12) && d->findChild<QPushButton *>("clockHour")->text() == "12" &&
			       d->findChild<QPushButton *>("clockMinute")->isChecked(),
		       "midnight or hour-to-minute selection");
		pm->click();
		expect(clock_pick(d, "clockHour", 7, 12) && clock_pick(d, "clockMinute", 30, 60) &&
			       clock_pick(d, "clockSecond", 0, 60),
		       "setting the evening deadline on the face");
		d->findChild<QPushButton *>("clockMinute")->click();
		d->findChild<QCheckBox *>("timerMessageEnabled")->setChecked(true);
		d->findChild<QCheckBox *>("timerSceneEnabled")->setChecked(true);
		d->findChild<QCheckBox *>("timerOnStream")->setChecked(true);
		d->findChild<QCheckBox *>("timerLeadingZero")->setChecked(false);
		d->findChild<QCheckBox *>("timerFormatOutput")->setChecked(true);
		d->findChild<QLineEdit *>("timerOutputPattern")->setText("IN %time%");
		d->findChild<QLineEdit *>("timerMessage")->setText("Session complete");
		auto *scene_list = d->findChild<QComboBox *>("timerEndScene");
		const int index = scene_list->findData(dest_id);
		expect(index > 0, "the finish scene was not listed");
		if (index > 0)
			scene_list->setCurrentIndex(index);
		const QString image_path = qEnvironmentVariable("STAGEKIT_TIMER_EDITOR_CAPTURE");
		if (!image_path.isEmpty())
			expect(d->grab().save(image_path), "could not capture the date and time controls");
	});
	expect(until([&] {
		       OBSDataAutoRelease saved = obs_source_get_settings(source);
		       return obs_data_get_int(saved, "deadline_ms") == QDateTime(date, at).toMSecsSinceEpoch() &&
			      obs_data_get_int(saved, "timer_mode") == 1 &&
			      readout->text().count(QLatin1Char(':')) >= 2;
	       }),
	       "the local date and time did not reach the scene timer");
	expect(readout->text().startsWith("IN ") &&
		       readout->fontMetrics().horizontalAdvance(readout->text()) <= readout->contentsRect().width(),
	       "formatted multi-day clock was clipped in the narrow dock");
	edit([&](QDialog *d) {
		auto *message = d->findChild<QLineEdit *>("timerMessage");
		auto *use_message = d->findChild<QCheckBox *>("timerMessageEnabled");
		auto *use_scene = d->findChild<QCheckBox *>("timerSceneEnabled");
		use_message->setChecked(false);
		use_scene->setChecked(false);
		expect(!message->isEnabled() && message->text() == "Session complete" &&
			       !d->findChild<QComboBox *>("timerEndScene")->isEnabled(),
		       "disabling finish actions discarded a saved choice");
		use_message->setChecked(true);
		use_scene->setChecked(true);
		d->findChild<QCheckBox *>("timerLeadingZero")->setChecked(true);
		d->findChild<QCheckBox *>("timerFormatOutput")->setChecked(false);
		expect(d->findChild<QLineEdit *>("timerOutputPattern")->text() == "IN %time%",
		       "disabling format output discarded its template");
		d->findChild<QComboBox *>("timerMode")->setCurrentIndex(2);
		d->findChild<QCheckBox *>("timerLimit")->setChecked(true);
		expect(clock_pick(d, "clockMinute", 0, 60) && clock_pick(d, "clockSecond", 2, 60),
		       "count-up limit clock");
		d->findChild<QCheckBox *>("timerAutomatic")->setChecked(false);
		for (const char *name : {"unit_days", "unit_hours", "unit_minutes"})
			d->findChild<QCheckBox *>(name)->setChecked(false);
		d->findChild<QCheckBox *>("unit_seconds")->setChecked(true);
	});
	expect(until([&] { return readout->text() == "00"; }), "count-up and seconds-only picker");
	start->click();
	expect(until([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PLAYING; }),
	       "bounded count-up did not start");
	obs_source_media_set_time(source, 1830);
	expect(until([&] {
		       OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		       return readout->text() == "Session complete" &&
			      program.Get() == obs_scene_get_source(destination);
	       }),
	       "finish message and Program scene switch did not complete");
	obs_frontend_set_current_scene(scene);
	expect(until([&] {
		       OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		       return program.Get() == scene.Get();
	       }),
	       "could not restore the fixture scene");
	QThread::msleep(130);
	QCoreApplication::processEvents();
	OBSSourceAutoRelease still = obs_frontend_get_current_scene();
	expect(still.Get() == scene.Get(), "a finished timer switched Program more than once");
	edit([&](QDialog *d) {
		d->findChild<QComboBox *>("timerMode")->setCurrentIndex(0);
		expect(clock_pick(d, "clockMinute", 1, 60) && clock_pick(d, "clockSecond", 13, 60),
		       "saved duration clock");
		d->findChild<QCheckBox *>("timerAutomatic")->setChecked(true);
		d->findChild<QLineEdit *>("timerMessage")->clear();
		d->findChild<QComboBox *>("timerEndScene")->setCurrentIndex(0);
	});
	expect(until([&] { return readout->text() == "01:13"; }), "restoring the test preset failed");
	obs_frontend_save();
}

} // namespace countdown_dock_test
