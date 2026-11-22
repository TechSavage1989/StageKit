#pragma once

#include "replacement-frame.hpp"

#include <obs-module.h>
#include <QColor>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QPainter>
#include <QStringList>
#include <QThread>
#include <array>
#include <atomic>
#include <cmath>

namespace window_morph_test {

inline constexpr uint32_t kWidth = 384, kHeight = 200;

// The ordinary capture input changes its pixels and dimensions when the actual
// Window Switcher aims it. Ready captures use this same registered source kind.
// Only window discovery is supplied through the switcher's existing fixture API.
struct Capture {
	std::atomic<bool> blue{false};
};

inline void update_capture(Capture *capture, obs_data_t *settings)
{
	const std::string window = obs_data_get_string(settings, "window");
	capture->blue = window.find("blue-window.exe") != std::string::npos || obs_data_get_bool(settings, "resized");
}

inline void register_capture()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	obs_source_info info{};
	info.id = "stagekit_test_morph_window";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Window Morph capture fixture";
	};
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		auto *capture = new Capture();
		update_capture(capture, settings);
		return capture;
	};
	info.destroy = [](void *data) {
		delete static_cast<Capture *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		update_capture(static_cast<Capture *>(data), settings);
	};
	info.get_width = [](void *data) -> uint32_t {
		return static_cast<Capture *>(data)->blue ? 160 : 240;
	};
	info.get_height = [](void *data) -> uint32_t {
		return static_cast<Capture *>(data)->blue ? 180 : 120;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		const bool blue = static_cast<Capture *>(data)->blue;
		const uint32_t w = blue ? 160 : 240, h = blue ? 180 : 120;
		auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		auto *color = gs_effect_get_param_by_name(effect, "color");
		vec4 value;
		vec4_set(&value, blue ? 0.0f : 1.0f, 0.0f, blue ? 1.0f : 0.0f, 1.0f);
		gs_effect_set_vec4(color, &value);
		while (gs_effect_loop(effect, "Solid"))
			gs_draw_sprite(nullptr, 0, w, h);
		// A fixed landmark in the window also exposes a second, delayed crop
		// animation after the switcher has finished its own Morph.
		vec4_set(&value, 1.0f, 1.0f, 1.0f, 1.0f);
		gs_effect_set_vec4(color, &value);
		gs_matrix_push();
		gs_matrix_translate3f(float(w) * .75f, float(h) * .25f, 0.0f);
		while (gs_effect_loop(effect, "Solid"))
			gs_draw_sprite(nullptr, 0, w / 10, h / 10);
		gs_matrix_pop();
	};
	obs_register_source(&info);
}

inline void wait_frame()
{
	QCoreApplication::processEvents();
	QThread::msleep(20);
}

inline std::array<int, 4> pixel(const std::vector<uint8_t> &pixels, uint32_t x = 64, uint32_t y = 64)
{
	const size_t at = (size_t(y) * kWidth + x) * 4;
	if (at + 3 >= pixels.size())
		return {};
	return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
}

inline bool solid(const std::vector<uint8_t> &pixels, bool blue)
{
	const auto p = pixel(pixels);
	return p[blue ? 2 : 0] > 240 && p[blue ? 0 : 2] < 12 && p[1] < 12 && p[3] > 248;
}

inline bool landmark(const std::vector<uint8_t> &pixels, uint32_t w, uint32_t h)
{
	const auto p = pixel(pixels, uint32_t(w * .8), uint32_t(h * .3));
	return p[0] > 235 && p[1] > 235 && p[2] > 235 && p[3] > 248;
}

inline obs_source_t *make_capture(const char *name)
{
	auto *settings = obs_data_create();
	obs_data_set_string(settings, "window", "Red window::red-window.exe");
	obs_data_set_int(settings, "priority", 2);
	auto *capture = obs_source_create("stagekit_test_morph_window", name, settings, nullptr);
	obs_data_release(settings);
	return capture;
}

inline obs_source_t *add_frame(obs_source_t *capture, int glide_ms)
{
	auto *settings = obs_data_create();
	obs_data_set_int(settings, "frame_mode", 1);
	obs_data_set_int(settings, "frame_height", 180);
	obs_data_set_int(settings, "trim", 0);
	obs_data_set_int(settings, "shape", 0);
	obs_data_set_bool(settings, "known_programs", false);
	obs_data_set_int(settings, "glide_ms", glide_ms);
	auto *frame = obs_source_create_private("stagekit_auto_frame", "Window Morph Auto Frame", settings);
	obs_data_release(settings);
	if (frame)
		obs_source_filter_add(capture, frame);
	return frame;
}

