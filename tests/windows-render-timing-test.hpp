#pragma once

#include "shadow-reuse-test.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/profiler.h>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace windows_render_timing_test {

inline void pump()
{
	QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
	QThread::msleep(5);
}

inline uint64_t completed_frames()
{
	const uint64_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
	return total - std::min(total, lagged);
}

struct Profile {
	const char *name;
	std::map<uint64_t, uint64_t> times;
	bool found = false;
};

inline bool find_profile(void *data, profiler_snapshot_entry_t *entry)
{
	auto &profile = *static_cast<Profile *>(data);
	const char *name = profiler_snapshot_entry_name(entry);
	const bool match =
		name && (strcmp(name, profile.name) == 0 || (strcmp(profile.name, "obs_graphics_thread") == 0 &&
							     strncmp(name, profile.name, strlen(profile.name)) == 0));
	if (match) {
		profile.found = true;
		auto *times = profiler_snapshot_entry_times(entry);
		for (size_t i = 0; times && i < times->num; ++i)
			profile.times[times->array[i].time_delta] += times->array[i].count;
	} else {
		profiler_snapshot_enumerate_children(entry, find_profile, data);
	}
	return !profile.found;
}

inline QJsonObject profile_difference(profiler_snapshot_t *before, profiler_snapshot_t *after, const char *name)
{
	Profile a{name, {}, false}, b{name, {}, false};
	profiler_snapshot_enumerate_roots(before, find_profile, &a);
	profiler_snapshot_enumerate_roots(after, find_profile, &b);
	uint64_t count = 0;
	double total_us = 0;
	for (auto &[us, calls] : b.times) {
		calls -= std::min(calls, a.times[us]);
		count += calls;
		total_us += double(us) * double(calls);
	}
	auto percentile = [&](double fraction) {
		const uint64_t target = uint64_t(std::ceil(double(count) * fraction));
		uint64_t seen = 0;
		for (const auto &[us, calls] : b.times) {
			seen += calls;
			if (seen >= target && target)
				return double(us) / 1000.0;
		}
		return 0.0;
	};
	return {{"frames", double(count)},
		{"mean_ms", count ? total_us / double(count) / 1000.0 : 0.0},
		{"p50_ms", percentile(0.5)},
		{"p95_ms", percentile(0.95)}};
}

inline obs_source_t *picture(const char *name, uint32_t width, uint32_t height, bool background)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_bool(settings, "background", background);
	return obs_source_create_private("stagekit_test_shadow_picture", name, settings);
}

