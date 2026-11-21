#pragma once

#include "replacement-frame.hpp"

#include <obs-module.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QStringList>
#include <cstdlib>

namespace camera_test {

inline void wait_frame()
{
	QCoreApplication::processEvents();
	QThread::msleep(20);
}

inline void check(QStringList &issues)
{
	auto *scene = obs_scene_create("Camera transition fixture");
	auto *output = obs_scene_get_source(scene);
	auto *other = obs_scene_create("Camera transition inactive fixture");
	auto *before = obs_get_output_source(0);
	auto *data = obs_data_create_from_json(R"({"width":160,"height":90,"color":4294967295})");
	auto *feed = obs_source_create("color_source_v3", "Camera test feed", data, nullptr);
	obs_data_release(data);
	auto *cv = obs_data_create_from_json(R"({"source":"Camera test feed","crop_to":1})");
	auto *square = obs_source_create("stagekit_cropped_view", "Square test view", cv, nullptr);
	auto *component = obs_source_create("stagekit_cropped_view", "Component test view", cv, nullptr);
	obs_data_release(cv);
	auto *bottom = obs_scene_add(scene, square);
	auto *top = obs_scene_add(scene, component);
	auto *plain = obs_scene_add(other, square); // no assigned transitions in this scene
	auto *already_hidden = obs_scene_add(other, square);
	obs_sceneitem_set_visible(already_hidden, false);
	obs_set_output_source(0, output);
	const uint32_t w = obs_source_get_width(output), h = obs_source_get_height(output);
	auto position = [&](obs_sceneitem_t *item, float x, float y) {
		vec2 pos, scale;
		const float size = .16f * h;
		vec2_set(&scale, size / 90, size / 90);
		vec2_set(&pos, x * w - size / 2, y * h);
		obs_sceneitem_set_alignment(item, OBS_ALIGN_TOP | OBS_ALIGN_LEFT);
		obs_sceneitem_set_scale(item, &scale);
		obs_sceneitem_set_pos(item, &pos);
	};
	position(bottom, .60f, .68f);
	position(top, .40f, .16f);

	// One item has a scene rule. The other carries its own OBS show/hide pair.
	auto *scene_data = obs_data_create_from_json(
		R"({"cover":1,"sources":[{"value":"Square test view"}],"transition":"stagekit_morph","transition_ms":850,"show|stagekit_morph|unpaired":"morph_make_room","show|stagekit_morph|room_way":1})");
	auto *rules = obs_source_create_private("stagekit_scene_transitions", "Camera scene rules", scene_data);
	obs_data_release(scene_data);
	obs_source_filter_add(output, rules);
	auto *make_room = obs_data_create_from_json(R"({"unpaired":"morph_make_room","room_way":1})");
	for (bool show : {true, false}) {
		auto *way = obs_source_create_private("stagekit_morph", "Camera item transition", make_room);
		obs_sceneitem_set_transition(top, show, way);
		obs_sceneitem_set_transition_duration(top, show, show ? 900 : 750);
		obs_source_release(way);
	}
	obs_data_release(make_room);
	auto *settings = obs_data_create_from_json(
		R"({"sources":[{"value":"Square test view"},{"value":"Component test view"}],"restore_hidden":true,"follow":true})");
	auto *sw = obs_source_create_private("stagekit_show_in_scenes", "Camera switch", settings);
	obs_source_filter_add(square, sw);
	for (int i = 0; i < 45; ++i)
		wait_frame();

	auto press_key = [&](bool expected) {
		calldata_t call = {};
		calldata_set_string(&call, "request", "ShowInScenesPress");
		calldata_set_string(
			&call, "data",
			R"({"sourceName":"Square test view","filterName":"Camera switch","how":"toggle","hideAs":"hide"})");
		proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &call);
		const char *raw = calldata_string(&call, "response");
		auto *reply = obs_data_create_from_json(raw ? raw : "{}");
		if (!obs_data_get_bool(reply, "success") || obs_data_get_bool(reply, "enabled") != expected)
			issues.append("Show in Scenes key did not toggle its selected filter");
		obs_data_release(reply);
		calldata_free(&call);
	};
	// A camera moving off its nearest edge crosses a pixel outside its resting box.
	// A cut or a fade at the original position never reaches that pixel.
	auto observe = [&](const char *action, bool shown, bool top_slides) {
		bool moved_top = false, moved_bottom = false, top_present = false, bottom_present = false;
		bool top_faded = false;
		QElapsedTimer clock;
		clock.start();
		do {
			wait_frame();
			const auto frame = replacement_test::render(output, 160, 100, w, h);
			auto level = [&](size_t x, size_t y) {
				const size_t pos = (160 * y + x) * 4;
				return std::min(frame[pos], frame[pos + 3]);
			};
			moved_top |= level(64, 9) > 110;
			moved_bottom |= level(96, 90) > 110;
			const auto top_level = level(64, 24);
			top_faded |= top_level > 22 && top_level < 220;
			top_present = top_level > 180;
			bottom_present = level(96, 76) > 180;
		} while (clock.elapsed() < 1350);
		const bool ok = (top_slides ? moved_top : (!moved_top && top_faded)) && moved_bottom &&
				top_present == shown && bottom_present == shown &&
				!obs_sceneitem_visible(already_hidden);
		if (!ok)
			issues.append(
				QString("%1 %2: top travel %3, bottom travel %4, top fade %5, final camera pixels %6/%7, previously hidden item %8")
					.arg(action, shown ? "show" : "hide")
					.arg(moved_top)
					.arg(moved_bottom)
					.arg(top_faded)
					.arg(top_present)
					.arg(bottom_present)
					.arg(obs_sceneitem_visible(already_hidden)));
		blog(LOG_INFO,
		     "[camera transition test] %s %s: top travel %d, bottom travel %d, top fade %d, final %d/%d",
		     action, shown ? "show" : "hide", moved_top, moved_bottom, top_faded, top_present, bottom_present);
	};
	for (bool show : {false, true}) {
		obs_sceneitem_set_visible(top, show);
		obs_sceneitem_set_visible(bottom, show);
		observe("Sources eye", show, true);
	}
	for (bool show : {false, true}) {
		press_key(show);
		observe("Stream Deck camera switch", show, true);
	}
	for (bool show : {false, true}) {
		obs_source_set_enabled(sw, show);
		observe("OBS filter switch", show, true);
	}
	// Removing the scene rule leaves the square camera's assigned transitions in place.
	obs_source_filter_remove(output, rules);
	for (bool show : {false, true}) {
		press_key(show);
		observe("Item transitions on both cameras", show, true);
	}
	// An explicitly chosen per-source hide still outranks inheritance.
	obs_data_set_string(settings, "for|Component test view|hide_transition", "fade_transition");
	obs_data_set_int(settings, "for|Component test view|hide_ms", 700);
	obs_data_set_int(settings, "transition_ms", 650);
	obs_source_update(sw, settings);
	obs_data_release(settings);
	for (int i = 0; i < 5; ++i)
		wait_frame();
	press_key(false);
	observe("Explicit per-source fade", false, false);
	press_key(true);
	observe("Restore after explicit fade", true, true);
	for (bool show : {true, false}) {
		auto *fallback = obs_sceneitem_get_transition(plain, show);
		if (!fallback || strcmp(obs_source_get_unversioned_id(fallback), "fade_transition") != 0 ||
		    obs_sceneitem_get_transition_duration(plain, show) != 650)
			issues.append("The switch did not update its fallback fade on the unconfigured item");
	}

