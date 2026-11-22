#pragma once

#include "replacement-frame.hpp"
#include "../src/third-party/obs-websocket-api.h"
#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <graphics/matrix4.h>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFormLayout>
#include <QFileInfo>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStringList>
#include <QThread>
#include <QWizard>
#include <cmath>
#include <mutex>
#include <string>

namespace auto_frame_detection_test {

// A real OBS source with capture metadata and a painted picture. The stored
// window deliberately names an old target: get_hooked is what is on screen.
struct Capture {
	std::mutex mutex;
	std::string title, executable;
	uint32_t width = 480, height = 270, picture_width = 300, picture_height = 270;
	bool hooked = true;
};

inline void update_capture(void *data, obs_data_t *settings)
{
	auto &capture = *static_cast<Capture *>(data);
	std::lock_guard lock(capture.mutex);
	capture.title = obs_data_get_string(settings, "title");
	capture.executable = obs_data_get_string(settings, "executable");
	capture.width = uint32_t(obs_data_get_int(settings, "width"));
	capture.height = uint32_t(obs_data_get_int(settings, "height"));
	capture.picture_width = uint32_t(obs_data_get_int(settings, "picture_width"));
	capture.picture_height = uint32_t(obs_data_get_int(settings, "picture_height"));
	capture.hooked = obs_data_get_bool(settings, "hooked");
}

inline void register_capture()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	obs_source_info info{};
	info.id = "stagekit_test_auto_frame_detection";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Auto Frame detection fixture";
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		auto *capture = new Capture;
		update_capture(capture, settings);
		proc_handler_add(
			obs_source_get_proc_handler(source), "void set_fixture(in ptr settings)",
			[](void *data, calldata_t *call) {
				update_capture(data, static_cast<obs_data_t *>(calldata_ptr(call, "settings")));
			},
			capture);
		if (!obs_data_get_bool(settings, "without_hook"))
			proc_handler_add(
				obs_source_get_proc_handler(source),
				"void get_hooked(out bool hooked, out string executable, out string title)",
				[](void *data, calldata_t *call) {
					auto &capture = *static_cast<Capture *>(data);
					std::lock_guard lock(capture.mutex);
					calldata_set_bool(call, "hooked", capture.hooked);
					calldata_set_string(call, "executable",
							    capture.hooked ? capture.executable.c_str() : "");
					calldata_set_string(call, "title", capture.hooked ? capture.title.c_str() : "");
				},
				capture);
		return capture;
	};
	info.destroy = [](void *data) {
		delete static_cast<Capture *>(data);
	};
	info.update = update_capture;
	info.get_width = [](void *data) -> uint32_t {
		auto &capture = *static_cast<Capture *>(data);
		std::lock_guard lock(capture.mutex);
		return capture.hooked ? capture.width : 0;
	};
	info.get_height = [](void *data) -> uint32_t {
		auto &capture = *static_cast<Capture *>(data);
		std::lock_guard lock(capture.mutex);
		return capture.hooked ? capture.height : 0;
	};
	info.video_render = [](void *data, gs_effect_t *) {
		auto &capture = *static_cast<Capture *>(data);
		std::lock_guard lock(capture.mutex);
		if (!capture.hooked || !capture.width || !capture.height)
			return;
		auto *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		auto *color = gs_effect_get_param_by_name(solid, "color");
		vec4 ink;
		vec4_set(&ink, 0.0f, 0.0f, 0.0f, 1.0f);
		gs_effect_set_vec4(color, &ink);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, capture.width, capture.height);
		if (!capture.picture_width || !capture.picture_height)
			return;
		gs_matrix_push();
		gs_matrix_translate3f(float(capture.width - capture.picture_width) * 0.5f,
				      float(capture.height - capture.picture_height) * 0.5f, 0.0f);
		vec4_set(&ink, 0.2f, 0.8f, 0.6f, 1.0f);
		gs_effect_set_vec4(color, &ink);
		while (gs_effect_loop(solid, "Solid"))
			gs_draw_sprite(nullptr, 0, capture.picture_width, capture.picture_height);
		gs_matrix_pop();
	};
	obs_register_source(&info);
}

inline void aim(obs_source_t *source, const char *title, uint32_t picture_width, uint32_t picture_height,
		uint32_t width = 480, uint32_t height = 270, bool hooked = true,
		const char *executable = "example-emulator.exe")
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "title", title);
	obs_data_set_string(settings, "executable", executable);
	obs_data_set_string(settings, "window", "Stale Target:WindowClass:stale-target.exe");
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_int(settings, "picture_width", picture_width);
	obs_data_set_int(settings, "picture_height", picture_height);
	obs_data_set_bool(settings, "hooked", hooked);
	// Capture hooks can switch without changing stored source settings. Set the
	// fixture metadata immediately so a command sees this target before the next
	// video frame, including when testing a switch during a pending measurement.
	calldata_t call = {};
	calldata_set_ptr(&call, "settings", settings);
	proc_handler_call(obs_source_get_proc_handler(source), "set_fixture", &call);
	calldata_free(&call);
}

template<typename Predicate> bool until(obs_source_t *source, Predicate predicate, int timeout_ms = 3000)
{
	QElapsedTimer timeout;
	timeout.start();
	do {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		if (source)
			replacement_test::render(source, 480, 270);
		if (predicate())
			return true;
		QThread::msleep(20);
	} while (timeout.elapsed() < timeout_ms);
	return predicate();
}

inline QComboBox *choice(QDialog *dialog, const QString &label)
{
	if (!dialog)
		return nullptr;
	for (auto *form : dialog->findChildren<QFormLayout *>())
		for (int row = 0; row < form->rowCount(); ++row) {
			auto *label_item = form->itemAt(row, QFormLayout::LabelRole);
			auto *field_item = form->itemAt(row, QFormLayout::FieldRole);
			auto *label_widget = label_item ? label_item->widget() : nullptr;
			auto *text = qobject_cast<QLabel *>(label_widget);
			bool matches = text && text->text().contains(label);
			// OBS wraps labels with tooltips alongside a help icon. The icon
			// may be the first QLabel child, so inspect every label in the row.
			if (label_widget && !matches)
				for (auto *child : label_widget->findChildren<QLabel *>())
					matches = matches || child->text().contains(label);
			auto *combo = field_item ? qobject_cast<QComboBox *>(field_item->widget()) : nullptr;
			if (matches && combo && combo->isVisible())
				return combo;
		}
	return nullptr;
}

inline QPushButton *button(QDialog *dialog, const QString &text)
{
	if (dialog)
		for (auto *button : dialog->findChildren<QPushButton *>())
			if (button->isVisible() && button->text() == text)
				return button;
	return nullptr;
}

inline QString detection(QDialog *dialog)
{
	if (dialog)
		for (auto *label : dialog->findChildren<QLabel *>())
			if (label->isVisible() && label->text().startsWith("Detected aspect ratio:"))
				return label->text();
	return {};
}