inline bool windows(obs_source_t *switcher, bool blue)
{
	calldata_t data{};
	calldata_set_string(&data, "windows",
			    blue ? "Red window|red-window.exe||1\nBlue window|blue-window.exe|front|2"
				 : "Red window|red-window.exe|front|1\nBlue window|blue-window.exe||2");
	const bool called = proc_handler_call(obs_source_get_proc_handler(switcher), "set_test_windows", &data);
	calldata_free(&data);
	return called;
}

struct Snapshots {
	QString label;
	std::vector<uint8_t> before, middle, after;
};

inline void check(QStringList &issues, const QString &image_path = {})
{
	register_capture();
	std::vector<Snapshots> snapshots;
	// The direct renderer is a control. Both routes then exercise the real
	// downstream Auto Frame, including a glide longer than the upstream Morph.
	struct Trial {
		bool ready, framed;
		int glide;
	};
	for (const Trial trial : {Trial{true, false, 0}, Trial{true, true, 0}, Trial{false, true, 1400}}) {
		const QString label = QString("%1 / %2 / glide %3 ms")
					      .arg(trial.framed ? "Switcher + Auto Frame" : "Switcher only",
						   trial.ready ? "ready capture" : "parent re-aim")
					      .arg(trial.glide);
		auto *capture = make_capture("Window Morph capture");
		auto *settings = obs_data_create_from_json(
			R"({"apps":[{"value":"red-window.exe"},{"value":"blue-window.exe"}],"follow":true,"change":7,"change_ms":900,"ease_curve":0,"ease_mode":0,"transition":"none","game_area":false})");
		obs_data_set_bool(settings, "ready", trial.ready);
		auto *switcher = obs_source_create_private("stagekit_app_capture", "Window Morph switcher", settings);
		obs_data_release(settings);
		if (!capture || !switcher) {
			issues.append(label + ": could not create the real switcher fixture");
			obs_source_release(switcher);
			obs_source_release(capture);
			continue;
		}
		obs_source_filter_add(capture, switcher);
		auto *frame = trial.framed ? add_frame(capture, trial.glide) : nullptr;
		if (trial.framed && !frame)
			issues.append(label + ": could not create Auto Frame");
		obs_source_inc_showing(capture);
		if (!windows(switcher, false))
			issues.append(label + ": switcher window discovery fixture is missing");
		std::vector<uint8_t> current;
		QElapsedTimer warm;
		warm.start();
		do {
			wait_frame();
			current = replacement_test::render(capture, kWidth, kHeight);
		} while (warm.elapsed() < 650);
		if (!solid(current, false))
			issues.append(label + ": the original red window was not rendered");

		for (const bool blue : {true, false}) {
			Snapshots shot{label + (blue ? " / red to blue" : " / blue to red"), current, {}, {}};
			const uint32_t from_w = blue ? 240 : 160, to_w = blue ? 160 : 240;
			const uint32_t to_h = blue ? 180 : 120;
			const uint32_t output_w = trial.framed ? (blue ? 160 : 360) : to_w;
			const uint32_t output_h = trial.framed ? 180 : to_h;
			int intermediates = 0, blends = 0, early_count = 0, late_count = 0;
			int low_alpha = 0;
			double early = 0.0, late = 0.0, nearest_middle = 2.0;
			qint64 arrived = -1, rendered = -1;
			windows(switcher, blue);
			QElapsedTimer clock;
			clock.start();
			do {
				wait_frame();
				current = replacement_test::render(capture, kWidth, kHeight);
				const uint32_t w = obs_source_get_base_width(switcher);
				const uint32_t h = obs_source_get_base_height(switcher);
				const double progress = (double(w) - from_w) / (double(to_w) - from_w);
				const auto p = pixel(current);
				if (progress > .15 && progress < .85) {
					++intermediates;
					if (p[0] > 30 && p[0] < 240 && p[2] > 30 && p[2] < 240 && p[1] < 12)
						++blends;
					if (p[3] < 248)
						++low_alpha;
					const int incoming = p[blue ? 2 : 0];
					if (progress < .45) {
						early += incoming;
						++early_count;
					} else if (progress > .55) {
						late += incoming;
						++late_count;
					}
					if (std::abs(progress - .5) < nearest_middle) {
						nearest_middle = std::abs(progress - .5);
						shot.middle = current;
					}
				}
				if (w == to_w && h == to_h) {
					if (arrived < 0)
						arrived = clock.elapsed();
					if (solid(current, blue) && landmark(current, output_w, output_h) &&
					    obs_source_get_width(capture) == output_w &&
					    obs_source_get_height(capture) == output_h) {
						rendered = clock.elapsed();
						break;
					}
				}
			} while (clock.elapsed() < 3500);
			shot.after = current;
			snapshots.push_back(std::move(shot));
			const double early_level = early_count ? early / early_count : -1;
			const double late_level = late_count ? late / late_count : -1;
			const bool blended = intermediates >= 3 && blends >= 3 && early_count && late_count &&
					     late_level > early_level + 24 && low_alpha == 0;
			const bool finished = arrived >= 0 && rendered >= arrived && rendered - arrived < 250;
			blog(LOG_INFO,
			     "[window morph test] %s %s: %d intermediate sizes, %d blended frames, incoming %.1f to %.1f, alpha failures %d, completion lag %lld ms",
			     label.toUtf8().constData(), blue ? "red to blue" : "blue to red", intermediates, blends,
			     early_level, late_level, low_alpha, (long long)(rendered < 0 ? -1 : rendered - arrived));
			if (!blended || !finished)
				issues.append(
					QString("%1 %2: Morph had %3 intermediate sizes but %4 blended frames, incoming %5 to %6, alpha failures %7, completion lag %8 ms")
						.arg(label, blue ? "red to blue" : "blue to red")
						.arg(intermediates)
						.arg(blends)
						.arg(early_level)
						.arg(late_level)
						.arg(low_alpha)
						.arg(rendered < 0 ? -1 : rendered - arrived));
			// Rounded dimensions can reach their endpoint just before Mix
			// finishes. Draw Idle too, so its kept frame and size are ready
			// before testing a separate switch in the reverse direction.
			QElapsedTimer settled;
			settled.start();
			do {
				wait_frame();
				current = replacement_test::render(capture, kWidth, kHeight);
			} while (settled.elapsed() < 120);
		}
		obs_source_dec_showing(capture);
		if (frame) {
			obs_source_filter_remove(capture, frame);
			obs_source_release(frame);
		}
		obs_source_filter_remove(capture, switcher);
		obs_source_release(switcher);
		obs_source_release(capture);
	}

	// A real resolution change of the same captured window still uses the
	// short settling hold. It is not an intentional Window Switcher Morph.
	auto *capture = make_capture("Window resize hold capture");
	auto *frame = capture ? add_frame(capture, 0) : nullptr;
	if (!capture || !frame) {
		issues.append("Could not create the ordinary resize hold fixture");
	} else {
		obs_source_inc_showing(capture);
		QElapsedTimer clock;
		clock.start();
		std::vector<uint8_t> current;
		do {
			wait_frame();
			current = replacement_test::render(capture, kWidth, kHeight);
		} while (clock.elapsed() < 350);
		const bool before = solid(current, false);
		auto *settings = obs_source_get_settings(capture);
		obs_data_set_bool(settings, "resized", true);
		obs_source_update(capture, settings);
		obs_data_release(settings);
		clock.restart();
		qint64 first_new = -1;
		int held = 0;
		do {
			wait_frame();
			current = replacement_test::render(capture, kWidth, kHeight);
			if (obs_source_get_base_width(capture) == 160 && solid(current, false))
				++held;
			if (solid(current, true)) {
				first_new = clock.elapsed();
				break;
			}
		} while (clock.elapsed() < 1000);
		blog(LOG_INFO, "[window morph test] ordinary resize: %d held frames, new image after %lld ms", held,
		     (long long)first_new);
		if (!before || held < 2 || first_new < 100 || first_new > 900)
			issues.append(
				QString("Ordinary Auto Frame resize lost its settling hold: %1 held frames, new image after %2 ms")
					.arg(held)
					.arg(first_new));
		obs_source_dec_showing(capture);
		obs_source_filter_remove(capture, frame);
	}
	obs_source_release(frame);
	obs_source_release(capture);

	if (!image_path.isEmpty() && !snapshots.empty()) {
		QImage sheet(int(kWidth * 3), int(snapshots.size()) * (int(kHeight) + 28), QImage::Format_RGBA8888);
		sheet.fill(QColor(24, 24, 24));
		QPainter paint(&sheet);
		paint.setPen(Qt::white);
		for (size_t row = 0; row < snapshots.size(); ++row) {
			const auto &shot = snapshots[row];
			const int top = int(row) * (int(kHeight) + 28);
			paint.drawText(8, top + 19, shot.label + " - before / during / after");
			int column = 0;
			for (const auto *pixels : {&shot.before, &shot.middle, &shot.after}) {
				if (pixels->size() == size_t(kWidth) * kHeight * 4) {
					QImage image(pixels->data(), int(kWidth), int(kHeight), int(kWidth * 4),
						     QImage::Format_RGBA8888_Premultiplied);
					paint.drawImage(column * int(kWidth), top + 28, image);
				}
				++column;
			}
		}
		paint.end();
		if (!sheet.save(image_path))
			issues.append("Could not save the Window Morph render contact sheet");
	}
}

} // namespace window_morph_test
