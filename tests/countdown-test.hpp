#pragma once

#include "replacement-frame.hpp"
#include "../src/common/alpha-bounds.hpp"
#include "../src/countdown/stream-start.hpp"
#include "../src/countdown/countdown.hpp"
#include <QTimeZone>
#include <QFileInfo>
#include <QDir>
#include <obs-module.h>
#include <graphics/vec2.h>
#include <QDateTime>
#include <QElapsedTimer>
#include <obs.hpp>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QThread>
#include <atomic>

namespace countdown_test {

template<typename Predicate> bool wait_for(Predicate predicate)
{
	QElapsedTimer timeout;
	timeout.start();
	do {
		if (predicate())
			return true;
		QThread::msleep(15);
	} while (timeout.elapsed() < 2500);
	return predicate();
}

inline QImage frame(obs_source_t *source)
{
	const auto w = obs_source_get_width(source);
	const auto h = obs_source_get_height(source);
	if (!w || !h || w > 4096 || h > 2048)
		return {};
	auto pixels = replacement_test::render(source, w, h);
	return QImage(pixels.data(), int(w), int(h), int(w * 4), QImage::Format_RGBA8888_Premultiplied).copy();
}

inline bool visible_digits(const QImage &image)
{
	if (image.isNull() || image.pixelColor(0, 0).alpha() != 0)
		return false;
	int opaque = 0;
	for (int y = 0; y < image.height(); ++y)
		for (int x = 0; x < image.width(); ++x)
			if (image.pixelColor(x, y).alpha() > 220)
				++opaque;
	return opaque > 80;
}

inline bool save_sheet(const QString &path, const QImage &start, const QImage &paused, const QImage &ended)
{
	QImage sheet(780, 146, QImage::Format_RGB32);
	sheet.fill(QColor("#17202a"));
	QPainter painter(&sheet);
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	QFont font;
	font.setPixelSize(17);
	painter.setFont(font);
	const QImage *frames[] = {&start, &paused, &ended};
	const char *captions[] = {"Ready", "Paused", "Finished"};
	for (int i = 0; i < 3; ++i) {
		const auto &image = *frames[i];
		const QSize size = image.size().scaled(222, 90, Qt::KeepAspectRatio);
		const QRect bounds(i * 260 + (260 - size.width()) / 2, 9 + (90 - size.height()) / 2, size.width(),
				   size.height());
		painter.drawImage(bounds, image);
		painter.setPen(QColor("#b8d1dd"));
		painter.drawText(QRect(i * 260, 99, 260, 34), Qt::AlignCenter, QString::fromLatin1(captions[i]));
	}
	painter.end();
	return sheet.save(path);
}

inline bool press_key(obs_source_t *source, const char *name)
{
	struct Binding {
		obs_source_t *source;
		const char *name;
		obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	} binding{source, name};
	obs_enum_hotkeys(
		[](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
			auto &b = *static_cast<Binding *>(data);
			if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE &&
			    std::strcmp(b.name, obs_hotkey_get_name(key)) == 0 &&
			    obs_weak_source_references_source(
				    static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key)), b.source)) {
				b.id = id;
				return false;
			}
			return true;
		},
		&binding);
	if (binding.id == OBS_INVALID_HOTKEY_ID)
		return false;
	obs_hotkey_trigger_routed_callback(binding.id, true);
	obs_hotkey_trigger_routed_callback(binding.id, false);
	return true;
}

inline QString readout(obs_source_t *source)
{
	calldata_t call{};
	proc_handler_call(obs_source_get_proc_handler(source), "timer_readout", &call);
	const QString text = QString::fromUtf8(calldata_string(&call, "text"));
	calldata_free(&call);
	return text;
}