	// A regular OBS color source linked beside the camera has a different nearest edge.
	auto *caption = obs_scene_add(scene, feed);
	position(caption, .10f, .46f);
	auto *link_settings = obs_data_create();
	obs_data_set_string(link_settings, "linked", obs_source_get_name(feed));
	auto *move = obs_source_create_private("stagekit_send_to", "Linked caption", link_settings);
	obs_data_release(link_settings);
	obs_source_set_enabled(move, false);
	obs_source_filter_add(square, move);
	auto *linked_settings = obs_source_get_settings(sw);
	obs_data_set_string(linked_settings, "transition", "stagekit_morph");
	obs_data_set_int(linked_settings, "transition_ms", 850);
	obs_data_set_string(linked_settings, "show|stagekit_morph|unpaired", "morph_make_room");
	obs_data_set_int(linked_settings, "show|stagekit_morph|room_way", 1);
	auto observe_link = [&](bool show, bool carry) {
		bool crossed = false, together = false, drifted = false, idle = true;
		QElapsedTimer clock;
		clock.start();
		do {
			wait_frame();
			const auto frame = replacement_test::render(output, 160, 100, w, h);
			auto first_row = [&](int x) {
				for (int y = 0; y < 100; ++y) {
					const size_t pixel = (160 * y + x) * 4;
					if (std::min(frame[pixel], frame[pixel + 3]) > 110)
						return y;
				}
				return 100;
			};
			const int primary_y = first_row(96), caption_y = first_row(16);
			crossed |= caption_y >= 70 && caption_y < 95;
			idle &= caption_y >= 45 && caption_y <= 47;
			if (primary_y > 73 && primary_y < 94 && caption_y < 100) {
				const bool aligned = std::abs(primary_y - caption_y - 22) <= 3;
				together |= aligned;
				drifted |= !aligned;
			}
		} while (clock.elapsed() < 1250);
		const bool ok = carry ? crossed && together && !drifted : idle;
		if (!ok || obs_sceneitem_visible(caption) != (carry ? show : true))
			issues.append(QString("Linked color source %1: carry %2 crossed %3 together %4 drifted %5 idle %6")
					      .arg(show ? "show" : "hide").arg(carry).arg(crossed).arg(together)
					      .arg(drifted).arg(idle));
		blog(LOG_INFO, "[camera transition test] linked source %s: carry %d crossed %d together %d drifted %d idle %d",
		     show ? "show" : "hide", carry, crossed, together, drifted, idle);
	};
	for (int phase = 0; phase < 3; ++phase) {
		const bool carry = phase != 2;
		obs_data_set_bool(linked_settings, "with_linked", carry);
		obs_data_set_bool(linked_settings, "restore_hidden", phase == 1);
		obs_source_update(sw, linked_settings);
		for (int i = 0; i < 8; ++i)
			wait_frame();
		for (bool show : {false, true}) {
			obs_source_set_enabled(sw, show);
			observe_link(show, carry);
		}
	}
	obs_data_release(linked_settings);
	obs_source_filter_remove(square, move);
	obs_source_release(move);

	obs_set_output_source(0, before);
	obs_source_release(before);
	obs_source_filter_remove(square, sw);
	obs_source_release(sw);
	obs_source_release(rules);
	obs_scene_release(other);
	obs_scene_release(scene);
	obs_source_release(component);
	obs_source_release(square);
	obs_source_release(feed);
}

} // namespace camera_test
