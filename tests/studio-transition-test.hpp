#pragma once

#include "../src/third-party/obs-websocket-api.h"
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <array>
#include <atomic>
#include <cstring>

namespace studio_transition_test {

template<typename Predicate> bool until(Predicate predicate, int timeout_ms = 3000)
{
	QElapsedTimer elapsed;
	elapsed.start();
	do {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
		if (predicate())
			return true;
		QThread::msleep(10);
	} while (elapsed.elapsed() < timeout_ms);
	return predicate();
}

inline bool current_scene_is(obs_source_t *source, bool preview = false)
{
	OBSSourceAutoRelease current = preview ? obs_frontend_get_current_preview_scene()
					       : obs_frontend_get_current_scene();
	return current.Get() == source;
}

inline bool set_swap_scenes(bool enabled, bool *previous = nullptr)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return false;
	QPushButton *options = nullptr;
	const QString transition = QString::fromUtf8(obs_frontend_get_locale_string("Transition"));
	for (auto *button : main->findChildren<QPushButton *>()) {
		if (button->text() != transition)
			continue;
		for (auto *sibling : button->parentWidget()->findChildren<QPushButton *>())
			if (sibling->property("class").toString() == "icon-dots-vert")
				options = sibling;
	}
	if (!options)
		return false;
	// OBS stores this preference in its main window, not a public frontend
	// setter. Exercise the same menu action as the operator and close its loop.
	bool found = false;
	QElapsedTimer elapsed;
	elapsed.start();
	QTimer watcher;
	watcher.setInterval(25);
	QObject::connect(&watcher, &QTimer::timeout, &watcher, [&] {
		const QString name = QString::fromUtf8(obs_frontend_get_locale_string("QuickTransitions.SwapScenes"));
		for (auto *menu : main->findChildren<QMenu *>()) {
			for (auto *action : menu->actions()) {
				if (action->text() != name || !action->isCheckable())
					continue;
				if (previous)
					*previous = action->isChecked();
				if (action->isChecked() != enabled)
					action->trigger();
				found = true;
				watcher.stop();
				menu->close();
				return;
			}
		}
		if (elapsed.elapsed() < 1000)
			return;
		// A delayed or changed popup must fail this assertion rather than hold
		// OBS inside QMenu::exec until the whole frontend process times out.
		watcher.stop();
		for (auto *widget : QApplication::allWidgets())
			if (auto *menu = qobject_cast<QMenu *>(widget))
				menu->close();
	});
	watcher.start();
	options->click();
	watcher.stop();
	return found;
}

inline std::array<bool, 5> output_states()
{
	return {obs_frontend_streaming_active(), obs_frontend_recording_active(), obs_frontend_recording_paused(),
		obs_frontend_replay_buffer_active(), obs_frontend_virtualcam_active()};
}