inline double ratio(const QString &text)
{
	const auto parts = text.split(':');
	bool width_ok = false, height_ok = true;
	const double width = parts.value(0).trimmed().toDouble(&width_ok);
	const double height = parts.size() > 1 ? parts.value(1).trimmed().toDouble(&height_ok) : 1.0;
	return width_ok && height_ok && width > 0.0 && height > 0.0 ? width / height : 0.0;
}

inline std::string rules(obs_source_t *filter)
{
	OBSDataAutoRelease settings = obs_source_get_settings(filter);
	return obs_data_get_string(settings, "rules_text");
}

inline OBSDataAutoRelease vendor(const char *name, obs_data_t *data)
{
	OBSDataAutoRelease request = obs_data_create();
	obs_data_set_string(request, "vendorName", "StageKit");
	obs_data_set_string(request, "requestType", name);
	obs_data_set_obj(request, "requestData", data);
	auto *response = obs_websocket_call_request("CallVendorRequest", request);
	OBSDataAutoRelease payload = response && response->status_code == 100 && response->response_data
					     ? obs_data_create_from_json(response->response_data)
					     : obs_data_create();
	obs_websocket_request_response_free(response);
	OBSDataAutoRelease result = obs_data_get_obj(payload, "responseData");
	return result ? obs_data_newref(result) : obs_data_create();
}

inline OBSDataAutoRelease request(obs_source_t *source, obs_source_t *filter, const char *action = nullptr,
				  long long request_id = 0, const char *remember_by = nullptr, int timeout_ms = 0)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "sourceName", obs_source_get_name(source));
	obs_data_set_string(data, "filterName", obs_source_get_name(filter));
	if (action)
		obs_data_set_string(data, "action", action);
	if (request_id > 0)
		obs_data_set_int(data, "requestId", request_id);
	if (remember_by)
		obs_data_set_string(data, "rememberBy", remember_by);
	if (timeout_ms > 0)
		obs_data_set_int(data, "timeoutMs", timeout_ms);
	return vendor(action ? "AutoFrameCommand" : "GetAutoFrameState", data);
}

