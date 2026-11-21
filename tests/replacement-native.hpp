#pragma once

#include "replacement-frame.hpp"
#include <obs-module.h>
#include <util/platform.h>
#include <atomic>
#include <vector>
#include <QFileInfo>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace replacement_test {
inline void check_clone_audio(QStringList &issues)
{
	obs_audio_info info{};
	if (!obs_get_audio_info(&info)) {
		issues.append("The clone test needs the OBS audio mixer");
		return;
	}
	auto *input = obs_source_create("stagekit_port_source_clone_audio_wrapper_source", "StageKit tone fixture",
					nullptr, nullptr);
	auto *choice = obs_data_create();
	obs_data_set_string(choice, "clone", "StageKit tone fixture");
	auto *clone = obs_source_create_private("stagekit_port_source-clone", "Clone tone", choice);
	obs_data_release(choice);
	if (!input || !clone) {
		issues.append("Could not create the clone audio fixture");
		obs_source_release(clone);
		obs_source_release(input);
		return;
	}
	obs_source_set_audio_mixers(input, 0);
	obs_source_set_audio_mixers(clone, 1u << 5);
	obs_source_t *before = obs_get_output_source(MAX_CHANNELS - 1);
	obs_set_output_source(MAX_CHANNELS - 1, clone);
	obs_source_release(clone);
	std::atomic_bool heard{false};
	auto tap = [](void *p, size_t, audio_data *data) {
		auto *samples = reinterpret_cast<const float *>(data->data[0]);
		for (uint32_t i = 0; samples && i < data->frames; ++i)
			if (std::abs(std::abs(samples[i]) - .1875f) < .002f) {
				static_cast<std::atomic_bool *>(p)->store(true);
				break;
			}
	};
	audio_convert_info format{};
	format.samples_per_sec = info.samples_per_sec;
	format.speakers = info.speakers;
	format.format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs_add_raw_audio_callback(5, &format, tap, &heard);
	const auto frames = info.samples_per_sec / 50;
	std::vector<float> signal(frames);
	for (size_t i = 0; i < signal.size(); ++i)
		signal[i] = (i % 60 < 30) ? .1875f : -.1875f;
	obs_source_audio packet{};
	packet.samples_per_sec = info.samples_per_sec;
	packet.speakers = info.speakers;
	packet.format = AUDIO_FORMAT_FLOAT_PLANAR;
	packet.frames = frames;
	for (size_t ch = 0; ch < get_audio_channels(info.speakers); ++ch)
		packet.data[ch] = reinterpret_cast<const uint8_t *>(signal.data());
	const auto start = os_gettime_ns();
	for (int i = 0; i < 100 && !heard; ++i) {
		packet.timestamp = start + uint64_t(i) * 20000000;
		obs_source_output_audio(input, &packet);
		QThread::msleep(20);
	}
	obs_remove_raw_audio_callback(5, tap, &heard);
	obs_set_output_source(MAX_CHANNELS - 1, before);
	obs_source_release(before);
	obs_source_release(input);
	if (!heard)
		issues.append("Source Clone did not route the input tone to its own OBS audio track");
}

