#pragma once

#include "replacement-frame.hpp"
#include <obs-module.h>
#include <QFileInfo>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cstring>

namespace replacement_test {
inline void check_native_filters(const QString &art, QStringList &issues)
{
	if (art.isEmpty())
		return;
	auto fail = [&](const char *message) { issues.append(QString::fromUtf8(message)); };
	for (const char *name : {"stagekit-port-freeze", "stagekit-port-pixel", "stagekit-port-3d", "stagekit-port-recursion", "stagekit-port-warp"}) {
		const char *author = obs_get_module_author(obs_get_module(name));
		if (!author || std::strcmp(author, "StageKit contributors"))
			issues.append(QString("Unexpected implementation for %1").arg(name));
	}
	auto *settings = obs_data_create();
	const auto path = art.toUtf8();
	obs_data_set_string(settings, "file", path.constData());
	auto *source = obs_source_create_private("image_source", "Native component test", settings);
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
		saved = obs_data_create_from_json("{\"downscales\":3,\"redcount\":4,\"greencount\":3,\"bluecount\":2,\"bayerlevel\":1,\"ditherspread\":0.2}");
		auto *pixel = obs_source_create_private("stagekit_port_pixel_art_plugin", "Previously saved pixels", saved);
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
		auto exercise_effect = [&](const char *id, const char *json, const char *message, bool hold_after_scan) {
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
		exercise_effect("stagekit_port_3d_effect_filter", R"({"mode":1,"pos_x":25,"rot_z":8})", "3D Effect failed to project the saved pose", false);
		exercise_effect("stagekit_port_recursion_effect_filter", R"({"offset_x":32,"delay_ms":1,"alpha":0.8,"inversed":true})", "Recursion failed to draw an offset previous frame", false);
		exercise_effect("stagekit_port_time_warp_scan_filter", R"({"scan_duration":0,"rotation":90,"line_width":0})", "A completed scan did not hold the scanned image", true);
	}
	obs_source_dec_showing(source);
	obs_source_release(source);
	obs_data_release(settings);
}
} // namespace replacement_test