inline void check_scoped_rules(QStringList &issues)
{
	const qsizetype first_issue = issues.size();
	auto expect = [&](bool value, const QString &message) {
		if (!value)
			issues.append(QStringLiteral("Auto Frame scopes: ") + message);
	};
	constexpr const char *app = "shared-emulator.exe";
	constexpr const char *other_app = "other-emulator.exe";
	const std::string first_title = "Scope, Demo; = \"Alpha\"\r\nLine #3A";
	const std::string second_title = "Scope, Demo; = \"Beta\"\r\nLine #3A";
	OBSDataAutoRelease frame_settings = obs_data_create();
	obs_data_set_int(frame_settings, "frame_width", 480);
	obs_data_set_int(frame_settings, "frame_height", 270);
	obs_data_set_int(frame_settings, "glide_ms", 0);
	obs_data_set_double(frame_settings, "trim_wait", 0.0);
	obs_data_set_bool(frame_settings, "known_programs", false);
	obs_data_set_string(frame_settings, "rules_text", "shared-emulator = 1:1");
	OBSSourceAutoRelease source =
		obs_source_create("stagekit_test_auto_frame_detection", "Scoped Auto Frame capture", nullptr, nullptr);
	OBSSourceAutoRelease filter =
		obs_source_create_private("stagekit_auto_frame", "Scoped Auto Frame", frame_settings);
	if (!source || !filter) {
		expect(false, "could not create the scoped capture fixture");
		return;
	}
	obs_source_filter_add(source, filter);
	obs_source_inc_showing(source);
	const std::string legacy_rules = rules(filter);
	auto count = [&] {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		OBSDataArrayAutoRelease entries = obs_data_get_array(settings, "scoped_rules");
		return obs_data_array_count(entries);
	};
	auto snapshot = [&] {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		OBSDataArrayAutoRelease entries = obs_data_get_array(settings, "scoped_rules");
		OBSDataAutoRelease saved = obs_data_create();
		obs_data_set_array(saved, "scoped_rules", entries);
		return std::string(obs_data_get_json(saved));
	};
	auto contains = [&](const char *scope, const char *executable, const std::string &title, double shape) {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		OBSDataArrayAutoRelease entries = obs_data_get_array(settings, "scoped_rules");
		for (size_t i = 0; i < obs_data_array_count(entries); ++i) {
			OBSDataAutoRelease entry = obs_data_array_item(entries, i);
			if (std::string(obs_data_get_string(entry, "scope")) == scope &&
			    std::string(obs_data_get_string(entry, "executable")) == executable &&
			    std::string(obs_data_get_string(entry, "title")) == title &&
			    std::abs(obs_data_get_double(entry, "ratio") - shape) < 0.006)
				return true;
		}
		return false;
	};
	auto complete = [&](OBSDataAutoRelease &state) {
		const long long id = obs_data_get_int(state, "requestId");
		return until(source, [&] {
			state = request(source, filter, nullptr, id);
			return obs_data_get_bool(state, "complete");
		});
	};
	auto save = [&](const std::string &title, uint32_t width, uint32_t height, const char *scope,
			const char *executable, bool separate_measure = false) {
		aim(source, title.c_str(), width, height, 480, 270, true, executable);
		if (separate_measure) {
			auto measured = request(source, filter, "measure");
			expect(complete(measured) && obs_data_get_bool(measured, "success") &&
				       !obs_data_get_bool(measured, "saved") &&
				       !obs_data_has_user_value(measured, "rememberBy"),
			       "legacy Measure was altered by adding scoped saves");
		}
		auto state = request(source, filter, separate_measure ? "remember" : "measure_and_remember", 0, scope);
		const std::string target = std::string(scope) == "window" ? title : executable;
		auto context_is = [&] {
			return obs_data_get_bool(state, "supportsRememberBy") &&
			       std::string(obs_data_get_string(state, "rememberBy")) == scope &&
			       std::string(obs_data_get_string(state, "rememberTarget")) == target;
		};
		expect(obs_data_get_bool(state, "success") && !obs_data_get_bool(state, "complete") && context_is(),
		       QStringLiteral("scoped save did not start with its exact scope and target: %1")
			       .arg(QString::fromUtf8(obs_data_get_json(state))));
		const long long id = obs_data_get_int(state, "requestId");
		bool context_kept = true;
		expect(until(source,
			     [&] {
				     state = request(source, filter, nullptr, id);
				     context_kept = context_kept && context_is();
				     return obs_data_get_bool(state, "complete");
			     }),
		       "scoped save did not complete");
		expect(context_kept && obs_data_get_bool(state, "success") && obs_data_get_bool(state, "saved") &&
			       std::string(obs_data_get_string(state, "status")) == "saved" &&
			       std::abs(obs_data_get_double(state, "ratio") - double(width) / height) < 0.006 &&
			       rules(filter) == legacy_rules,
		       QStringLiteral("scoped save changed identity, ratio or legacy rules: %1")
			       .arg(QString::fromUtf8(obs_data_get_json(state))));
	};
	// Check the actual crop after rendering, as well as the saved-rule status.
	auto applied = [&](const std::string &title, const char *executable, double shape) {
		aim(source, title.c_str(), 480, 270, 480, 270, true, executable);
		return until(source, [&] {
			calldata_t call = {};
			const bool got = proc_handler_call(obs_source_get_proc_handler(filter), "get_trim", &call);
			const double x = calldata_float(&call, "x"), y = calldata_float(&call, "y");
			calldata_free(&call);
			auto *properties = obs_source_properties(filter);
			auto *status = properties ? obs_properties_get(properties, "status") : nullptr;
			const bool saved =
				status &&
				std::string(obs_property_description(status)).find("from your saved aspect ratios") !=
					std::string::npos;
			obs_properties_destroy(properties);
			return got && saved && std::abs((480.0 - 2.0 * x) / (270.0 - 2.0 * y) - shape) < 0.006;
		});
	};
	auto idle = request(source, filter);
	expect(obs_data_get_bool(idle, "success") && obs_data_get_bool(idle, "supportsRememberBy") &&
		       std::string(obs_data_get_string(idle, "status")) == "idle",
	       "a new valid filter did not advertise scoped-save support");
	save("Application default", 360, 270, "application", app, true);
	save(first_title, 300, 270, "window", app);
	save(second_title, 480, 270, "window", app);
	save(first_title, 320, 256, "application", other_app);
	expect(count() == 4 && contains("window", app, first_title, 10.0 / 9.0) &&
		       contains("window", app, second_title, 16.0 / 9.0),
	       "delimiter-bearing window titles were truncated, collided, or lost their application identity");
	expect(applied(first_title, app, 10.0 / 9.0) && applied(second_title, app, 16.0 / 9.0) &&
		       applied("Unknown game", app, 4.0 / 3.0),
	       "two window rules did not override their application default independently");
	expect(applied(first_title, other_app, 5.0 / 4.0),
	       "a window rule leaked into a different application with the exact same title");
	// Saving the shared application's default last must not replace either game.
	save("Application default changed", 360, 240, "application", app);
	expect(count() == 4 && applied(first_title, app, 10.0 / 9.0) && applied(second_title, app, 16.0 / 9.0) &&
		       applied("Unknown game", app, 3.0 / 2.0) && applied(first_title, other_app, 5.0 / 4.0),
	       "saving an application default later overrode a window or another application's default");
	expect(applied("Unknown game", "SHARED-EMULATOR.EXE", 3.0 / 2.0),
	       "application identity was not case-insensitive");
	expect(applied("Unknown game", "prefix-shared-emulator.exe", 1.0),
	       "an exact application rule matched an executable substring instead of using the legacy fallback");
	const std::string before_failures = snapshot();
	for (int invalid_scope = 0; invalid_scope < 4; ++invalid_scope) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "sourceName", obs_source_get_name(source));
		obs_data_set_string(data, "filterName", obs_source_get_name(filter));
		obs_data_set_string(data, "action", "measure_and_remember");
		if (invalid_scope == 0)
			obs_data_set_string(data, "rememberBy", "");
		else if (invalid_scope == 1)
			obs_data_set_string(data, "rememberBy", "process");
		else if (invalid_scope == 2)
			obs_data_set_int(data, "rememberBy", 1);
		else
			obs_data_set_bool(data, "rememberBy", true);
		auto invalid = vendor("AutoFrameCommand", data);
		expect(!obs_data_get_bool(invalid, "success") &&
			       std::string(obs_data_get_string(invalid, "error")).find("rememberBy") !=
				       std::string::npos &&
			       snapshot() == before_failures,
		       QStringLiteral("malformed save scope %1 was accepted").arg(invalid_scope));
	}
	const auto before_invalid_timeouts = request(source, filter);
	const long long unchanged_request_id = obs_data_get_int(before_invalid_timeouts, "requestId");
	for (int invalid_timeout = 0; invalid_timeout < 7; ++invalid_timeout) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "sourceName", obs_source_get_name(source));
		obs_data_set_string(data, "filterName", obs_source_get_name(filter));
		obs_data_set_string(data, "action", "measure_and_remember");
		obs_data_set_string(data, "rememberBy", "window");
		switch (invalid_timeout) {
		case 0:
			obs_data_set_int(data, "timeoutMs", 0);
			break;
		case 1:
			obs_data_set_int(data, "timeoutMs", -1);
			break;
		case 2:
			obs_data_set_int(data, "timeoutMs", 4001);
			break;
		case 3:
			obs_data_set_int(data, "timeoutMs", 9007199254740991LL);
			break;
		case 4:
			obs_data_set_double(data, "timeoutMs", 1.5);
			break;
		case 5:
			obs_data_set_string(data, "timeoutMs", "250");
			break;
		case 6:
			obs_data_set_bool(data, "timeoutMs", true);
			break;
		}
		auto invalid = vendor("AutoFrameCommand", data);
		auto unchanged = request(source, filter);
		expect(!obs_data_get_bool(invalid, "success") &&
			       std::string(obs_data_get_string(invalid, "error")).find("timeoutMs") !=
				       std::string::npos &&
			       obs_data_get_int(unchanged, "requestId") == unchanged_request_id &&
			       snapshot() == before_failures,
		       QStringLiteral("malformed timeout %1 changed the command or its saved rules")
			       .arg(invalid_timeout));
	}
	for (const char *scope : {"application", "window"}) {
		aim(source, first_title.c_str(), 300, 270, 480, 270, true, "");
		auto missing = request(source, filter, "measure_and_remember", 0, scope);
		expect(!obs_data_get_bool(missing, "success") && obs_data_get_bool(missing, "complete") &&
			       !obs_data_get_bool(missing, "saved") &&
			       obs_data_get_bool(missing, "supportsRememberBy") &&
			       std::string(obs_data_get_string(missing, "rememberBy")) == scope &&
			       snapshot() == before_failures,
		       "a scoped save fell back to a window title without an application identity");
	}
	aim(source, "", 300, 270, 480, 270, true, app);
	auto no_title = request(source, filter, "measure_and_remember", 0, "window");
	expect(!obs_data_get_bool(no_title, "success") && obs_data_get_bool(no_title, "complete") &&
		       !obs_data_get_bool(no_title, "saved") && !*obs_data_get_string(no_title, "rememberTarget") &&
		       snapshot() == before_failures,
	       "a Window save silently fell back to the application's name");
	auto recoverable = request(source, filter);
	expect(!obs_data_get_bool(recoverable, "success") && obs_data_get_bool(recoverable, "supportsRememberBy"),
	       "a failed previous command hid the valid filter's scoped-save capability");
	for (const char *scope : {"application", "window"}) {
		obs_enter_graphics();
		aim(source, "Scoped change first", 300, 270, 480, 270, true, app);
		auto changed = request(source, filter, "measure_and_remember", 0, scope);
		aim(source, "Scoped change second", 480, 270, 480, 270, true, app);
		obs_leave_graphics();
		expect(complete(changed) && !obs_data_get_bool(changed, "success") &&
			       !obs_data_get_bool(changed, "saved") &&
			       std::string(obs_data_get_string(changed, "rememberBy")) == scope &&
			       std::string(obs_data_get_string(changed, "rememberTarget")) ==
				       (std::string(scope) == "window" ? "Scoped change first" : app) &&
			       snapshot() == before_failures,
		       "changing games within one executable saved a stale scoped measurement");
	}
	// Round-trip the actual OBS settings through JSON, then recreate the filter.
	auto reload = [&] {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		OBSDataAutoRelease loaded = obs_data_create_from_json(obs_data_get_json(settings));
		obs_source_filter_remove(source, filter);
		filter = obs_source_create_private("stagekit_auto_frame", "Scoped Auto Frame", loaded);
		if (filter)
			obs_source_filter_add(source, filter);
		return filter != nullptr;
	};
	if (!reload()) {
		expect(false, "could not reload the scoped filter settings");
		obs_source_dec_showing(source);
		obs_source_remove(source);
		return;
	}
	expect(snapshot() == before_failures && applied(first_title, app, 10.0 / 9.0) &&
		       applied(second_title, app, 16.0 / 9.0) && applied("Unknown game", app, 3.0 / 2.0) &&
		       applied(first_title, other_app, 5.0 / 4.0),
	       "saved window or application rules changed after the filter was recreated");
	obs_frontend_open_source_filters(source);
	QPointer<QDialog> dialog;
	const bool opened = until(source, [&] {
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			auto *candidate = qobject_cast<QDialog *>(widget);
			if (candidate && candidate->isVisible() &&
			    candidate->windowTitle().contains("Scoped Auto Frame capture"))
				dialog = candidate;
		}
		return dialog && choice(dialog, "Program or window");
	});
	expect(opened, "the reloaded scoped filter did not open its actual Filters window");
	auto saved_button = [&](const QString &text) -> QPushButton * {
		if (dialog)
			for (auto *row : dialog->findChildren<QPushButton *>())
				if (row->isVisible() && row->text().contains(text))
					return row;
		return nullptr;
	};
	if (opened) {
		expect(until(source,
			     [&] {
				     return saved_button("Window: \"Scope, Demo; = \\\"Alpha\\\"") &&
					    saved_button("Window: \"Scope, Demo; = \\\"Beta\\\"") &&
					    saved_button("Application: \"shared-emulator.exe\"") &&
					    saved_button("Application: \"other-emulator.exe\"");
			     }),
		       "scoped application and full window identities were not visible as removable rows");
		if (auto *remove = saved_button("Window: \"Scope, Demo; = \\\"Alpha\\\""))
			remove->click();
		expect(until(source, [&] { return count() == 3; }) && applied(first_title, app, 3.0 / 2.0) &&
			       applied(second_title, app, 16.0 / 9.0),
		       "removing a window row did not reveal its application fallback or changed another window");
		if (auto *remove = saved_button("Application: \"shared-emulator.exe\""))
			remove->click();
		expect(until(source, [&] { return count() == 2; }) && applied("Unknown game", app, 1.0) &&
			       applied(second_title, app, 16.0 / 9.0) && applied(first_title, other_app, 5.0 / 4.0),
		       "removing an application row changed its window override or another application's default");
		if (auto *remove = saved_button("Window: \"Scope, Demo; = \\\"Beta\\\""))
			remove->click();
		expect(until(source, [&] { return count() == 1; }),
		       "removing the remaining window row changed another application's rule");
		if (auto *remove = saved_button("Application: \"other-emulator.exe\""))
			remove->click();
		expect(until(source, [&] { return count() == 0; }) && rules(filter) == legacy_rules,
		       "removing scoped rows deleted legacy rules or left hidden scoped entries");
		dialog->close();
		until(nullptr, [&] { return !dialog || !dialog->isVisible(); });
	}
	if (!reload()) {
		expect(false, "could not reload after deleting scoped rules");
		obs_source_dec_showing(source);
		obs_source_remove(source);
		return;
	}
	expect(count() == 0 && applied(first_title, app, 1.0), "deleted scoped rules returned after reload");
	// Leave representative application and window rows in a real screenshot.
	save("Application default", 360, 270, "application", app);
	save("Portrait Demo", 300, 270, "window", app);
	save("Wide Demo", 480, 270, "window", app);
	const QString capture = qEnvironmentVariable("STAGEKIT_AUTO_FRAME_CAPTURE");
	if (!capture.isEmpty()) {
		obs_frontend_open_source_filters(source);
		expect(until(source,
			     [&] {
				     for (QWidget *widget : QApplication::topLevelWidgets()) {
					     auto *candidate = qobject_cast<QDialog *>(widget);
					     if (candidate && candidate->isVisible() &&
						 candidate->windowTitle().contains("Scoped Auto Frame capture"))
						     dialog = candidate;
				     }
				     return saved_button("Window: \"Wide Demo\"") &&
					    saved_button("Window: \"Portrait Demo\"") &&
					    saved_button("Application: \"shared-emulator.exe\"");
			     }),
		       "the representative scoped rules were not visible for the Filters capture");
		if (dialog) {
			dialog->resize(1000, 960);
			for (auto *splitter : dialog->findChildren<QSplitter *>())
				if (splitter->orientation() == Qt::Vertical)
					splitter->setSizes({150, 720});
			int frames = 0;
			until(source, [&] { return ++frames >= 3; });
			for (auto *group : dialog->findChildren<QGroupBox *>())
				if (group->title() == "Saved app/window ratios")
					for (auto *area : dialog->findChildren<QScrollArea *>())
						if (area->widget() && area->widget()->isAncestorOf(group))
							area->ensureWidgetVisible(group, 20, 20);
			QCoreApplication::processEvents();
			expect(dialog->grab().save(QFileInfo(capture).absolutePath() + "/auto-frame-scopes.png"),
			       "could not capture the actual application/window rule controls");
		}
	}
	if (dialog) {
		dialog->close();
		until(nullptr, [&] { return !dialog || !dialog->isVisible(); });
	}
	obs_source_dec_showing(source);
	obs_source_filter_remove(source, filter);
	obs_source_remove(source);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	if (issues.size() == first_issue)
		blog(LOG_INFO,
		     "[Auto Frame detection test] PASS: explicit application/window scope, same-app window precedence, cross-app title isolation, delimiter-safe persistence, actual row deletion and rejection safeguards");
}