inline void check(QStringList &issues)
{
	if (obs_websocket_get_api_version() == 0) {
		issues.append("Preview to Program: obs-websocket is unavailable in the frontend fixture");
		return;
	}
	OBSSourceAutoRelease fade, cut;
	obs_frontend_source_list transitions = {};
	obs_frontend_get_transitions(&transitions);
	for (size_t i = 0; i < transitions.sources.num; ++i) {
		auto *source = transitions.sources.array[i];
		const char *id = obs_source_get_unversioned_id(source);
		if (std::strcmp(id, "fade_transition") == 0)
			fade = obs_source_get_ref(source);
		else if (std::strcmp(id, "cut_transition") == 0)
			cut = obs_source_get_ref(source);
	}
	obs_frontend_source_list_free(&transitions);
	if (!fade || !cut) {
		issues.append("Preview to Program: the fixture has no native Fade or Cut transition");
		return;
	}

	const qsizetype first_issue = issues.size();
	auto expect = [&](bool value, const QString &message) {
		if (!value)
			issues.append(QStringLiteral("Preview to Program: ") + message);
	};
	const bool was_studio = obs_frontend_preview_program_mode_active();
	const bool was_preview_enabled = obs_frontend_preview_enabled();
	OBSSourceAutoRelease old_program = obs_frontend_get_current_scene();
	OBSSourceAutoRelease old_preview = was_studio ? obs_frontend_get_current_preview_scene() : nullptr;
	OBSSourceAutoRelease old_transition = obs_frontend_get_current_transition();
	const int old_duration = obs_frontend_get_transition_duration();
	const auto states_before = output_states();
	OBSOutputAutoRelease stream = obs_frontend_get_streaming_output();
	OBSOutputAutoRelease recording = obs_frontend_get_recording_output();
	OBSSceneAutoRelease first = obs_scene_create("Studio key first scene");
	OBSSceneAutoRelease second = obs_scene_create("Studio key preview scene");
	auto *first_source = obs_scene_get_source(first);
	auto *second_source = obs_scene_get_source(second);
	bool previous_swap = false;
	bool captured_swap = false;

	for (const bool swap : {false, true}) {
		// Begin each trial from the same Program and a distinct Preview. Cut is
		// only for fixture setup; the tested request must use the selected Fade.
		obs_frontend_set_preview_program_mode(false);
		obs_frontend_set_current_transition(cut);
		obs_frontend_set_current_scene(first_source);
		expect(until([&] { return current_scene_is(first_source); }), "could not prepare Program");
		obs_frontend_set_preview_program_mode(true);
		const bool configured_swap = set_swap_scenes(swap, captured_swap ? nullptr : &previous_swap);
		captured_swap |= configured_swap;
		if (!configured_swap) {
			expect(false, "could not set OBS's Swap Preview/Output Scenes option");
			break;
		}
		obs_frontend_set_current_preview_scene(second_source);
		expect(until([&] { return current_scene_is(second_source, true); }), "could not prepare Preview");
		const int duration = swap ? 900 : 450;
		obs_frontend_set_current_transition(fade);
		obs_frontend_set_transition_duration(duration);
		expect(until([&] {
			       OBSSourceAutoRelease current = obs_frontend_get_current_transition();
			       return current.Get() == fade.Get() && obs_frontend_get_transition_duration() == duration;
		       }),
		       "could not select the native Fade and duration");

		struct Observed {
			std::atomic<uint64_t> start{0}, stop{0};
			std::atomic<unsigned> starts{0};
		} observed;
		auto start = [](void *data, calldata_t *) {
			auto &event = *static_cast<Observed *>(data);
			event.start.store(os_gettime_ns());
			++event.starts;
		};
		auto stop = [](void *data, calldata_t *) {
			static_cast<Observed *>(data)->stop.store(os_gettime_ns());
		};
		auto *handler = obs_source_get_signal_handler(fade);
		signal_handler_connect(handler, "transition_start", start, &observed);
		signal_handler_connect(handler, "transition_video_stop", stop, &observed);
		auto *response = obs_websocket_call_request("TriggerStudioModeTransition");
		const bool accepted = response && response->status_code == 100;
		const QString comment = response && response->comment ? QString::fromUtf8(response->comment)
								      : QString();
		obs_websocket_request_response_free(response);
		expect(accepted, QStringLiteral("OBS rejected the transition: ") + comment);
		const bool completed = accepted && until([&] {
					       return observed.stop.load() != 0 && current_scene_is(second_source) &&
						      current_scene_is(swap ? first_source : second_source, true);
				       });
		signal_handler_disconnect(handler, "transition_start", start, &observed);
		signal_handler_disconnect(handler, "transition_video_stop", stop, &observed);
		const uint64_t begun = observed.start.load(), ended = observed.stop.load();
		const double elapsed_ms = ended > begun && begun ? double(ended - begun) / 1000000.0 : 0.0;
		expect(completed,
		       QStringLiteral("Program or Preview did not finish with Swap Scenes %1").arg(swap ? "on" : "off"));
		expect(observed.starts.load() == 1 && elapsed_ms >= duration * 0.75 && elapsed_ms <= duration + 750,
		       QStringLiteral(
			       "native Fade did not use its duration (%1 ms requested, %2 ms observed, %3 starts)")
			       .arg(duration)
			       .arg(elapsed_ms, 0, 'f', 1)
			       .arg(observed.starts.load()));
		OBSSourceAutoRelease current_transition = obs_frontend_get_current_transition();
		expect(current_transition.Get() == fade.Get() && obs_frontend_get_transition_duration() == duration &&
			       obs_frontend_preview_program_mode_active(),
		       "the press changed Studio Mode, the selected transition or its duration");
		blog(LOG_INFO, "[Studio transition test] Swap Scenes %s: Fade %d ms, observed %.1f ms, completed=%s",
		     swap ? "on" : "off", duration, elapsed_ms, completed ? "yes" : "no");
	}
	if (captured_swap)
		expect(set_swap_scenes(previous_swap), "could not restore the Swap Scenes preference");

	obs_frontend_set_preview_program_mode(false);
	OBSSourceAutoRelease before_disabled_press = obs_frontend_get_current_scene();
	auto *disabled = obs_websocket_call_request("TriggerStudioModeTransition");
	expect(disabled && disabled->status_code != 100, "OBS accepted the press while Studio Mode was off");
	obs_websocket_request_response_free(disabled);
	QCoreApplication::processEvents();
	expect(!obs_frontend_preview_program_mode_active() && current_scene_is(before_disabled_press),
	       "the disabled press enabled Studio Mode or changed Program");
	OBSOutputAutoRelease stream_after = obs_frontend_get_streaming_output();
	OBSOutputAutoRelease recording_after = obs_frontend_get_recording_output();
	expect(output_states() == states_before && stream_after.Get() == stream.Get() &&
		       recording_after.Get() == recording.Get(),
	       "the presses changed an output or its running/paused state");

	obs_frontend_set_current_transition(cut);
	if (old_program)
		obs_frontend_set_current_scene(old_program);
	expect(until([&] { return current_scene_is(old_program); }), "could not restore Program");
	if (old_transition)
		obs_frontend_set_current_transition(old_transition);
	obs_frontend_set_transition_duration(old_duration);
	if (was_studio) {
		obs_frontend_set_preview_program_mode(true);
		if (old_preview)
			obs_frontend_set_current_preview_scene(old_preview);
	}
	obs_frontend_set_preview_enabled(was_preview_enabled);
	obs_source_remove(first_source);
	obs_source_remove(second_source);
	QCoreApplication::processEvents();
	if (issues.size() == first_issue)
		blog(LOG_INFO,
		     "[Studio transition test] PASS: actual OBS request honors transition, duration and swap; off press rejected; output state preserved");
}

} // namespace studio_transition_test
