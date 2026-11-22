#pragma once

#include "replacement-frame.hpp"

#include <obs-module.h>
#include <util/platform.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStringList>
#include <QThread>
#include <array>
#include <mutex>
#include <utility>

namespace capture_settings_test {

inline constexpr uint32_t kWidth = 160, kHeight = 100;
inline constexpr uint64_t kStartDelay = 350000000;

struct Capture {
	std::mutex mutex;
	int configuration = -1;
	uint64_t ready_at = 0;
};

inline void update_capture(Capture *capture, obs_data_t *settings)
{
	const std::string window = obs_data_get_string(settings, "window");
	const bool panel = window.find("panel-window.exe") != std::string::npos;
	const int configuration = (panel ? 1 : 0) | (obs_data_get_int(settings, "method") ? 2 : 0) |
				  (obs_data_get_bool(settings, "capture_cursor") ? 4 : 0);
	std::lock_guard lock(capture->mutex);
	if (capture->configuration != configuration) {
		capture->configuration = configuration;
		capture->ready_at = panel ? os_gettime_ns() + kStartDelay : 0;
	}
}

inline std::pair<int, uint64_t> capture_state(void *data)
{
	auto *capture = static_cast<Capture *>(data);
	std::lock_guard lock(capture->mutex);
	return {capture->configuration, capture->ready_at};
}

inline void register_capture()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	obs_source_info info{};
	info.id = "stagekit_test_settings_window";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Capture settings fixture";
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
		return os_gettime_ns() >= capture_state(data).second ? kWidth : 0;
	};
	info.get_height = [](void *data) -> uint32_t {
		return os_gettime_ns() >= capture_state(data).second ? kHeight : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		const auto [configuration, ready_at] = capture_state(data);
		if (os_gettime_ns() < ready_at)
			return;
		vec4 color;
		if (!(configuration & 1))
			vec4_set(&color, 1.0f, 0.0f, 0.0f, 1.0f); // The parent's unrelated automatic window.
		else if ((configuration & 6) == 4)
			vec4_set(&color, 0.0f, 0.0f, 1.0f, 1.0f); // Original method, cursor on.
		else if ((configuration & 6) == 6)
			vec4_set(&color, 0.0f, 1.0f, 0.0f, 1.0f); // New method, cursor on.
		else if ((configuration & 6) == 2)
			vec4_set(&color, 0.0f, 1.0f, 1.0f, 1.0f); // New method, cursor off.
		else
			vec4_set(&color, 1.0f, 0.0f, 1.0f, 1.0f); // Original method, cursor off.
		auto *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_vec4(gs_effect_get_param_by_name(effect, "color"), &color);
		while (gs_effect_loop(effect, "Solid"))
			gs_draw_sprite(nullptr, 0, kWidth, kHeight);
	};
	obs_register_source(&info);
}

enum class Image { Empty, Editor, Blue, Green, Cyan, Magenta, Mixed };

inline Image image_of(obs_source_t *source)
{
	const auto pixels = replacement_test::render(source, kWidth, kHeight);
	const size_t at = (size_t(50) * kWidth + 80) * 4;
	if (pixels.size() <= at + 3 || pixels[at + 3] < 240)
		return Image::Empty;
	const int r = pixels[at], g = pixels[at + 1], b = pixels[at + 2];
	if (r > 240 && g < 12 && b < 12)
		return Image::Editor;
	if (r < 12 && g < 12 && b > 240)
		return Image::Blue;
	if (r < 12 && g > 240 && b < 12)
		return Image::Green;
	if (r < 12 && g > 240 && b > 240)
		return Image::Cyan;
	if (r > 240 && g < 12 && b > 240)
		return Image::Magenta;
	return Image::Mixed;
}

inline void wait_frame()
{
	QCoreApplication::processEvents();
	QThread::msleep(20);
}

inline std::pair<long long, long long> pool()
{
	calldata_t data{};
	const bool called = proc_handler_call(obs_get_proc_handler(), "stagekit_test_ready_pool", &data);
	const auto value = std::make_pair(called ? calldata_int(&data, "captures") : -1,
					  called ? calldata_int(&data, "users") : -1);
	calldata_free(&data);
	return value;
}