inline void check_native_filters(const QString &art, QStringList &issues)
{
	if (art.isEmpty())
		return;
	auto fail = [&](const char *message) {
		issues.append(QString::fromUtf8(message));
	};
	for (const char *name : {"stagekit-port-freeze", "stagekit-port-pixel", "stagekit-port-3d",
				 "stagekit-port-recursion", "stagekit-port-warp"}) {
		const char *author = obs_get_module_author(obs_get_module(name));
		if (!author || std::strcmp(author, "StageKit contributors"))
			issues.append(QString("Unexpected implementation for %1").arg(name));
	}
	auto *settings = obs_data_create();
	const auto path = art.toUtf8();
	obs_data_set_string(settings, "file", path.constData());
	auto *source = obs_source_create("image_source", "Native component test", settings, nullptr);
	if (!source) {
		fail("Could not create native component fixture");
		obs_data_release(settings);
		return;
	}
	obs_source_inc_showing(source);
	QThread::msleep(120);
	const uint32_t w = obs_source_get_width(source), h = obs_source_get_height(source);
	if (w != 320 || h != 180) {
		fail("Unexpected native fixture dimensions");
	} else {
		auto *saved = obs_data_create_from_json("{\"duration\":0,\"mask\":false,\"refresh_interval\":0}");
		auto *hold = obs_source_create_private("stagekit_port_freeze_filter", "Previously saved hold", saved);
		obs_data_release(saved);
		if (!hold) {
			fail("The beta234 Freeze ID did not load");
		} else {
			obs_source_filter_add(source, hold);
			QThread::msleep(80);
			const auto original = render(source, w, h);
			const auto alternate = (QFileInfo(art).absolutePath() + "/alternate.png").toUtf8();
			obs_data_set_string(settings, "file", alternate.constData());
			obs_source_update(source, settings);
			QThread::msleep(160);
			const auto still = render(source, w, h);
			if (!retains_artwork(original) || still != original)
				fail("Freeze did not retain the original frame while its source changed");
			obs_source_filter_remove(source, hold);
			obs_source_release(hold);
			QThread::msleep(60);
			if (render(source, w, h) == original)
				fail("Removing Freeze did not reveal the new input");
		}
		obs_data_set_string(settings, "file", path.constData());
		obs_source_update(source, settings);
		QThread::msleep(120);
		const auto original = render(source, w, h);
		saved = obs_data_create_from_json(
			"{\"downscales\":3,\"redcount\":4,\"greencount\":3,\"bluecount\":2,\"bayerlevel\":1,\"ditherspread\":0.2}");
		auto *pixel =
			obs_source_create_private("stagekit_port_pixel_art_plugin", "Previously saved pixels", saved);
		obs_data_release(saved);
		if (!pixel) {
			fail("The beta234 Pixel Art ID did not load");
		} else {
			obs_source_filter_add(source, pixel);
			QThread::msleep(100);
			const auto changed = render(source, w, h);
			if (!retains_artwork(changed) || changed == original)
				fail("Pixel Art did not render visible, changed pixels");
			else {
				size_t solid = 0, tested = 0;
				for (uint32_t y = 50; y < 120; y += 8)
					for (uint32_t x = 52; x < 160; x += 8) {
						const size_t pos = 4 * (size_t(y) * w + x);
						++tested;
						solid += !std::memcmp(&changed[pos], &changed[pos + 4], 4);
					}
				if (solid < tested * 9 / 10)
					fail("Pixel Art did not form the requested blocks");
			}
			obs_source_filter_remove(source, pixel);
			obs_source_release(pixel);
		}
		auto exercise_effect = [&](const char *id, const char *json, const char *message,
					   bool hold_after_scan) {
			auto *preset = obs_data_create_from_json(json);
			auto *filter = obs_source_create_private(id, "Saved beta234 settings", preset);
			obs_data_release(preset);
			if (!filter) {
				fail(message);
				return;
			}
			obs_source_filter_add(source, filter);
			QThread::msleep(80);
			auto first = render(source, w, h);
			QThread::msleep(70);
			auto output = render(source, w, h);
			if (hold_after_scan) {
				auto alt = (QFileInfo(art).absolutePath() + "/alternate.png").toUtf8();
				obs_data_set_string(settings, "file", alt.constData());
				obs_source_update(source, settings);
				QThread::msleep(150);
				if (!retains_artwork(first) || first != render(source, w, h))
					fail(message);
			} else if (!retains_artwork(output) || output == original) {
				fail(message);
			}
			obs_source_filter_remove(source, filter);
			obs_source_release(filter);
			obs_data_set_string(settings, "file", path.constData());
			obs_source_update(source, settings);
			QThread::msleep(100);
		};
		exercise_effect("stagekit_port_3d_effect_filter", R"({"mode":1,"pos_x":25,"rot_z":8})",
				"3D Effect failed to project the saved pose", false);
		exercise_effect("stagekit_port_recursion_effect_filter",
				R"({"offset_x":32,"delay_ms":1,"alpha":0.8,"inversed":true})",
				"Recursion failed to draw an offset previous frame", false);
		exercise_effect("stagekit_port_time_warp_scan_filter",
				R"({"scan_duration":0,"rotation":90,"line_width":0})",
				"A completed scan did not hold the scanned image", true);
	}
	auto *delay_options = obs_data_create_from_json(R"({"duration":1.0,"easing_duration":0.0})");
	auto *delay = obs_source_create_private("stagekit_port_dynamic_delay_filter", "Saved delay", delay_options);
	obs_data_release(delay_options);
	if (delay && w == 320 && h == 180) {
		obs_source_filter_add(source, delay);
		QThread::msleep(50);
		for (int i = 0; i < 5; ++i) {
			render(source, w, h);
			QThread::msleep(25);
		}
		struct PlaybackKey {
			obs_source_t *parent;
			obs_hotkey_id id;
		} key{source, OBS_INVALID_HOTKEY_ID};
		obs_enum_hotkeys(
			[](void *arg, obs_hotkey_id id, obs_hotkey_t *hotkey) {
				auto &key = *static_cast<PlaybackKey *>(arg);
				if (obs_hotkey_get_registerer_type(hotkey) == OBS_HOTKEY_REGISTERER_SOURCE &&
				    std::strcmp(obs_hotkey_get_name(hotkey), "pause") == 0 &&
				    obs_weak_source_references_source(
					    static_cast<obs_weak_source_t *>(obs_hotkey_get_registerer(hotkey)),
					    key.parent)) {
					key.id = id;
					return false;
				}
				return true;
			},
			&key);
		if (key.id == OBS_INVALID_HOTKEY_ID)
			fail("The beta234 Dynamic Delay pause key was not registered on the parent");
		else {
			obs_hotkey_trigger_routed_callback(key.id, true);
			obs_hotkey_trigger_routed_callback(key.id, false);
			render(source, w, h);
			QThread::msleep(50);
			const auto held = render(source, w, h);
			const auto alternate = (QFileInfo(art).absolutePath() + "/alternate.png").toUtf8();
			obs_data_set_string(settings, "file", alternate.constData());
			obs_source_update(source, settings);
			QThread::msleep(120);
			if (!retains_artwork(held) || render(source, w, h) != held)
				fail("Dynamic Delay did not hold playback on its saved pause hotkey");
		}
		obs_source_filter_remove(source, delay);
	}
	if (!delay)
		fail("Saved Dynamic Delay ID did not load");
	obs_source_release(delay);
	obs_data_set_string(settings, "file", path.constData());
	obs_source_update(source, settings);
	QThread::msleep(100);
	auto *watch_settings = obs_data_create();
	const auto folder = QFileInfo(art).absolutePath().toUtf8();
	obs_data_set_string(watch_settings, "dir", folder.constData());
	obs_data_set_string(watch_settings, "filter", "alternate");
	obs_data_set_string(watch_settings, "extension", ".png");
	obs_data_set_int(watch_settings, "scan_interval", 0);
	auto *watch = obs_source_create_private("stagekit_port_dir_watch_media", "Folder preset", watch_settings);
	obs_data_release(watch_settings);
	if (watch) {
		obs_source_filter_add(source, watch);
		bool picked = false;
		for (int i = 0; i < 30 && !picked; ++i) {
			QThread::msleep(25);
			auto *value = obs_source_get_settings(source);
			picked = QFileInfo(QString::fromUtf8(obs_data_get_string(value, "file"))).fileName() ==
				 "alternate.png";
			obs_data_release(value);
		}
		if (!picked)
			fail("Directory Watch did not apply the saved folder and filename filter");
		obs_source_filter_remove(source, watch);
		obs_source_release(watch);
	} else {
		fail("Saved Directory Watch filter could not load");
	}

	auto *model = obs_source_create("image_source", "Native defaults model", nullptr, nullptr);
	auto *options = obs_data_create();
	obs_data_set_bool(options, "copy_properties", false);
	obs_data_set_bool(options, "copy_filters", false);
	obs_data_set_string(options, "name_prefix", "Default ");
	auto *marker = obs_source_create_private("stagekit_port_source_defaults_video_v1", "Saved defaults", options);
	obs_data_release(options);
	if (model && marker) {
		obs_source_set_volume(model, 0.37f);
		obs_source_filter_add(model, marker);
		auto *target = obs_source_create("image_source", "New native test source", nullptr, nullptr);
		bool applied = false;
		for (int i = 0; target && i < 30 && !applied; ++i) {
			QThread::msleep(25);
			applied = std::abs(obs_source_get_volume(target) - .37f) < .001f &&
				  QString::fromUtf8(obs_source_get_name(target)).startsWith("Default ");
		}
		if (!applied)
			fail("Source Defaults did not apply the saved volume and name prefix to a newly created source");
		obs_source_filter_remove(model, marker);
		obs_source_release(target);
	} else {
		fail("Saved Source Defaults ID could not load");
	}
	obs_source_release(marker);
	obs_source_release(model);
	obs_data_set_string(settings, "file", path.constData());
	obs_source_update(source, settings);
	QThread::msleep(100);
	auto *clone_options = obs_data_create();
	obs_data_set_string(clone_options, "clone", obs_source_get_name(source));
	obs_data_set_int(clone_options, "buffer_frame", 2);
	auto *copy = obs_source_create_private("stagekit_port_source-clone", "Saved buffered clone", clone_options);
	obs_data_release(clone_options);
	if (copy) {
		obs_source_inc_showing(copy);
		QThread::msleep(120);
		if (obs_source_get_width(copy) != w / 2 || obs_source_get_height(copy) != h / 2 ||
		    !retains_artwork(render(copy, w / 2, h / 2)))
			fail("Source Clone did not render the saved half-resolution image");
		obs_source_dec_showing(copy);
	} else
		fail("The saved Source Clone ID did not load");
	obs_source_release(copy);
	check_clone_audio(issues);
	obs_source_dec_showing(source);
	obs_source_release(source);
	obs_data_release(settings);
}
} // namespace replacement_test
