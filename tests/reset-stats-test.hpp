#pragma once

#include "../src/third-party/obs-websocket-api.h"
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QCoreApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGridLayout>
#include <QLabel>
#include <QMainWindow>
#include <QStringList>
#include <QThread>
#include <array>

namespace reset_stats_test {

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

inline qint64 rendered_frames(const QLabel *label)
{
	const QString text = label->text();
	const int slash = text.indexOf('/');
	const int percent = text.indexOf('(', slash);
	if (slash < 0 || percent < 0)
		return -1;
	bool ok = false;
	const qint64 total = text.mid(slash + 1, percent - slash - 1).trimmed().toLongLong(&ok);
	return ok ? total : -1;
}

inline std::array<bool, 5> output_states()
{
	return {obs_frontend_streaming_active(), obs_frontend_recording_active(), obs_frontend_recording_paused(),
		obs_frontend_replay_buffer_active(), obs_frontend_virtualcam_active()};
}

inline void check(QStringList &issues)
{
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *dock = main ? main->findChild<QDockWidget *>("statsDock") : nullptr;
	auto *stats = dock ? dock->widget() : nullptr;
	auto *layout = stats && stats->layout() ? stats->layout()->itemAt(0) : nullptr;
	auto *grid = layout ? qobject_cast<QGridLayout *>(layout->layout()) : nullptr;
	// OBS's Stats has no object names on its labels. This is the rendered-frame
	// denominator in the "Frames missed due to rendering lag" row of its top grid.
	auto *cell = grid ? grid->itemAtPosition(2, 3) : nullptr;
	auto *frames = cell ? qobject_cast<QLabel *>(cell->widget()) : nullptr;
	if (!frames || !stats->inherits("OBSBasicStats")) {
		issues.append("Reset Stats: could not find OBS's rendered-frame counter");
		return;
	}
	if (obs_websocket_get_api_version() == 0) {
		issues.append("Reset Stats: obs-websocket is unavailable in the frontend fixture");
		return;
	}

	const bool was_visible = dock->isVisible();
	const qsizetype first_issue = issues.size();
	for (const bool visible : {false, true}) {
		const QString mode = visible ? QStringLiteral("visible") : QStringLiteral("hidden");
		auto expect = [&](bool value, const QString &message) {
			if (!value)
				issues.append(QStringLiteral("Reset Stats (%1): ").arg(mode) + message);
		};

		// Let OBS publish real preview frames before each press. A counter already
		// at zero would not establish that the request actually reached Reset().
		dock->show();
		if (!until([&] { return rendered_frames(frames) > 10; }, 4500)) {
			expect(false, "rendered-frame counter did not advance before the press");
			break;
		}
		dock->setVisible(visible);
		const qint64 before = rendered_frames(frames);
		const uint32_t total_before = obs_get_total_frames();
		const auto state_before = output_states();
		OBSOutputAutoRelease stream = obs_frontend_get_streaming_output();
		OBSOutputAutoRelease recording = obs_frontend_get_recording_output();
		const uint64_t stream_bytes = stream ? obs_output_get_total_bytes(stream) : 0;
		const uint64_t record_bytes = recording ? obs_output_get_total_bytes(recording) : 0;

		OBSDataAutoRelease request = obs_data_create();
		obs_data_set_string(request, "hotkeyName", "OBSBasic.ResetStats");
		auto *response = obs_websocket_call_request("TriggerHotkeyByName", request);
		const bool accepted = response && response->status_code == 100;
		const QString error = response && response->comment ? QString::fromUtf8(response->comment) : QString();
		obs_websocket_request_response_free(response);
		expect(accepted, QStringLiteral("OBS rejected its Reset Stats hotkey: ") + error);
		if (!accepted)
			break;

		// The OBS hotkey queues the same Reset() used by its button. Pump the
		// frontend until that callback updates the real label, including when hidden.
		qint64 after = -1;
		const bool reset = until(
			[&] {
				after = rendered_frames(frames);
				return after >= 0 && after <= 2 && after < before;
			},
			1500);
		expect(reset, QStringLiteral("rendered-frame counter was not reset (%1 before, %2 after)")
				      .arg(before)
				      .arg(after));
		expect(dock->isVisible() == visible, "the press changed Stats dock visibility");
		expect(obs_get_total_frames() >= total_before, "the press cleared the underlying render total");
		expect(until([&] { return obs_get_total_frames() > total_before + 5; }, 1500),
		       "rendering did not continue after the press");
		OBSOutputAutoRelease stream_after = obs_frontend_get_streaming_output();
		OBSOutputAutoRelease recording_after = obs_frontend_get_recording_output();
		expect(output_states() == state_before && stream_after.Get() == stream.Get() &&
			       recording_after.Get() == recording.Get(),
		       "the press changed an output or its running/paused state");
		expect((!stream || obs_output_get_total_bytes(stream) >= stream_bytes) &&
			       (!recording || obs_output_get_total_bytes(recording) >= record_bytes),
		       "the press cleared an output's total data counter");
		blog(LOG_INFO,
		     "[Reset Stats test] %s Stats: rendered frames before=%lld, after=%lld, reset=%s, rendering continues=%s",
		     mode.toUtf8().constData(), static_cast<long long>(before), static_cast<long long>(after),
		     reset ? "yes" : "no", obs_get_total_frames() > total_before ? "yes" : "no");
	}
	dock->setVisible(was_visible);
	if (issues.size() == first_issue)
		blog(LOG_INFO,
		     "[Reset Stats test] PASS: actual OBS hotkey reset hidden and visible Stats; output state preserved");
}

} // namespace reset_stats_test