inline bool command(obs_source_t *switcher, const char *what)
{
	calldata_t data{};
	calldata_set_string(&data, "what", what);
	const bool called = proc_handler_call(obs_source_get_proc_handler(switcher), "command", &data);
	calldata_free(&data);
	return called;
}

inline bool manual(obs_source_t *capture, obs_source_t *switcher)
{
	auto *request = obs_data_create();
	obs_data_set_string(request, "sourceName", obs_source_get_name(capture));
	obs_data_set_string(request, "filterName", obs_source_get_name(switcher));
	calldata_t data{};
	calldata_set_string(&data, "request", "GetSwitcherState");
	calldata_set_string(&data, "data", obs_data_get_json(request));
	const bool called = proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &data);
	const char *json = calldata_string(&data, "response");
	auto *response = obs_data_create_from_json(json ? json : "{}");
	const bool held = called && obs_data_get_bool(response, "success") && obs_data_get_bool(response, "manual");
	obs_data_release(response);
	calldata_free(&data);
	obs_data_release(request);
	return held;
}

inline void set_options(obs_source_t *capture, int method, bool cursor)
{
	auto *settings = obs_source_get_settings(capture);
	obs_data_set_int(settings, "method", method);
	obs_data_set_bool(settings, "capture_cursor", cursor);
	obs_source_update(capture, settings);
	obs_data_release(settings);
}