inline void clock_shortcuts(QStringList &issues)
{
	const QDate d(2026, 11, 21);
	const QTimeZone utc_zone(0);
	const QDateTime now(d, QTime(23, 38, 12, 777), utc_zone);
	const auto expect = [&](const QDateTime &clock, int mark, const QDateTime &expected) {
		if (stagekit::next_timer_mark(clock, mark) != expected)
			issues.append(QStringLiteral("Countdown: wrong next clock mark %1").arg(mark));
	};
	expect(now, -1, QDateTime(d, QTime(23, 45), utc_zone));
	expect(now, 45, QDateTime(d, QTime(23, 45), utc_zone));
	for (int mark : {0, 15, 30})
		expect(now, mark, QDateTime(d.addDays(1), QTime(0, mark), utc_zone));
	expect(QDateTime(d, QTime(12, 15), utc_zone), 15, QDateTime(d, QTime(13, 15), utc_zone));
	expect(now, 14, {});
	const QTimeZone zone("America/New_York");
	if (zone.isValid()) {
		// Spring jumps to 03:00 while the autumn change revisits 01:00.
		for (const auto &utc : {QDateTime(QDate(2026, 3, 8), QTime(6, 58), utc_zone),
					QDateTime(QDate(2026, 11, 1), QTime(5, 58), utc_zone)})
			expect(utc.toTimeZone(zone), -1, utc.addSecs(120).toTimeZone(zone));
	}
}

