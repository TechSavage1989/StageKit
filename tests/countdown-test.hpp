#pragma once

#include "replacement-frame.hpp"
#include <obs-module.h>
#include <QElapsedTimer>
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
		const QRect bounds(i * 260 + (260 - size.width()) / 2, 9 + (90 - size.height()) / 2, size.width(), size.height());
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
	obs_enum_hotkeys([](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
		auto &b = *static_cast<Binding *>(data);
		if (obs_hotkey_get_registerer_type(key) == OBS_HOTKEY_REGISTERER_SOURCE &&
		    std::strcmp(b.name, obs_hotkey_get_name(key)) == 0 &&
		    obs_weak_source_references_source(static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(key)), b.source)) {
			b.id = id;
			return false;
		}
		return true;
	}, &binding);
	if (binding.id == OBS_INVALID_HOTKEY_ID)
		return false;
	obs_hotkey_trigger_routed_callback(binding.id, true);
	obs_hotkey_trigger_routed_callback(binding.id, false);
	return true;
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
	auto ended = [](void *data, calldata_t *) { ++*static_cast<std::atomic_int *>(data); };
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
	expect(press_key(source, "stagekit.countdown.pause") && wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_PAUSED; }),
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
	expect(press_key(source, "stagekit.countdown.start") && wait_for([&] { return obs_source_media_get_time(source) > 93050; }),
	       "resume lost the paused time");
	obs_source_media_set_time(source, 299840);
	expect(wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_ENDED; }), "the timer did not finish");
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
		expect(obs_source_media_get_state(other) == OBS_MEDIA_STATE_STOPPED, "a zero-length timer started running");
		obs_data_set_int(settings, "hours", 1);
		obs_data_set_int(settings, "color", 0xff00ff00);
		obs_data_set_bool(settings, "restart_when_active", true);
		obs_source_update(other, settings);
		expect(wait_for([&] { return obs_source_media_get_duration(other) == 3600000; }) && obs_source_get_width(other) > obs_source_get_width(source),
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
		expect(obs_source_media_get_state(other) == OBS_MEDIA_STATE_STOPPED, "Preview started the Program timer");
		obs_source_inc_active(other);
		expect(wait_for([&] { return obs_source_media_get_time(other) > 0; }), "going live did not restart the timer");
		obs_source_dec_active(other);
		obs_source_dec_showing(other);
		obs_source_release(other);
	}
	obs_data_release(settings);
	expect(button("stagekit.countdown.reset") && wait_for([&] { return obs_source_media_get_state(source) == OBS_MEDIA_STATE_STOPPED; }) &&
	       obs_source_media_get_time(source) == 0 && frame(source) == ready, "Reset did not restore the full preset");
	if (!sheet_path.isEmpty())
		expect(save_sheet(sheet_path, ready, paused, finished), "could not save the rendered guide image");
	obs_properties_destroy(props);
	signal_handler_disconnect(obs_source_get_signal_handler(source), "media_ended", ended, &ends);
	obs_source_dec_showing(source);
	obs_source_release(source);
}

} // namespace countdown_test