inline void check(QStringList &issues)
{
	register_capture();
	const auto before_pool = pool();
	std::array<obs_source_t *, 2> captures{}, switchers{};
	std::array<bool, 2> showing{};
	auto release = [&] {
		for (size_t i = 0; i < captures.size(); ++i) {
			if (showing[i])
				obs_source_dec_showing(captures[i]);
			if (captures[i] && switchers[i])
				obs_source_filter_remove(captures[i], switchers[i]);
			obs_source_release(switchers[i]);
			obs_source_release(captures[i]);
			captures[i] = switchers[i] = nullptr;
			showing[i] = false;
		}
	};
	for (size_t i = 0; i < captures.size(); ++i) {
		auto *settings = obs_data_create_from_json(
			R"({"window":"Editor window::editor-window.exe","priority":2,"method":0,"capture_cursor":true})");
		captures[i] = obs_source_create("stagekit_test_settings_window",
						i ? "Settings capture B" : "Settings capture A", settings, nullptr);
		obs_data_release(settings);
		settings = obs_data_create_from_json(
			R"({"apps":[{"value":"editor-window.exe"}],"by_hand":[{"value":"panel-window.exe"}],"follow":true,"hand_follow":false,"ready":true,"change":1,"change_ms":0,"transition":"none"})");
		switchers[i] = obs_source_create_private("stagekit_app_capture", "Settings switcher", settings);
		obs_data_release(settings);
		if (!captures[i] || !switchers[i]) {
			issues.append("Could not create the capture settings switcher fixture");
			release();
			return;
		}
		obs_source_filter_add(captures[i], switchers[i]);
		obs_source_inc_showing(captures[i]);
		showing[i] = true;
		calldata_t windows{};
		calldata_set_string(&windows, "windows",
				    "Editor window|editor-window.exe|front|101\nPanel window|panel-window.exe||102");
		if (!proc_handler_call(obs_source_get_proc_handler(switchers[i]), "set_test_windows", &windows))
			issues.append("The capture settings fixture could not supply window discovery");
		calldata_free(&windows);
	}

	auto draw = [&] {
		wait_frame();
		return std::array<Image, 2>{image_of(captures[0]), image_of(captures[1])};
	};
	auto check_pool = [&](long long captures_added, const QString &label) {
		const auto now = pool();
		if (now.first != before_pool.first + captures_added || now.second != before_pool.second + 2)
			issues.append(QString("%1: ready pool is %2 captures/%3 users, expected %4/%5")
					      .arg(label)
					      .arg(now.first)
					      .arg(now.second)
					      .arg(before_pool.first + captures_added)
					      .arg(before_pool.second + 2));
	};
	auto check_selection = [&](const QString &label) {
		for (size_t i = 0; i < captures.size(); ++i) {
			auto *settings = obs_source_get_settings(captures[i]);
			const bool aimed = std::string(obs_data_get_string(settings, "window")) ==
					   "Editor window::editor-window.exe";
			const bool untouched = !i || (obs_data_get_int(settings, "method") == 0 &&
						      obs_data_get_bool(settings, "capture_cursor"));
			obs_data_release(settings);
			if (!aimed || !untouched || !manual(captures[i], switchers[i]))
				issues.append(
					label +
					": capture aim, other parent options, or manual window selection changed");
		}
	};
	auto stable_for = [&](int ms, Image first, const QString &label) {
		QElapsedTimer clock;
		clock.start();
		bool correct = true;
		do {
			const auto shown = draw();
			correct = correct && shown[0] == first && shown[1] == Image::Blue;
		} while (clock.elapsed() < ms);
		if (!correct)
			issues.append(
				label +
				": a pending or canceled capture replaced the selected image, or altered the other switcher");
	};
	auto await_change = [&](Image previous, Image expected, const QString &label, bool cold) {
		QElapsedTimer clock;
		clock.start();
		qint64 first_new = -1;
		int held = 0;
		bool allowed = true, other_unchanged = true;
		do {
			const auto shown = draw();
			other_unchanged = other_unchanged && shown[1] == Image::Blue;
			allowed = allowed && (shown[0] == previous || shown[0] == expected);
			if (shown[0] == previous)
				++held;
			if (shown[0] == expected) {
				first_new = clock.elapsed();
				break;
			}
		} while (clock.elapsed() < 2500);
		blog(LOG_INFO,
		     "[capture settings test] %s: previous image %d frames, replacement after %lld ms, other unchanged %d",
		     label.toUtf8().constData(), held, (long long)first_new, other_unchanged);
		const bool ok = first_new >= 0 && allowed && other_unchanged &&
				(!cold || (held >= 2 && first_new >= 250));
		if (!ok)
			issues.append(
				QString("%1: held %2 frames, new image after %3 ms, unexpected image %4, other unchanged %5")
					.arg(label)
					.arg(held)
					.arg(first_new)
					.arg(!allowed)
					.arg(other_unchanged));
		return ok;
	};

	QElapsedTimer warm;
	warm.start();
	do {
		draw();
	} while (warm.elapsed() < 800);
	check_pool(1, "initial shared capture");
	for (auto *switcher : switchers)
		if (!command(switcher, "next"))
			issues.append("The switcher's existing manual command was unavailable");
	warm.restart();
	do {
		draw();
	} while (warm.elapsed() < 650);
	const auto selected = draw();
	if (selected[0] != Image::Blue || selected[1] != Image::Blue) {
		issues.append("Both switchers did not select the original prepared manual window");
	} else {
		check_selection("initial manual selection");
		set_options(captures[0], 1, true);
		if (await_change(Image::Blue, Image::Green, "method change", true)) {
			stable_for(120, Image::Green, "method change stays selected");
			check_pool(2, "independent settings capture");
			check_selection("method change");

			// Reverting while a replacement starts must discard that candidate.
			set_options(captures[0], 1, false);
			stable_for(80, Image::Green, "cursor replacement starting");
			set_options(captures[0], 1, true);
			stable_for(500, Image::Green, "reverted pending settings");
			check_pool(2, "reverted pending settings");

			// A newer choice supersedes the first candidate. Cyan must never
			// appear before the newer magenta capture is ready.
			set_options(captures[0], 1, false);
			stable_for(80, Image::Green, "superseded replacement starting");
			set_options(captures[0], 0, false);
			if (await_change(Image::Green, Image::Magenta, "newer method and cursor options", true)) {
				stable_for(120, Image::Magenta, "new options stay selected");
				check_pool(2, "superseded candidate released");
				check_selection("newer parent options");
				set_options(captures[0], 0, true);
				await_change(Image::Magenta, Image::Blue, "rejoin original shared capture", false);
				stable_for(550, Image::Blue, "rejoined capture stays selected");
				check_pool(1, "rejoined original settings");
				check_selection("rejoined original settings");
			}
		}
	}
	release();
	warm.restart();
	while (pool() != before_pool && warm.elapsed() < 1500)
		wait_frame();
	const auto after_pool = pool();
	if (after_pool != before_pool)
		issues.append(QString("Capture settings fixture leaked ready captures/users: before %1/%2, after %3/%4")
				      .arg(before_pool.first)
				      .arg(before_pool.second)
				      .arg(after_pool.first)
				      .arg(after_pool.second));
}

} // namespace capture_settings_test