inline void modes(QStringList &issues)
{
	auto fail = [&](bool ok, const char *msg) {
		if (!ok)
			issues.append(QStringLiteral("Timer modes: ") + QString::fromLatin1(msg));
	};
	OBSDataAutoRelease config = obs_data_create();
	obs_data_set_int(config, "days", 1);
	obs_data_set_int(config, "hours", 2);
	obs_data_set_int(config, "minutes", 3);
	obs_data_set_int(config, "seconds", 4);
	obs_data_set_int(config, "display_units", 15);
	OBSSourceAutoRelease timer = obs_source_create("stagekit_countdown", "Timing modes fixture", config, nullptr);
	if (!timer) {
		fail(false, "source unavailable");
		return;
	}
	auto is = [&](const QString &s) {
		return wait_for([&] { return readout(timer) == s; });
	};
	fail(is("01:02:03:04"), "day/hour/minute/second format");
	obs_data_set_bool(config, "leading_zero", false);
	obs_data_set_bool(config, "format_output", true);
	obs_data_set_string(config, "output_format", "IN %time% (%days%d %hours2%h %minutes%m %seconds2%s)");
	obs_source_update(timer, config);
	fail(is("IN 1:02:03:04 (1d 02h 3m 04s)") && visible_digits(frame(timer)),
	     "leading field and custom output tokens");
	obs_data_set_bool(config, "leading_zero", true);
	obs_data_set_bool(config, "format_output", false);
	obs_source_update(timer, config);
	for (const auto &pair : {std::pair{12, "1563:04"}, {8, "93784"}, {1, "02"}, {5, "01:124"}}) {
		obs_data_set_int(config, "display_units", pair.first);
		obs_source_update(timer, config);
		fail(is(QString::fromLatin1(pair.second)), "hidden units were not folded into the selected units");
	}
	obs_data_set_int(config, "display_units", 12);
	obs_data_set_int(config, "timer_mode", 2);
	obs_source_update(timer, config);
	fail(is("00:00"), "count-up must start at zero");
	obs_source_media_play_pause(timer, false);
	obs_source_media_set_time(timer, 62000);
	fail(is("01:02") && obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PLAYING, "open-ended count-up");
	obs_source_media_play_pause(timer, true);
	fail(wait_for([&] { return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PAUSED; }), "count-up pause");
	obs_data_set_int(config, "days", 0);
	obs_data_set_int(config, "hours", 0);
	obs_data_set_int(config, "minutes", 0);
	obs_data_set_int(config, "seconds", 2);
	obs_data_set_bool(config, "limit_up", true);
	obs_data_set_string(config, "end_message", "Break over");
	obs_source_update(timer, config);
	fail(is("00:00"), "bounded count-up initial value");
	const auto before = frame(timer);
	obs_source_media_restart(timer);
	obs_source_media_set_time(timer, 1850);
	fail(is("Break over") && obs_source_media_get_time(timer) == 2000, "count-up finish message at its limit");
	frame(timer);
	QThread::msleep(280);
	const auto ending = frame(timer);
	fail(visible_digits(ending) && ending != before && ending.size() == before.size(),
	     "message did not render on the same canvas");
	obs_source_media_stop(timer);
	fail(is("00:00"), "reset did not clear the finish message");
	obs_data_set_int(config, "timer_mode", 1);
	const int64_t target = QDateTime::currentMSecsSinceEpoch() + 2100;
	obs_data_set_int(config, "deadline_ms", target);
	obs_source_update(timer, config);
	fail(wait_for([&] {
		     return obs_source_media_get_duration(timer) > 0 && obs_source_media_get_duration(timer) < 3000 &&
			    readout(timer) != "00:00";
	     }),
	     "date and time preset");
	obs_source_media_play_pause(timer, false);
	fail(wait_for([&] { return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PLAYING; }), "calendar start");
	obs_source_media_play_pause(timer, true);
	fail(wait_for([&] { return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PAUSED; }), "calendar pause");
	const QString held = readout(timer);
	QThread::msleep(260);
	fail(readout(timer) == held, "calendar pause did not freeze the readout");
	obs_source_media_play_pause(timer, false);
	fail(is("Break over") && QDateTime::currentMSecsSinceEpoch() >= target &&
		     QDateTime::currentMSecsSinceEpoch() - target < 1200,
	     "resume moved the actual deadline");
	obs_source_media_stop(timer);
	fail(wait_for([&] { return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_STOPPED; }),
	     "finished deadline reset");
	const int64_t paused_target = QDateTime::currentMSecsSinceEpoch() + 1200;
	obs_data_set_int(config, "deadline_ms", paused_target);
	obs_source_update(timer, config);
	fail(wait_for([&] { return obs_source_media_get_duration(timer) > 0; }), "second calendar preset");
	std::atomic_int ends{0};
	auto ended = [](void *data, calldata_t *) {
		++*static_cast<std::atomic_int *>(data);
	};
	signal_handler_connect(obs_source_get_signal_handler(timer), "media_ended", ended, &ends);
	obs_source_media_play_pause(timer, false);
	obs_source_media_play_pause(timer, true);
	fail(wait_for([&] { return QDateTime::currentMSecsSinceEpoch() >= paused_target; }), "paused target time");
	fail(ends == 0 && obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PAUSED,
	     "paused calendar completed early");
	obs_source_media_play_pause(timer, false);
	fail(wait_for([&] { return ends == 1 && readout(timer) == "Break over"; }),
	     "overdue resume missed finish actions");
	signal_handler_disconnect(obs_source_get_signal_handler(timer), "media_ended", ended, &ends);
	obs_data_set_int(config, "deadline_ms", QDateTime::currentMSecsSinceEpoch() + 1900);
	obs_data_set_bool(config, "count_up_to_date", true);
	obs_data_set_bool(config, "steady_seconds", true);
	obs_data_set_bool(config, "use_end_message", false);
	obs_source_update(timer, config);
	fail(is("00:00"), "count up to a date must start at zero");
	obs_source_media_play_pause(timer, false);
	fail(is("00:01") && wait_for([&] { return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_ENDED; }) &&
		     readout(timer) != "Break over",
	     "calendar count-up or disabled legacy end message");
	obs_data_set_int(config, "timer_mode", 2);
	obs_data_set_bool(config, "limit_up", false);
	obs_data_set_bool(config, "start_on_stream", true);
	obs_source_update(timer, config);
	OBSDataAutoRelease private_settings = obs_data_create();
	obs_data_set_bool(private_settings, "start_on_stream", true);
	OBSSourceAutoRelease hidden = obs_source_create_private("stagekit_countdown", nullptr, private_settings);
	OBSSourceAutoRelease manual = obs_source_create("stagekit_countdown", "Manual timer fixture", nullptr, nullptr);
	fail(is("00:00"), "steady preset");
	stagekit::start_stream_timers(hidden);
	fail(wait_for([&] {
		     return obs_source_media_get_state(timer) == OBS_MEDIA_STATE_PLAYING &&
			    obs_source_media_get_state(hidden) == OBS_MEDIA_STATE_PLAYING &&
			    obs_source_media_get_state(manual) == OBS_MEDIA_STATE_STOPPED;
	     }),
	     "stream-start opt-in for scene and private timers");
	obs_source_media_set_time(timer, 5000);
	fail(wait_for([&] { return obs_source_media_get_time(timer) >= 5000; }), "steady timer seek");
	stagekit::start_stream_timers(hidden);
	fail(obs_source_media_get_time(timer) >= 5000, "a repeated stream-start reset a running timer");
	const auto start_ms = obs_source_media_get_time(timer);
	obs_queue_task(OBS_TASK_GRAPHICS, [](void *) { QThread::msleep(2200); }, nullptr, true);
	fail(wait_for([&] { return obs_source_media_get_time(timer) >= start_ms + 950; }) &&
		     obs_source_media_get_time(timer) < start_ms + 1600,
	     "a late render skipped multiple steady seconds");
	obs_source_media_stop(timer);
}