inline void check(QStringList &issues)
{
	const QString path = qEnvironmentVariable("STAGEKIT_WINDOWS_TIMING");
	if (path.isEmpty())
		return;
	const qsizetype first_issue = issues.size();
	auto expect = [&](bool condition, const char *message) {
		if (!condition)
			issues << QStringLiteral("Windows render timing: ") + QString::fromUtf8(message);
	};
	QJsonObject result{
		{"complete", false},
		{"comparison", "same candidate binary; bypass, reuse, reuse, bypass"},
		{"obs_version", QString::fromUtf8(obs_get_version_string())},
		{"warmup_seconds", 6},
		{"requested_frames", 450},
		{"fixture_width", 320},
		{"fixture_height", 180},
		{"stats_note", "OBS Stats reports roughly one-second graphics-thread wall-time averages, "
			       "excluding frame pacing sleep. Samples below are those averages, not GPU timers."},
		{"scope_note", "This isolates Scene Shadow preparation reuse in one synthetic scene. "
			       "It does not compare releases or establish the owner's hardware performance."}};
	auto save = [&] {
		QSaveFile file(path);
		const QByteArray json = QJsonDocument(result).toJson();
		expect(file.open(QIODevice::WriteOnly) && file.write(json) == json.size() && file.commit(),
		       "could not save the diagnostic JSON");
	};
	obs_video_info video{};
	expect(obs_get_video_info(&video), "OBS video information is unavailable");
	result["video"] = QJsonObject{{"base_width", int(video.base_width)},
				      {"base_height", int(video.base_height)},
				      {"output_width", int(video.output_width)},
				      {"output_height", int(video.output_height)},
				      {"fps_num", int(video.fps_num)},
				      {"fps_den", int(video.fps_den)},
				      {"format", int(video.output_format)},
				      {"colorspace", int(video.colorspace)},
				      {"range", int(video.range)}};
	obs_enter_graphics();
	const int device = gs_get_device_type();
	result["backend"] = QString::fromUtf8(gs_get_device_name());
	result["renderer"] = QString::fromUtf8(gs_get_renderer());
	result["driver"] = QString::fromUtf8(gs_get_driver_version());
	obs_leave_graphics();
	expect(device == GS_DEVICE_DIRECT3D_11, "the Windows diagnostic did not use Direct3D 11");
	save();

	shadow_reuse_test::register_picture();
	OBSSourceAutoRelease background = picture("Windows timing background", 320, 180, true);
	OBSSourceAutoRelease panel = picture("Windows timing panel", 120, 80, false);
	OBSDataAutoRelease scene_settings = obs_data_create();
	obs_data_set_bool(scene_settings, "custom_size", true);
	obs_data_set_int(scene_settings, "cx", 320);
	obs_data_set_int(scene_settings, "cy", 180);
	OBSSourceAutoRelease scene = obs_source_create("scene", "Windows render timing", scene_settings, nullptr);
	OBSDataAutoRelease shadow_settings = obs_data_create();
	obs_data_set_int(shadow_settings, "shadow_from", 0);
	obs_data_set_bool(shadow_settings, "shadow", true);
	obs_data_set_double(shadow_settings, "softness", 20.0);
	obs_data_set_double(shadow_settings, "shadow_opacity", 60.0);
	obs_data_set_bool(shadow_settings, "outline", true);
	obs_data_set_int(shadow_settings, "outline_design", 5);
	obs_data_set_double(shadow_settings, "outline_width", 6.0);
	obs_data_set_int(shadow_settings, "ramp_ms", 0);
	obs_data_set_int(shadow_settings, "ramp_out_ms", 0);
	OBSSourceAutoRelease shadow =
		obs_source_create_private("stagekit_scene_shadow", "Windows timing shadow", shadow_settings);
	OBSDataAutoRelease blur_settings = obs_data_create();
	obs_data_set_int(blur_settings, "blur_area", 1);
	obs_data_set_int(blur_settings, "blur_type", 0);
	obs_data_set_double(blur_settings, "strength", 12.0);
	obs_data_set_int(blur_settings, "ramp_in_ms", 0);
	obs_data_set_int(blur_settings, "ramp_out_ms", 0);
	OBSSourceAutoRelease blur =
		obs_source_create_private("stagekit_scene_blur", "Windows timing blur", blur_settings);
	expect(background && panel && scene && shadow && blur, "a fixture source or filter is unavailable");
	QJsonArray trials;
	if (issues.size() == first_issue) {
		obs_source_load(scene);
		expect(obs_source_get_width(scene) == 320 && obs_source_get_height(scene) == 180,
		       "the fixture scene did not retain its fixed canvas");
		auto *items = obs_scene_from_source(scene);
		expect(obs_scene_add(items, background) != nullptr, "the background scene item is unavailable");
		for (const float x : {62.25f, 126.5f}) {
			auto *item = obs_scene_add(items, panel);
			expect(item != nullptr, "a foreground scene item is unavailable");
			if (!item)
				continue;
			vec2 position;
			vec2_set(&position, x, x == 62.25f ? 40.5f : 72.25f);
			obs_sceneitem_set_pos(item, &position);
		}
		obs_source_filter_add(scene, shadow);
		obs_source_filter_add(scene, blur);
		const bool was_studio = obs_frontend_preview_program_mode_active();
		OBSSourceAutoRelease old_program = obs_frontend_get_current_scene();
		OBSSourceAutoRelease old_preview = was_studio ? obs_frontend_get_current_preview_scene() : nullptr;
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_scene(scene);
		result["preview_enabled"] = obs_frontend_preview_enabled();
		result["studio_mode"] = obs_frontend_preview_program_mode_active();
		const std::array<const char *, 4> names{"A1", "B1", "B2", "A2"};
		for (size_t i = 0; i < names.size(); ++i) {
			if (issues.size() != first_issue)
				break;
			const bool bypass = i == 0 || i == 3;
			expect(shadow_reuse_test::counts(shadow, bypass ? 1 : 0).available,
			       "the Scene Shadow operation probe is unavailable");
			QElapsedTimer warmup;
			warmup.start();
			while (warmup.elapsed() < 6000)
				pump();
			OBSSourceAutoRelease current = obs_frontend_get_current_scene();
			expect(current.Get() == scene.Get(), "the fixture is not OBS's current Program scene");
			if (issues.size() != first_issue)
				break;
			// Probe reads and profiler snapshots bracket the quiet interval. No
			// manual renders, screenshots, readbacks or graphics tasks run inside it.
			const auto work_before = shadow_reuse_test::counts(shadow);
			using Snapshot = std::unique_ptr<profiler_snapshot_t, decltype(&profile_snapshot_free)>;
			Snapshot before(profile_snapshot_create(), &profile_snapshot_free);
			const uint64_t frames_before = completed_frames(), lagged_before = obs_get_lagged_frames();
			QElapsedTimer elapsed;
			elapsed.start();
			qint64 next_sample = 1100;
			QJsonArray samples;
			double stats_sum = 0;
			while (completed_frames() - frames_before < 450 && elapsed.elapsed() < 90000) {
				pump();
				if (elapsed.elapsed() >= next_sample) {
					const double ms = double(obs_get_average_frame_time_ns()) / 1000000.0;
					stats_sum += ms;
					samples.append(QJsonObject{{"at_seconds", double(elapsed.elapsed()) / 1000.0},
								   {"average_ms", ms}});
					next_sample = elapsed.elapsed() + 1100;
				}
			}
			const double seconds = double(elapsed.elapsed()) / 1000.0;
			const uint64_t frames = completed_frames() - frames_before;
			const uint64_t lagged = obs_get_lagged_frames() - lagged_before;
			Snapshot after(profile_snapshot_create(), &profile_snapshot_free);
			const auto work = shadow_reuse_test::difference(shadow_reuse_test::counts(shadow), work_before);
			QJsonObject profiles;
			bool profiles_complete = true;
			for (const char *name :
			     {"obs_graphics_thread", "tick_sources", "render_video", "render_displays"}) {
				const auto profile = profile_difference(before.get(), after.get(), name);
				profiles_complete = profiles_complete && profile["frames"].toDouble() + 2 >= 450;
				profiles[name] = profile;
			}
			// The counter reads may include a boundary frame. Require normal
			// Program rendering throughout, not merely one successful test draw.
			const long long minimum = static_cast<long long>(frames - std::min<uint64_t>(frames, 2));
			const bool coverage = work.available && work.stages == 1 && work.passes > 0 &&
					      (bypass ? work.reuses == 0 && work.preparations >= minimum * 2 &&
								work.retentions == 0 && work.eligibility_checks == 0
						      : work.reuses >= minimum && work.preparations == work.reuses &&
								work.retentions >= minimum &&
								work.eligibility_checks >= minimum);
			current = obs_frontend_get_current_scene();
			const bool complete = frames >= 450 && samples.size() >= 3 && stats_sum > 0 && coverage &&
					      profiles_complete && current.Get() == scene.Get();
			const double stats_mean = samples.isEmpty() ? 0.0 : stats_sum / double(samples.size());
			trials.append(QJsonObject{{"name", names[i]},
						  {"bypass", bypass},
						  {"complete", complete},
						  {"completed_frames", double(frames)},
						  {"lagged_frames", double(lagged)},
						  {"elapsed_seconds", seconds},
						  {"obs_stats_sample_mean_ms", stats_mean},
						  {"obs_stats_samples", samples},
						  {"profiles", profiles},
						  {"preparations", double(work.preparations)},
						  {"reuses", double(work.reuses)},
						  {"retentions", double(work.retentions)},
						  {"eligibility_checks", double(work.eligibility_checks)},
						  {"preparation_passes", double(work.passes)}});
			blog(LOG_INFO,
			     "[Windows render timing] %s: complete=%d, bypass=%d, frames=%llu, lagged=%llu, Stats sample mean=%.3f ms, preparations=%lld, reuses=%lld, passes=%lld, retentions=%lld, eligibility=%lld",
			     names[i], int(complete), int(bypass), (unsigned long long)frames,
			     (unsigned long long)lagged, stats_mean, work.preparations, work.reuses, work.passes,
			     work.retentions, work.eligibility_checks);
			result["trials"] = trials;
			save();
			expect(complete, "a quiet trial has incomplete frames, timing samples or render coverage");
			if (!complete)
				break;
		}
		if (old_program)
			obs_frontend_set_current_scene(old_program);
		if (was_studio) {
			obs_frontend_set_preview_program_mode(true);
			if (old_preview)
				obs_frontend_set_current_preview_scene(old_preview);
		}
		obs_source_filter_remove(scene, blur);
		obs_source_filter_remove(scene, shadow);
	}
	if (scene)
		obs_source_remove(scene);
	result["trials"] = trials;
	result["complete"] = issues.size() == first_issue && trials.size() == 4;
	save();
}

} // namespace windows_render_timing_test