inline void check(QStringList &issues)
{
	const qsizetype first_issue = issues.size();
	auto expect = [&](bool value, const QString &message) {
		if (!value)
			issues.append(QStringLiteral("Auto Frame detection: ") + message);
	};
	register_capture();
	OBSDataAutoRelease capture_settings = obs_data_create();
	obs_data_set_int(capture_settings, "width", 480);
	obs_data_set_int(capture_settings, "height", 270);
	obs_data_set_int(capture_settings, "picture_width", 300);
	obs_data_set_int(capture_settings, "picture_height", 270);
	obs_data_set_bool(capture_settings, "hooked", true);
	obs_data_set_string(capture_settings, "title", "Portrait Demo");
	obs_data_set_string(capture_settings, "executable", "example-emulator.exe");
	obs_data_set_string(capture_settings, "window", "Stale Target:WindowClass:stale-target.exe");
	OBSSourceAutoRelease source = obs_source_create("stagekit_test_auto_frame_detection",
							"Auto Frame detection fixture", capture_settings, nullptr);
	OBSDataAutoRelease frame_settings = obs_data_create();
	obs_data_set_int(frame_settings, "frame_width", 480);
	obs_data_set_int(frame_settings, "frame_height", 270);
	obs_data_set_int(frame_settings, "glide_ms", 0);
	obs_data_set_double(frame_settings, "trim_wait", 0.0);
	obs_data_set_bool(frame_settings, "known_programs", false);
	obs_data_set_string(frame_settings, "rules_text", "kept game = 3:2");
	OBSSourceAutoRelease filter = obs_source_create_private("stagekit_auto_frame", "Auto Frame", frame_settings);
	if (!source || !filter) {
		expect(false, "could not create the capture and filter");
		return;
	}
	obs_source_filter_add(source, filter);
	obs_source_inc_showing(source);
	const std::string original_rules = rules(filter);
	// The disposable OBS profile opens its first-run wizard. Cancel only that
	// identified wizard so it cannot keep the actual Filters window modal-blocked.
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		auto *wizard = qobject_cast<QWizard *>(widget);
		if (wizard && wizard->isVisible() && QByteArray(wizard->metaObject()->className()) == "AutoConfig" &&
		    wizard->findChild<QRadioButton *>("prioritizeStreaming")) {
			wizard->reject();
			blog(LOG_INFO, "[Auto Frame detection test] Dismissed OBS first-run Auto-Configuration Wizard");
		}
	}
	obs_frontend_open_source_filters(source);
	QPointer<QDialog> dialog;
	const bool opened = until(source, [&] {
		for (QWidget *widget : QApplication::topLevelWidgets()) {
			auto *candidate = qobject_cast<QDialog *>(widget);
			if (candidate && candidate->isVisible() &&
			    candidate->windowTitle().contains("Auto Frame detection fixture"))
				dialog = candidate;
		}
		return dialog && choice(dialog, "Program or window") && choice(dialog, "Its aspect ratio");
	});
	expect(opened, "the real Filters window did not expose the program and ratio controls");
	auto draft_is = [&](const QString &program, double aspect) {
		auto *program_box = choice(dialog, "Program or window");
		auto *ratio_box = choice(dialog, "Its aspect ratio");
		return program_box && ratio_box && program_box->currentText() == program &&
		       std::abs(ratio(ratio_box->currentText()) - aspect) < 0.006;
	};
	auto edit_draft = [&](const QString &program, const QString &shape) {
		if (auto *box = choice(dialog, "Program or window"))
			box->setEditText(program);
		QCoreApplication::processEvents();
		if (auto *box = choice(dialog, "Its aspect ratio"))
			box->setEditText(shape);
	};
	auto measure_button = [&] {
		auto *measure = button(dialog, "Measure the bars now");
		if (measure)
			measure->click();
		return measure != nullptr;
	};
	if (opened) {
		expect(until(source,
			     [&] {
				     return draft_is("Portrait Demo", 10.0 / 9.0) && detection(dialog).contains("10:9");
			     }),
		       "automatic detection did not fill the captured window and 10:9 ratio");
		blog(LOG_INFO, "[Auto Frame detection test] First target: %s", detection(dialog).toUtf8().constData());
		aim(source, "Landscape Demo", 360, 270);
		expect(until(source,
			     [&] {
				     return draft_is("Landscape Demo", 4.0 / 3.0) && detection(dialog).contains("4:3");
			     }),
		       "two windows sharing an executable did not receive separate current drafts");
		edit_draft("Manual Draft", "13:7");
		expect(until(source, [&] { return draft_is("Manual Draft", 13.0 / 7.0); }),
		       "manual draft entry was not retained");
		auto *ratio_box = choice(dialog, "Its aspect ratio");
		QPointer<QLineEdit> editor = ratio_box ? ratio_box->lineEdit() : nullptr;
		auto editor_has_focus = [&] {
			auto *current_box = choice(dialog, "Its aspect ratio");
			auto *focused = QApplication::focusWidget();
			// Editable QComboBox delegates the line edit's focus to the combo
			// itself, then forwards focus and keyboard events to the editor.
			return editor && current_box && current_box->lineEdit() == editor &&
			       (focused == editor || (editor->focusProxy() && focused == editor->focusProxy()));
		};
		auto focus_state = [&] {
			auto *current_box = choice(dialog, "Its aspect ratio");
			auto *focused = QApplication::focusWidget();
			auto *proxy = editor ? editor->focusProxy() : nullptr;
			auto widget_name = [](QWidget *widget) {
				return widget ? QString::fromLatin1(widget->metaObject()->className()) + '/' +
							widget->objectName()
					      : QStringLiteral("none");
			};
			return QStringLiteral("active dialog=%1, editor alive=%2, same editor=%3, focus=%4, proxy=%5, "
					      "caret=%6")
				.arg(dialog && QApplication::activeWindow() == dialog ? "yes" : "no",
				     editor ? "yes" : "no",
				     editor && current_box && current_box->lineEdit() == editor ? "yes" : "no",
				     widget_name(focused), widget_name(proxy),
				     editor ? QString::number(editor->cursorPosition()) : QStringLiteral("none"));
		};
		if (editor) {
			// CI uses a hidden Windows session or Xvfb without a window manager.
			// Establish Qt activation and focus events independently of the OS.
			QT_WARNING_PUSH
			QT_WARNING_DISABLE_DEPRECATED
			QApplication::setActiveWindow(dialog);
			QT_WARNING_POP
			editor->setFocus(Qt::OtherFocusReason);
		}
		const bool typing_focus = until(source, editor_has_focus);
		expect(typing_focus,
		       QStringLiteral("the editable ratio could not receive typing focus (%1)").arg(focus_state()));
		if (editor)
			editor->setCursorPosition(2);
		aim(source, "Wide Demo", 480, 270);
		expect(until(source, [&] { return detection(dialog).contains("16:9"); }),
		       "measurement feedback did not follow the current image while a draft was edited");
		expect(draft_is("Manual Draft", 13.0 / 7.0), "automatic refresh overwrote the manually edited draft");
		expect(editor_has_focus() && editor->cursorPosition() == 2,
		       QStringLiteral("live detection rebuilt the active editor or moved its typing caret (%1)")
			       .arg(focus_state()));
		expect(measure_button(), "Measure the bars now button was missing");
		expect(until(source,
			     [&] {
				     OBSDataAutoRelease settings = obs_source_get_settings(filter);
				     return obs_data_get_int(settings, "trim") == 2 &&
					    draft_is("Wide Demo", 16.0 / 9.0) && detection(dialog).contains("16:9");
			     }),
		       "Measure did not publish a fresh ratio or retain the visible program editor in manual trim");
		expect(rules(filter) == original_rules, "measuring or filling a draft saved a rule without Remember");

		const QString capture = qEnvironmentVariable("STAGEKIT_AUTO_FRAME_CAPTURE");
		if (!capture.isEmpty()) {
			dialog->resize(900, 960);
			for (auto *splitter : dialog->findChildren<QSplitter *>())
				if (splitter->orientation() == Qt::Vertical)
					splitter->setSizes({170, 720});
			if (auto *shape = choice(dialog, "Its aspect ratio"))
				for (auto *area : dialog->findChildren<QScrollArea *>())
					if (area->widget() && area->widget()->isAncestorOf(shape))
						area->ensureWidgetVisible(shape, 20, 20);
			QCoreApplication::processEvents();
			expect(dialog->grab().save(capture), "could not capture the actual filter controls");
		}
	}

	// The real websocket vendor path, as all three new Stream Deck keys use it.
	aim(source, "Custom Demo; Variant", 390, 210, 480, 280);
	auto measured = request(source, filter, "measure");
	const long long measure_id = obs_data_get_int(measured, "requestId");
	expect(obs_data_get_bool(measured, "success") && measure_id > 0 &&
		       std::string(obs_data_get_string(measured, "status")) == "pending" &&
		       !obs_data_get_bool(measured, "complete"),
	       "Measure did not return a pending request identifier");
	expect(until(
		       source,
		       [&] {
			       measured = request(source, filter, nullptr, measure_id);
			       return obs_data_get_bool(measured, "complete");
		       },
		       5000),
	       "the custom-ratio Measure request did not finish");
	expect(std::string(obs_data_get_string(measured, "status")) == "measured" &&
		       std::string(obs_data_get_string(measured, "program")) == "Custom Demo; Variant" &&
		       std::string(obs_data_get_string(measured, "executable")) == "example-emulator.exe" &&
		       std::abs(obs_data_get_double(measured, "ratio") - 13.0 / 7.0) < 0.006 &&
		       std::abs(obs_data_get_double(measured, "width") - 390) <= 2 &&
		       std::abs(obs_data_get_double(measured, "height") - 210) <= 2,
	       QStringLiteral("fresh custom dimensions or captured identity were wrong: %1")
		       .arg(QString::fromUtf8(obs_data_get_json(measured))));
	expect(rules(filter) == original_rules, "Measure saved its result without a Remember command");
	if (opened)
		edit_draft("Unrelated Draft", "1:1");
	auto remembered = request(source, filter, "remember");
	const long long remember_id = obs_data_get_int(remembered, "requestId");
	expect(obs_data_get_bool(remembered, "complete") ||
		       until(
			       source,
			       [&] {
				       remembered = request(source, filter, nullptr, remember_id);
				       return obs_data_get_bool(remembered, "complete");
			       },
			       5000),
	       "Remember did not finish saving the current detection");
	expect(obs_data_get_bool(remembered, "success") && obs_data_get_bool(remembered, "complete") &&
		       obs_data_get_bool(remembered, "saved") &&
		       std::string(obs_data_get_string(remembered, "program")) == "Custom Demo; Variant" &&
		       rules(filter).find("custom demo = ") != std::string::npos &&
		       rules(filter).find("variant") == std::string::npos &&
		       rules(filter).find("unrelated draft") == std::string::npos &&
		       rules(filter).find(original_rules) != std::string::npos,
	       "Remember did not save the actual current detection while preserving previous rules");
	const std::string before_stale = rules(filter);
	obs_enter_graphics();
	aim(source, "Remember Changed", 360, 270);
	auto stale_remember = request(source, filter, "remember");
	obs_leave_graphics();
	expect(obs_data_get_bool(stale_remember, "complete") && !obs_data_get_bool(stale_remember, "success") &&
		       std::string(obs_data_get_string(stale_remember, "status")) == "failed" &&
		       !obs_data_get_bool(stale_remember, "saved") && rules(filter) == before_stale,
	       "Remember saved the previous window's ratio after capture switched");

	aim(source, "Combined Demo", 300, 270);
	const std::string before_combined = rules(filter);
	auto combined = request(source, filter, "measure_and_remember");
	const long long combined_id = obs_data_get_int(combined, "requestId");
	expect(obs_data_get_bool(combined, "success") && !obs_data_get_bool(combined, "complete") &&
		       !obs_data_get_bool(combined, "saved") && combined_id > measure_id &&
		       rules(filter) == before_combined,
	       "combined command reported a save before completing a fresh measurement");
	expect(until(
		       source,
		       [&] {
			       combined = request(source, filter, nullptr, combined_id);
			       return obs_data_get_bool(combined, "complete");
		       },
		       5000),
	       "combined measurement and save did not finish");
	expect(std::string(obs_data_get_string(combined, "status")) == "saved" &&
		       obs_data_get_bool(combined, "saved") &&
		       std::string(obs_data_get_string(combined, "program")) == "Combined Demo" &&
		       std::abs(obs_data_get_double(combined, "ratio") - 10.0 / 9.0) < 0.006 &&
		       rules(filter).find("combined demo = 10:9") != std::string::npos,
	       QStringLiteral("combined command saved the wrong measurement: %1")
		       .arg(QString::fromUtf8(obs_data_get_json(combined))));
	int saved_frames = 0;
	until(source, [&] { return ++saved_frames >= 6; });
	if (opened)
		expect(draft_is("Combined Demo", 10.0 / 9.0) && detection(dialog).contains("10:9"),
		       "applying a saved rule discarded the measured ratio label or current draft");
	auto current = request(source, filter);
	expect(obs_data_get_bool(current, "success") && obs_data_get_int(current, "requestId") == combined_id &&
		       std::string(obs_data_get_string(current, "status")) == "saved",
	       "GetAutoFrameState without an ID did not return the current request");
	auto replaced = request(source, filter, nullptr, measure_id);
	expect(!obs_data_get_bool(replaced, "success") && obs_data_get_bool(replaced, "complete") &&
		       std::string(obs_data_get_string(replaced, "status")) == "failed" &&
		       !obs_data_get_bool(replaced, "saved"),
	       "a replaced request inherited a later command's successful save");

	const std::string before_disabled = rules(filter);
	obs_source_set_enabled(filter, false);
	// Remember goes first while a valid detection still exists. Disabling the
	// filter must reject it instead of silently saving that previous result.
	for (const auto *action : {"remember", "measure", "measure_and_remember"}) {
		auto disabled = request(source, filter, action);
		expect(!obs_data_get_bool(disabled, "success") && obs_data_get_bool(disabled, "complete") &&
			       std::string(obs_data_get_string(disabled, "status")) == "failed" &&
			       !obs_data_get_bool(disabled, "saved") && *obs_data_get_string(disabled, "error") &&
			       rules(filter) == before_disabled,
		       QStringLiteral("disabled Auto Frame accepted %1").arg(action));
	}
	obs_source_set_enabled(filter, true);
	obs_enter_graphics();
	aim(source, "Disabled Pending Demo", 300, 270);
	auto disabled_pending = request(source, filter, "measure_and_remember");
	const long long disabled_id = obs_data_get_int(disabled_pending, "requestId");
	expect(obs_data_get_bool(disabled_pending, "success") && !obs_data_get_bool(disabled_pending, "complete"),
	       "the disable-during-measurement fixture did not start a pending command");
	obs_source_set_enabled(filter, false);
	obs_leave_graphics();
	expect(until(source,
		     [&] {
			     disabled_pending = request(source, filter, nullptr, disabled_id);
			     return obs_data_get_bool(disabled_pending, "complete");
		     }),
	       "disabling Auto Frame did not cancel its pending combined command");
	expect(!obs_data_get_bool(disabled_pending, "success") &&
		       std::string(obs_data_get_string(disabled_pending, "status")) == "failed" &&
		       !obs_data_get_bool(disabled_pending, "saved") && rules(filter) == before_disabled,
	       "disabling Auto Frame saved its pending combined command");
	obs_source_set_enabled(filter, true);
	int enabled_frames = 0;
	until(source, [&] { return ++enabled_frames >= 4; });
	auto enabled_again = request(source, filter, nullptr, disabled_id);
	expect(std::string(obs_data_get_string(enabled_again, "status")) == "failed" &&
		       !obs_data_get_bool(enabled_again, "saved") && rules(filter) == before_disabled,
	       "reenabling Auto Frame saved a command canceled while disabled");

	const std::string before_failures = rules(filter);
	// Prevent a render between starting the command and changing its target.
	obs_enter_graphics();
	aim(source, "Changing First", 300, 270);
	auto changed = request(source, filter, "measure_and_remember");
	const long long changed_id = obs_data_get_int(changed, "requestId");
	aim(source, "Changing Second", 360, 270);
	obs_leave_graphics();
	expect(until(
		       source,
		       [&] {
			       changed = request(source, filter, nullptr, changed_id);
			       return obs_data_get_bool(changed, "complete");
		       },
		       5000),
	       "a target change left the combined command pending");
	expect(std::string(obs_data_get_string(changed, "status")) == "failed" &&
		       !obs_data_get_bool(changed, "saved") && rules(filter) == before_failures,
	       "a combined command saved a ratio for a different capture target");
	aim(source, "Dark Demo", 0, 0);
	auto dark = request(source, filter, "measure_and_remember");
	const long long dark_id = obs_data_get_int(dark, "requestId");
	expect(until(
		       source,
		       [&] {
			       dark = request(source, filter, nullptr, dark_id);
			       return obs_data_get_bool(dark, "complete");
		       },
		       5000),
	       "a fully dark image left the combined command pending indefinitely");
	expect(std::string(obs_data_get_string(dark, "status")) == "failed" && !obs_data_get_bool(dark, "saved") &&
		       rules(filter) == before_failures,
	       "a dark frame produced or saved a guessed ratio");
	aim(source, "", 0, 0, 0, 0, false);
	auto unavailable = request(source, filter, "measure_and_remember");
	const long long unavailable_id = obs_data_get_int(unavailable, "requestId");
	expect(obs_data_get_bool(unavailable, "complete") ||
		       until(
			       source,
			       [&] {
				       unavailable = request(source, filter, nullptr, unavailable_id);
				       return obs_data_get_bool(unavailable, "complete");
			       },
			       5000),
	       "an unhooked capture left its command pending");
	expect(std::string(obs_data_get_string(unavailable, "status")) == "failed" &&
		       !obs_data_get_bool(unavailable, "success") && !obs_data_get_bool(unavailable, "saved") &&
		       *obs_data_get_string(unavailable, "error") && rules(filter) == before_failures,
	       "an unavailable capture used stale window metadata or saved a ratio");
	if (opened)
		expect(until(source, [&] { return detection(dialog).isEmpty(); }),
		       "an unavailable capture still displayed a fresh measured ratio");

	for (int invalid_id = 0; invalid_id < 6; ++invalid_id) {
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "sourceName", obs_source_get_name(source));
		obs_data_set_string(data, "filterName", obs_source_get_name(filter));
		switch (invalid_id) {
		case 0:
			obs_data_set_string(data, "requestId", "1");
			break;
		case 1:
			obs_data_set_double(data, "requestId", 1.5);
			break;
		case 2:
			obs_data_set_int(data, "requestId", 0);
			break;
		case 3:
			obs_data_set_int(data, "requestId", -1);
			break;
		case 4:
			obs_data_set_int(data, "requestId", 9007199254740992LL);
			break;
		case 5:
			obs_data_set_bool(data, "requestId", true);
			break;
		}
		auto invalid = vendor("GetAutoFrameState", data);
		expect(!obs_data_get_bool(invalid, "success") &&
			       std::string(obs_data_get_string(invalid, "error")).find("requestId") !=
				       std::string::npos,
		       QStringLiteral("GetAutoFrameState accepted malformed request ID case %1").arg(invalid_id));
	}
	OBSDataAutoRelease missing = obs_data_create();
	obs_data_set_string(missing, "sourceName", "Missing Auto Frame capture");
	obs_data_set_string(missing, "filterName", obs_source_get_name(filter));
	obs_data_set_string(missing, "action", "measure");
	auto missing_result = vendor("AutoFrameCommand", missing);
	expect(!obs_data_get_bool(missing_result, "success") && *obs_data_get_string(missing_result, "error"),
	       "AutoFrameCommand accepted a missing source");
	OBSSourceAutoRelease wrong_filter =
		obs_source_create_private("stagekit_ramp_blur", "Wrong filter type", nullptr);
	expect(wrong_filter != nullptr, "could not create the wrong-type filter validation fixture");
	if (wrong_filter) {
		obs_source_filter_add(source, wrong_filter);
		auto wrong_result = request(source, wrong_filter, "measure");
		expect(!obs_data_get_bool(wrong_result, "success") &&
			       std::string(obs_data_get_string(wrong_result, "error")).find("Auto Frame") !=
				       std::string::npos,
		       "AutoFrameCommand accepted a filter of a different type");
		obs_source_filter_remove(source, wrong_filter);
	}
	if (dialog) {
		dialog->close();
		until(nullptr, [&] { return !dialog || !dialog->isVisible(); });
	}
	obs_source_dec_showing(source);
	// Nothing displays this source once its Filters preview is closed. A valid
	// capture that stops rendering must time out, and resuming it must not save
	// an old combined command after the Stream Deck key has already failed.
	aim(source, "Timed Out Demo", 300, 270);
	auto timed_out = request(source, filter, "measure_and_remember");
	const long long timeout_id = obs_data_get_int(timed_out, "requestId");
	expect(obs_data_get_bool(timed_out, "success") && !obs_data_get_bool(timed_out, "complete"),
	       "a hooked capture without rendered frames did not begin a pending measurement");
	expect(until(
		       nullptr,
		       [&] {
			       timed_out = request(source, filter, nullptr, timeout_id);
			       return obs_data_get_bool(timed_out, "complete");
		       },
		       5000),
	       "a capture that stopped rendering did not time out before the Stream Deck deadline");
	expect(!obs_data_get_bool(timed_out, "success") &&
		       std::string(obs_data_get_string(timed_out, "status")) == "failed" &&
		       !obs_data_get_bool(timed_out, "saved") && *obs_data_get_string(timed_out, "error") &&
		       rules(filter) == before_failures,
	       "a timed-out combined command saved an unmeasured ratio");
	obs_source_inc_showing(source);
	int resumed_frames = 0;
	until(source, [&] { return ++resumed_frames >= 4; });
	auto resumed = request(source, filter, nullptr, timeout_id);
	expect(std::string(obs_data_get_string(resumed, "status")) == "failed" &&
		       !obs_data_get_bool(resumed, "saved") && rules(filter) == before_failures,
	       "resuming capture saved a combined command after its timeout");
	obs_source_dec_showing(source);
	// A slow capability reply leaves less than four seconds for native work.
	// Keep this capture unrendered until its reduced budget has expired, then
	// expose a valid image without changing its identity or source dimensions.
	aim(source, "Reduced Budget Demo", 0, 0);
	QElapsedTimer reduced_timer;
	reduced_timer.start();
	auto reduced = request(source, filter, "measure_and_remember", 0, "window", 250);
	const long long reduced_id = obs_data_get_int(reduced, "requestId");
	expect(obs_data_get_bool(reduced, "success") && !obs_data_get_bool(reduced, "complete"),
	       "the reduced-budget capture did not begin a pending measurement");
	expect(until(
		       nullptr,
		       [&] {
			       reduced = request(source, filter, nullptr, reduced_id);
			       return obs_data_get_bool(reduced, "complete");
		       },
		       1500),
	       "the reduced native budget was replaced by the legacy four-second deadline");
	expect(reduced_timer.elapsed() >= 225 && reduced_timer.elapsed() < 1500 &&
		       !obs_data_get_bool(reduced, "success") &&
		       std::string(obs_data_get_string(reduced, "status")) == "failed" &&
		       !obs_data_get_bool(reduced, "saved") &&
		       std::string(obs_data_get_string(reduced, "rememberBy")) == "window" &&
		       std::string(obs_data_get_string(reduced, "rememberTarget")) == "Reduced Budget Demo",
	       QStringLiteral("a 250 ms scoped timeout did not expire with its original identity (%1 ms): %2")
		       .arg(reduced_timer.elapsed())
		       .arg(QString::fromUtf8(obs_data_get_json(reduced))));
	aim(source, "Reduced Budget Demo", 300, 270);
	obs_source_inc_showing(source);
	int reduced_frames = 0;
	until(source, [&] { return ++reduced_frames >= 6; });
	auto reduced_resumed = request(source, filter, nullptr, reduced_id);
	OBSDataAutoRelease reduced_settings = obs_source_get_settings(filter);
	OBSDataArrayAutoRelease reduced_rules = obs_data_get_array(reduced_settings, "scoped_rules");
	expect(!obs_data_get_bool(reduced_resumed, "success") &&
		       std::string(obs_data_get_string(reduced_resumed, "status")) == "failed" &&
		       !obs_data_get_bool(reduced_resumed, "saved") && obs_data_array_count(reduced_rules) == 0 &&
		       rules(filter) == before_failures,
	       "a fresh image saved a scoped ratio after the client's reduced native budget expired");
	obs_source_dec_showing(source);
	obs_source_filter_remove(source, filter);
	obs_source_remove(source);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	// Window captures without get_hooked store an escaped title. Saving its
	// readable title must still match that capture when the saved rule is used.
	obs_data_set_bool(capture_settings, "without_hook", true);
	obs_data_set_string(capture_settings, "window", "Fallback#3A Demo:WindowClass:example-emulator.exe");
	OBSSourceAutoRelease fallback_source = obs_source_create("stagekit_test_auto_frame_detection",
								 "Fallback window fixture", capture_settings, nullptr);
	OBSSourceAutoRelease fallback_filter =
		obs_source_create_private("stagekit_auto_frame", "Fallback Auto Frame", frame_settings);
	expect(fallback_source && fallback_filter, "could not create the encoded-window fixture");
	if (fallback_source && fallback_filter) {
		obs_source_filter_add(fallback_source, fallback_filter);
		obs_source_inc_showing(fallback_source);
		auto fallback = request(fallback_source, fallback_filter, "measure_and_remember");
		const long long fallback_id = obs_data_get_int(fallback, "requestId");
		expect(until(fallback_source,
			     [&] {
				     fallback = request(fallback_source, fallback_filter, nullptr, fallback_id);
				     return obs_data_get_bool(fallback, "complete");
			     }),
		       "the encoded-window measurement did not finish");
		expect(obs_data_get_bool(fallback, "saved") &&
			       std::string(obs_data_get_string(fallback, "program")) == "Fallback: Demo" &&
			       rules(fallback_filter).find("fallback: demo = 10:9") != std::string::npos,
		       "the stored window title was not decoded before saving its ratio");
		expect(until(fallback_source,
			     [&] {
				     auto *properties = obs_source_properties(fallback_filter);
				     auto *status = properties ? obs_properties_get(properties, "status") : nullptr;
				     const bool matched = status &&
							  std::string(obs_property_description(status))
									  .find("from your saved aspect ratios") !=
								  std::string::npos;
				     obs_properties_destroy(properties);
				     return matched;
			     }),
		       "the decoded saved title did not match the encoded capture setting");
		obs_source_dec_showing(fallback_source);
		obs_source_filter_remove(fallback_source, fallback_filter);
		obs_source_remove(fallback_source);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}
	check_scoped_rules(issues);
	if (issues.size() == first_issue)
		blog(LOG_INFO,
		     "[Auto Frame detection test] PASS: live draft and feedback, manual edits, real measure/remember/combined requests, custom ratio and stale/empty capture rejection");
}

} // namespace auto_frame_detection_test