inline QRect painted(const QImage &image)
{
	const auto b = stagekit::visible_alpha_bounds(image.constBits(), size_t(image.bytesPerLine()),
						      uint32_t(image.width()), uint32_t(image.height()));
	return b.left < uint32_t(image.width())
		       ? QRect(QPoint(int(b.left), int(b.top)), QPoint(int(b.right), int(b.bottom)))
		       : QRect();
}

// Compare the unchanged parts of real OBS frames, including the tens slot disappearing at 10:00.
inline void steady_ticks(QStringList &issues, const QString &sheet_path)
{
	OBSDataAutoRelease config = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();
	obs_data_set_string(font, "face", "Open Sans");
	obs_data_set_int(font, "size", 300);
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_obj(config, "font", font);
	obs_data_set_int(config, "minutes", 10);
	obs_data_set_int(config, "seconds", 11);
	obs_data_set_bool(config, "leading_zero", false);
	obs_data_set_bool(config, "format_output", true);
	obs_data_set_string(config, "output_format", "IN %time%");
	obs_data_set_string(config, "end_message", "MOMENTARILY");
	OBSSourceAutoRelease timer = obs_source_create_private("stagekit_countdown", nullptr, config);
	auto expect = [&](bool ok, const char *why) {
		if (!ok)
			issues.append(QStringLiteral("Countdown placement: ") + QString::fromLatin1(why));
	};
	if (!timer) {
		expect(false, "source missing");
		return;
	}
	auto shot = [&](int64_t position, const QString &expected) {
		obs_source_media_set_time(timer, position);
		expect(wait_for([&] { return readout(timer) == expected; }), "wrong test time");
		return frame(timer);
	};
	auto same = [](const QImage &a, const QImage &b, int from, int to) {
		if (a.isNull() || a.size() != b.size())
			return false;
		const auto box = QRect(a.width() * from / 100, 0, a.width() * (to - from) / 100, a.height());
		const auto part = a.copy(box);
		return !painted(part).isEmpty() && part == b.copy(box);
	};
	const QImage ten = shot(0, "IN 10:11");
	const QImage nine = shot(60000, "IN 9:11");
	expect(ten != nine && same(ten, nine, 0, 21) && same(ten, nine, 62, 100),
	       "prefix, colon or seconds moved when the leading digit disappeared");
	for (int n = 0; n < 10; ++n) {
		const auto change = shot(61000 + n * 1000, QStringLiteral("IN 9:%1").arg(10 - n, 2, 10, QLatin1Char('0')));
		expect(same(nine, change, 0, 65), "changing seconds moved the prefix, minutes or colon");
	}
	const auto before = shot(11000, "IN 10:00");
	obs_source_media_play_pause(timer, false);
	expect(wait_for([&] { return readout(timer) == "IN 9:59"; }), "live minute rollover");
	obs_source_media_play_pause(timer, true);
	const auto after = frame(timer);
	expect(before != after && same(before, after, 0, 21), "the formatted prefix moved during playback");
	obs_source_media_set_time(timer, 610999);
	obs_source_media_play_pause(timer, false);
	expect(wait_for([&] { return readout(timer) == "MOMENTARILY"; }), "end message");
	frame(timer);
	QThread::msleep(270);
	const auto ended = frame(timer);
	expect(ended.size() == ten.size() && !painted(ended).isEmpty(), "end message changed the source bounds");
	obs_source_media_stop(timer);
	frame(timer);
	QThread::msleep(270);
	expect(frame(timer) == ten, "reset did not restore the same running pixels");

	if (!sheet_path.isEmpty()) {
		QImage sheet(840, 138, QImage::Format_RGB32);
		sheet.fill(QColor("#202329"));
		QPainter paint(&sheet);
		paint.setRenderHint(QPainter::SmoothPixmapTransform);
		const QImage samples[]{before, after, ended};
		const QString labels[]{QStringLiteral("10:00 remaining"), QStringLiteral("9:59 remaining"),
				       QStringLiteral("End message")};
		const QSize size = before.size().scaled(252, 88, Qt::KeepAspectRatio);
		for (int i = 0; i < 3; ++i) {
			const QRect box(QPoint(i * 280 + (280 - size.width()) / 2, (100 - size.height()) / 2), size);
			paint.setPen(QColor("#72cbd7"));
			paint.drawRect(box.adjusted(-1, -1, 0, 0));
			paint.drawImage(box, samples[i]);
			paint.setPen(Qt::white);
			paint.drawText(QRect(280 * i, 98, 280, 28), Qt::AlignCenter, labels[i]);
		}
		paint.end();
		expect(sheet.save(QFileInfo(sheet_path).dir().filePath("countdown-rollover.png")), "saving the rollover image");
	}

	obs_data_set_string(config, "output_format", "%minutes%m %seconds2%s");
	obs_source_update(timer, config);
	const auto tokens = shot(0, "10m 11s");
	const auto one = shot(60000, "9m 11s");
	expect(same(tokens, one, 54, 100), "custom unit placeholders changed the suffix placement");
	obs_data_set_bool(config, "format_output", false);
	obs_source_update(timer, config);
	const auto bare = shot(0, "10:11");
	const auto less = shot(60000, "9:11");
	expect(same(bare, less, 50, 100), "bare clock refitted when the leading zero was hidden");

	obs_data_set_int(config, "timer_mode", 2);
	obs_data_set_int(font, "size", 88);
	obs_data_set_obj(config, "font", font);
	obs_source_update(timer, config);
	expect(wait_for([&] { return readout(timer) == "0:00"; }), "open-ended timer setup");
	const auto minutes = shot(3551000, "59:11");
	const auto hours = shot(3611000, "1:00:11");
	const auto days = shot(86411000, "1:00:00:11");
	expect(same(minutes, hours, 84, 100) && same(hours, days, 84, 100),
	       "adding hours or days to a count-up moved its seconds");
	blog(LOG_INFO, "StageKit timer layout: checked fixed clock anchors across ten digits, minute, hour and day rollovers");
}

inline void fitted_states(QStringList &issues, const QString &sheet_path)
{
	auto require = [&](bool ok, const char *why) {
		if (!ok)
			issues.append(QStringLiteral("Countdown: ") + QString::fromLatin1(why));
	};
	OBSDataAutoRelease config = obs_data_create();
	OBSSourceAutoRelease timer = obs_source_create_private("stagekit_countdown", nullptr, config);
	OBSSourceAutoRelease neighbor = obs_source_create_private("stagekit_countdown", nullptr, nullptr);
	OBSSceneAutoRelease scene = obs_scene_create_private("Adjacent timer layout");
	if (!timer || !neighbor || !scene) {
		require(false, "could not create timer layout");
		return;
	}
	const QImage clock = frame(timer);
	auto *guard = obs_scene_add(scene, neighbor);
	auto *item = obs_scene_add(scene, timer);
	if (clock.isNull() || !guard || !item) {
		require(false, "could not place adjacent sources");
		return;
	}
	const int w = clock.width(), h = clock.height();
	vec2 position;
	vec2_set(&position, float(w), 8.0f);
	obs_sceneitem_set_pos(item, &position);
	obs_sceneitem_force_update_transform(item);
	const QRect target(w, 8, w, h);
	// Include eight pixels beyond the intended bottom and right canvas edges as overscan.
	auto scene_frame = [&] {
		const int sw = 2 * w + 8, sh = h + 16;
		auto pixels = replacement_test::render(obs_scene_get_source(scene), sw, sh);
		return QImage(pixels.data(), sw, sh, sw * 4, QImage::Format_RGBA8888_Premultiplied).copy();
	};
	const QImage reference = scene_frame();
	const QImage placed_clock = reference.copy(target);
	const QRect clock_ink = painted(clock);
	const QRect scene_ink = painted(placed_clock);
	require(clock_ink.width() >= w * 0.90 && clock_ink.height() >= h * 0.90, "digits did not fill the transform");
	require(std::abs(scene_ink.width() - clock_ink.width()) <= 3 &&
			std::abs(scene_ink.height() - clock_ink.height()) <= 3 &&
			!painted(reference.copy(0, 0, w, h)).isEmpty(),
		"edge fixture placement");
	auto inside = [&](const QImage &capture) {
		QImage outside = capture;
		QPainter cover(&outside);
		cover.setCompositionMode(QPainter::CompositionMode_Source);
		cover.fillRect(target, Qt::transparent);
		cover.end();
		QImage expected = reference;
		QPainter clean(&expected);
		clean.setCompositionMode(QPainter::CompositionMode_Source);
		clean.fillRect(target, Qt::transparent);
		clean.end();
		return outside == expected && int(obs_source_get_width(timer)) == w &&
		       int(obs_source_get_height(timer)) == h;
	};
	auto change = [&](const QString &text, bool animate) {
		require(wait_for([&] { return readout(timer) == text; }), "timer layout readout");
		const QImage first = scene_frame();
		QThread::msleep(85);
		const QImage middle = scene_frame();
		QThread::msleep(195);
		const QImage last = scene_frame();
		require(inside(first) && inside(middle) && inside(last),
			"text crossed the transform, screen edge or neighboring source");
		if (animate)
			require(first != middle && middle != last && first != last, "end message did not fade through an intermediate frame");
		else
			require(first == middle && middle == last, "running text moved during a format change");
		const auto img = last.copy(target);
		const QRect ink = painted(img);
		require(!ink.isEmpty() && (ink.width() >= w * 0.88 || ink.height() >= h * 0.88),
			"fitted text left its box empty");
		return img;
	};
	obs_data_set_string(config, "end_message", "MOMENTARILY");
	obs_data_set_bool(config, "use_end_message", true);
	obs_data_set_string(config, "output_format", "IN %time%");
	obs_source_update(timer, config);
	require(change("05:00", false) == placed_clock, "a hidden message or format changed the running digits");
	obs_data_set_bool(config, "format_output", true);
	obs_source_update(timer, config);
	const auto formatted = change("IN 05:00", false);
	obs_source_media_restart(timer);
	obs_source_media_set_time(timer, 299990);
	const auto message = change("MOMENTARILY", true);
	obs_source_media_stop(timer);
	require(change("IN 05:00", true) == formatted, "reset did not restore formatted output in place");
	obs_data_set_bool(config, "format_output", false);
	obs_source_update(timer, config);
	require(change("05:00", false) == placed_clock, "returning from formatted text moved the digits");
	obs_data_set_bool(config, "format_output", true);
	obs_data_set_string(config, "output_format", "WE WILL BE RIGHT BACK IN %time%");
	obs_source_update(timer, config);
	change("WE WILL BE RIGHT BACK IN 05:00", false);
	blog(LOG_INFO, "StageKit timer layout: %dx%d source, clock ink %dx%d, formatted ink %dx%d, message ink %dx%d",
	     w, h, clock_ink.width(), clock_ink.height(), painted(formatted).width(), painted(formatted).height(),
	     painted(message).width(), painted(message).height());
	if (!sheet_path.isEmpty()) {
		QImage sheet(3 * (w + 20), h + 50, QImage::Format_RGB32);
		sheet.fill(QColor("#202329"));
		QPainter painter(&sheet);
		const QImage samples[]{placed_clock, formatted, message};
		const QString labels[]{QStringLiteral("Timer"), QStringLiteral("Formatted output"),
				       QStringLiteral("End message")};
		for (int i = 0; i < 3; ++i) {
			const QRect box(10 + i * (w + 20), 8, w, h);
			painter.setPen(QColor("#72cbd7"));
			painter.drawRect(box.adjusted(-1, -1, 0, 0));
			painter.drawImage(box.topLeft(), samples[i]);
			painter.setPen(Qt::white);
			painter.drawText(QRect(box.left(), h + 12, w, 30), Qt::AlignCenter, labels[i]);
		}
		painter.end();
		require(sheet.save(QFileInfo(sheet_path).dir().filePath("countdown-layout.png")),
			"could not save timer layout preview");
	}
}

// Match an OBS scene item enlarged fivefold, and compare its alpha edges
// with the old operation of enlarging a one-to-one timer bitmap.
inline bool sharp_scaled_timer(const QString &sheet_path, int font_size, int zoom)
{
	OBSDataAutoRelease config = obs_data_create();
	OBSDataAutoRelease font = obs_data_create();
	obs_data_set_string(font, "face", "Open Sans");
	obs_data_set_int(font, "size", font_size);
	obs_data_set_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_obj(config, "font", font);
	OBSSourceAutoRelease source = obs_source_create_private("stagekit_countdown", nullptr, config);
	OBSSceneAutoRelease scene = obs_scene_create_private("Timer at five times size");
	if (!source || !scene)
		return false;
	const QImage original = frame(source);
	if (original.isNull())
		return false;
	auto *item = obs_scene_add(scene, source);
	if (!item)
		return false;
	vec2 scale;
	vec2_set(&scale, float(zoom), float(zoom));
	obs_sceneitem_set_scale(item, &scale);
	obs_sceneitem_force_update_transform(item);
	const int w = original.width() * zoom, h = original.height() * zoom;
	auto pixels = replacement_test::render(obs_scene_get_source(scene), w, h);
	const QImage sharp(pixels.data(), w, h, w * 4, QImage::Format_RGBA8888_Premultiplied);
	const QImage enlarged = original.scaled(w, h, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	auto coverage = [](const QImage &image) {
		struct Coverage {
			int solid = 0, soft = 0;
			int64_t alpha = 0;
		} totals;
		for (int y = 0; y < image.height(); ++y) {
			const auto *line = image.constScanLine(y);
			for (int x = 0; x < image.width(); ++x) {
				const int a = line[4 * x + 3];
				totals.solid += a >= 224;
				totals.alpha += a;
				totals.soft += a >= 32 && a < 224;
			}
		}
		return totals;
	};
	const auto old = coverage(enlarged), current = coverage(sharp);
	blog(LOG_INFO, "StageKit timer at %dx: solid pixels %d/%d, soft edge pixels %d/%d (bitmap/native)", zoom,
	     old.solid, current.solid, old.soft, current.soft);
	if (!sheet_path.isEmpty()) {
		const int cell = 270;
		const QRect crop(w / 2 - cell / 2, h / 2 - cell / 2, cell, cell);
		QImage sheet(cell * 2, cell + 36, QImage::Format_RGB32);
		sheet.fill(QColor("#202329"));
		QPainter painter(&sheet);
		painter.drawImage(QPoint(0, 0), enlarged, crop);
		painter.drawImage(QPoint(cell, 0), sharp, crop);
		painter.setPen(Qt::white);
		painter.drawText(QRect(0, cell, cell, 32), Qt::AlignCenter, QStringLiteral("Enlarged bitmap"));
		painter.drawText(QRect(cell, cell, cell, 32), Qt::AlignCenter,
				 QStringLiteral("Rendered at %1x").arg(zoom));
		painter.end();
		if (!sheet.save(QFileInfo(sheet_path).dir().filePath("countdown-scaled.png")))
			return false;
	}
	return current.alpha > old.alpha * 0.88 && current.alpha < old.alpha * 1.12 && old.soft > 100 &&
	       current.soft < old.soft * 0.68 && int(obs_source_get_width(source)) == original.width() &&
	       int(obs_source_get_height(source)) == original.height();
}

inline void check(QStringList &issues, const QString &sheet_path = {})
{
	auto expect = [&](bool ok, const char *message) {
		if (!ok)
			issues.append(QStringLiteral("Countdown: ") + QString::fromLatin1(message));
	};
	obs_source_t *source = obs_source_create("stagekit_countdown", "StageKit test countdown", nullptr, nullptr);
	if (!source || obs_source_media_get_duration(source) != 300000) {
		expect(false, "the source did not open with a five-minute preset");
		obs_source_release(source);
		return;
	}
	obs_source_inc_showing(source);
	std::atomic_int ends{0};
	auto ended = [](void *data, calldata_t *) {
		++*static_cast<std::atomic_int *>(data);
	};
	signal_handler_connect(obs_source_get_signal_handler(source), "media_ended", ended, &ends);
	obs_properties_t *props = obs_source_properties(source);
	auto button = [&](const char *name) {
		auto *property = obs_properties_get(props, name);
		if (property)
			obs_property_button_clicked(property, source);
		return property != nullptr;
	};
	const QImage ready = frame(source);
	expect(visible_digits(ready), "the ready frame must contain digits on a transparent background");
	expect(button("stagekit.countdown.start") && wait_for([&] { return obs_source_media_get_time(source) >= 75; }),
	       "the Start button did not start the timer");
	expect(press_key(source, "stagekit.countdown.pause") &&
		       wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED; }),
	       "the per-source Pause hotkey did not pause the timer");
	const auto held = obs_source_media_get_time(source);
	QThread::msleep(100);
	expect(held == obs_source_media_get_time(source), "a paused timer kept counting");
	obs_source_media_set_time(source, 93000);
	expect(wait_for([&] { return obs_source_media_get_time(source) == 93000; }), "seeking while paused failed");
	const QImage paused = frame(source);
	auto *settings = obs_data_create();
	obs_data_set_int(settings, "minutes", 3);
	obs_data_set_int(settings, "seconds", 27);
	obs_source_t *other = obs_source_create("stagekit_countdown", "Independent countdown", settings, nullptr);
	expect(other && frame(other) == paused && paused != ready && paused.size() == ready.size(),
	       "the frame at 03:27 did not match an independent 03:27 timer at the same size");
	expect(press_key(source, "stagekit.countdown.start") &&
		       wait_for([&] { return obs_source_media_get_time(source) > 93050; }),
	       "resume lost the paused time");
	obs_source_media_set_time(source, 299840);
	expect(wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_ENDED; }),
	       "the timer did not finish");
	const QImage finished = frame(source);
	QThread::msleep(80);
	expect(ends == 1 && obs_source_media_get_time(source) == 300000 && finished == frame(source),
	       "the timer did not stop at zero with one end event");
	if (other) {
		obs_data_set_int(settings, "minutes", 0);
		obs_data_set_int(settings, "seconds", 0);
		obs_source_update(other, settings);
		expect(wait_for([&] { return obs_source_media_get_duration(other) == 0; }) && frame(other) == finished,
		       "the last frame did not read 00:00");
		obs_source_media_play_pause(other, false);
		QThread::msleep(60);
		expect(obs_source_media_get_state(other) == OBS_MEDIA_STATE_STOPPED,
		       "a zero-length timer started running");
		obs_data_set_int(settings, "hours", 1);
		obs_data_set_int(settings, "color", 0xff00ff00);
		obs_data_set_bool(settings, "restart_when_active", true);
		obs_source_update(other, settings);
		expect(wait_for([&] { return obs_source_media_get_duration(other) == 3600000; }) &&
			       obs_source_get_width(other) > obs_source_get_width(source),
		       "the hour field did not fit on the source");
		const auto green = frame(other);
		bool colored = false;
		for (int y = 0; y < green.height() && !colored; ++y)
			for (int x = 0; x < green.width() && !colored; ++x) {
				const QColor p = green.pixelColor(x, y);
				colored = p.green() > 220 && p.red() < 20 && p.alpha() > 220;
			}
		expect(colored, "the saved text colour did not reach the rendered source");
		obs_source_inc_showing(other);
		QThread::msleep(100);
		expect(obs_source_media_get_state(other) == OBS_MEDIA_STATE_STOPPED,
		       "Preview started the Program timer");
		obs_source_inc_active(other);
		expect(wait_for([&] { return obs_source_media_get_time(other) > 0; }),
		       "going live did not restart the timer");
		obs_source_dec_active(other);
		obs_source_dec_showing(other);
		obs_source_release(other);
	}
	obs_data_release(settings);
	expect(button("stagekit.countdown.reset") &&
		       wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED; }) &&
		       obs_source_media_get_time(source) == 0 && frame(source) == ready,
	       "Reset did not restore the full preset");
	if (!sheet_path.isEmpty())
		expect(save_sheet(sheet_path, ready, paused, finished), "could not save the rendered guide image");
	obs_properties_destroy(props);
	signal_handler_disconnect(obs_source_get_signal_handler(source), "media_ended", ended, &ends);
	obs_source_dec_showing(source);
	obs_source_release(source);
	clock_shortcuts(issues);
	modes(issues);
	fitted_states(issues, sheet_path);
	steady_ticks(issues, sheet_path);
	expect(sharp_scaled_timer(sheet_path, 100, 5),
	       "the fivefold timer lost edge detail or changed its logical dimensions");
	expect(sharp_scaled_timer({}, 24, 16), "a small timer lost edge detail when enlarged sixteenfold");
}

} // namespace countdown_test
