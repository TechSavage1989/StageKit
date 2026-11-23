#pragma once

#include <obs.hpp>
#include <graphics/vec2.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

// Public requests and real OBS items exercise the same model on Windows and
// in the headless renderer. The fixture does not link to StageKit internals.
namespace scene_controller_test {

template<typename Predicate> bool until(Predicate predicate, int timeout_ms = 4000)
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

inline void settle(int milliseconds = 160)
{
	QElapsedTimer elapsed;
	elapsed.start();
	until([&] { return elapsed.elapsed() >= milliseconds; }, milliseconds + 100);
}

inline OBSDataAutoRelease request(const char *name, obs_data_t *data)
{
	calldata_t call{};
	calldata_set_string(&call, "request", name);
	calldata_set_string(&call, "data", data ? obs_data_get_json(data) : "{}");
	proc_handler_call(obs_get_proc_handler(), "stagekit_vendor_call", &call);
	const char *json = calldata_string(&call, "response");
	OBSDataAutoRelease result = obs_data_create_from_json(json && *json ? json : "{}");
	calldata_free(&call);
	return result;
}

inline OBSDataAutoRelease get(const std::string &id)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "controllerId", id.c_str());
	return request("GetSceneController", data);
}

inline OBSDataAutoRelease row(obs_data_t *data, const char *array_name, const std::string &id)
{
	OBSDataArrayAutoRelease rows = obs_data_get_array(data, array_name);
	for (size_t i = 0; i < obs_data_array_count(rows); ++i) {
		OBSDataAutoRelease item = obs_data_array_item(rows, i);
		if (id == obs_data_get_string(item, "id"))
			return item;
	}
	return nullptr;
}

inline std::string current(const std::string &id, const char *group)
{
	OBSDataAutoRelease state = get(id);
	OBSDataAutoRelease found = row(state, "groups", group);
	return obs_data_get_string(found, "current");
}

inline std::string choice(const std::string &id, const char *group, const char *name)
{
	OBSDataAutoRelease state = get(id);
	OBSDataAutoRelease found = row(state, "groups", group);
	OBSDataArrayAutoRelease choices = obs_data_get_array(found, "choices");
	for (size_t i = 0; i < obs_data_array_count(choices); ++i) {
		OBSDataAutoRelease item = obs_data_array_item(choices, i);
		if (std::strcmp(obs_data_get_string(item, "name"), name) == 0)
			return obs_data_get_string(item, "id");
	}
	return {};
}

inline bool press(const std::string &id, const char *group, const char *choice_id, const char *view = "",
		  const char *command = "select")
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "controllerId", id.c_str());
	obs_data_set_string(data, "groupId", group);
	obs_data_set_string(data, "choiceId", choice_id);
	obs_data_set_string(data, "viewId", view);
	obs_data_set_string(data, "command", command);
	OBSDataAutoRelease response = request("SceneControllerCommand", data);
	return obs_data_get_bool(response, "success");
}

inline bool configure(const std::string &id, obs_data_t *config)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "controllerId", id.c_str());
	obs_data_set_obj(data, "config", config);
	OBSDataAutoRelease response = request("ConfigureSceneController", data);
	return obs_data_get_bool(response, "success");
}

inline void list(obs_data_t *data, const std::string &name, const std::vector<std::string> &values)
{
	OBSDataArrayAutoRelease rows = obs_data_array_create();
	for (const auto &value : values) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "value", value.c_str());
		obs_data_array_push_back(rows, item);
	}
	obs_data_set_array(data, name.c_str(), rows);
}

inline OBSDataAutoRelease modes(const std::string &first, const std::string &second)
{
	OBSDataAutoRelease data = obs_data_create();
	list(data, "modes", {"front", "detail"});
	obs_data_set_string(data, "mode|front|name", "Main view");
	obs_data_set_string(data, "mode|detail|name", "Detail view");
	list(data, "mode|front|sources", {first});
	list(data, "mode|detail|sources", {second});
	obs_data_set_string(data, "mode_now", "front");
	obs_data_set_bool(data, "allow_none", true);
	obs_data_set_string(data, "transition", "none");
	obs_data_set_string(data, "hide_transition", "none");
	return data;
}

inline void add_group(obs_data_array_t *groups, const char *id, const char *name, const char *kind,
		      obs_data_t *settings, const char *shared = "")
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "id", id);
	obs_data_set_string(data, "name", name);
	obs_data_set_string(data, "kind", kind);
	obs_data_set_bool(data, "enabled", true);
	obs_data_set_obj(data, "settings", settings);
	if (*shared)
		list(data, "scope", {shared});
	obs_data_array_push_back(groups, data);
}

inline void add_view(obs_data_array_t *views, const char *id, const char *name,
		     const std::vector<std::pair<std::string, std::string>> &selections, const char *scene = "")
{
	OBSDataAutoRelease data = obs_data_create();
	OBSDataArrayAutoRelease rows = obs_data_array_create();
	for (const auto &[group, selected] : selections) {
		OBSDataAutoRelease item = obs_data_create();
		obs_data_set_string(item, "groupId", group.c_str());
		obs_data_set_string(item, "choiceId", selected.c_str());
		obs_data_array_push_back(rows, item);
	}
	obs_data_set_string(data, "id", id);
	obs_data_set_string(data, "name", name);
	obs_data_set_string(data, "type", "scene");
	obs_data_set_string(data, "scene", scene);
	obs_data_set_array(data, "selections", rows);
	obs_data_array_push_back(views, data);
}

inline void register_item()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	obs_source_info info{};
	info.id = "stagekit_test_controller_item";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = [](void *) {
		return "Scene Controller test item";
	};
	info.create = [](obs_data_t *, obs_source_t *) -> void * {
		return new int(0);
	};
	info.destroy = [](void *data) {
		delete static_cast<int *>(data);
	};
	info.get_width = [](void *) -> uint32_t {
		return 320;
	};
	info.get_height = [](void *) -> uint32_t {
		return 180;
	};
	info.video_render = [](void *, gs_effect_t *) {
	};
	obs_register_source(&info);
}

struct Fixture {
	obs_scene_t *scene = nullptr, *shared = nullptr;
	obs_source_t *controller = nullptr;
	std::vector<obs_source_t *> sources;
	std::string id;

	Fixture()
	{
		register_item();
		scene = obs_scene_create("Controller test layout");
		shared = obs_scene_create("Controller test other layout");
		obs_source_inc_showing(obs_scene_get_source(scene));
		controller = obs_source_create("stagekit_scene_controller", "Test view controller", nullptr, nullptr);
		if (controller) {
			obs_source_filter_add(obs_scene_get_source(scene), controller);
			id = obs_source_get_uuid(controller);
		}
	}

	obs_sceneitem_t *add(const char *name, bool visible = false)
	{
		auto *source = obs_source_create("stagekit_test_controller_item", name, nullptr, nullptr);
		sources.push_back(source);
		auto *item = obs_scene_add(scene, source);
		obs_sceneitem_set_visible(item, visible);
		return item;
	}

	void remove_controller()
	{
		if (controller) {
			obs_source_filter_remove(obs_scene_get_source(scene), controller);
			obs_source_release(controller);
			controller = nullptr;
		}
	}

	~Fixture()
	{
		remove_controller();
		obs_source_dec_showing(obs_scene_get_source(scene));
		obs_source_remove(obs_scene_get_source(scene));
		obs_source_remove(obs_scene_get_source(shared));
		obs_scene_release(scene);
		obs_scene_release(shared);
		for (auto *source : sources) {
			obs_source_remove(source);
			obs_source_release(source);
		}
	}
};

inline void check(QStringList &issues)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("Scene Controller: ") + QString::fromUtf8(message));
	};
	Fixture f;
	if (!f.controller) {
		expect(false, "source type was not registered");
		return;
	}
	auto *main = f.add("Controller main picture");
	auto *detail = f.add("Controller detail picture");
	auto *camera_a = f.add("Controller camera A");
	auto *camera_b = f.add("Controller camera B");
	auto *overlay_a = f.add("Controller caption");
	auto *overlay_b = f.add("Controller badge");
	auto *frame = f.add("Controller camera frame");
	auto *unrelated = f.add("Controller unrelated", true);
	auto *shared_main = obs_scene_add(f.shared, obs_sceneitem_get_source(main));
	auto *shared_detail = obs_scene_add(f.shared, obs_sceneitem_get_source(detail));
	auto *nested = obs_scene_add_group(f.scene, "Controller camera group");
	obs_sceneitem_group_add_item(nested, camera_a);
	obs_sceneitem_group_add_item(nested, camera_b);

	OBSDataAutoRelease config = obs_data_create();
	obs_data_set_int(config, "schema", 1);
	OBSDataArrayAutoRelease groups = obs_data_array_create();
	OBSDataAutoRelease content = modes("Controller main picture", "Controller detail picture");
	add_group(groups, "content", "Main content", "modes", content, "Controller test other layout");
	OBSDataAutoRelease camera = obs_data_create();
	list(camera, "items", {"Controller camera A", "Controller camera B"});
	obs_data_set_string(camera, "transition", "none");
	add_group(groups, "camera", "Camera", "one", camera);
	OBSDataAutoRelease overlays = obs_data_create();
	list(overlays, "items", {"Controller caption", "Controller badge"});
	obs_data_set_string(overlays, "transition", "none");
	add_group(groups, "overlays", "Overlays", "together", overlays);
	OBSDataAutoRelease follow = obs_data_create();
	list(follow, "items", {"Controller camera frame"});
	list(follow, "leads", {"Controller camera A", "Controller camera B"});
	obs_data_set_int(follow, "lead_rule", 0);
	obs_data_set_string(follow, "transition", "none");
	add_group(groups, "frame", "Camera frame", "follow", follow);
	obs_data_set_array(config, "groups", groups);
	expect(configure(f.id, config), "configuration was rejected");
	expect(until([&] { return current(f.id, "content") == "front"; }), "initial mode was not restored");
	const std::string a = choice(f.id, "camera", "Controller camera A");
	const std::string b = choice(f.id, "camera", "Controller camera B");
	if (a.empty() || b.empty()) {
		expect(false, "source choices were not exposed with stable IDs");
		return;
	}
	expect(until([&] {
		       return obs_sceneitem_visible(main) && !obs_sceneitem_visible(detail) &&
			      obs_sceneitem_visible(shared_main) && !obs_sceneitem_visible(shared_detail);
	       }),
	       "mode selection did not cover the shared scene");
	expect(press(f.id, "camera", a.c_str()) && until([&] {
		       return obs_sceneitem_visible(camera_a) && !obs_sceneitem_visible(camera_b) &&
			      obs_sceneitem_visible(frame);
	       }),
	       "nested camera selection or Any lead follow failed");
	obs_sceneitem_set_visible(camera_b, true);
	expect(until([&] { return !obs_sceneitem_visible(camera_a) && obs_sceneitem_visible(camera_b); }),
	       "a nested source eye did not select its one-source group");
	expect(press(f.id, "camera", a.c_str()) && until([&] { return !obs_sceneitem_visible(camera_b); }),
	       "camera group did not return to its first choice");
	obs_sceneitem_set_visible(main, false);
	expect(press(f.id, "camera", "", "", "next") &&
		       until([&] { return !obs_sceneitem_visible(camera_a) && obs_sceneitem_visible(camera_b); }),
	       "Next did not select the next camera");
	settle();
	expect(current(f.id, "content") == "front" && !obs_sceneitem_visible(main) && obs_sceneitem_visible(unrelated),
	       "a camera change reset the content mode or a manual visibility choice");
	OBSSceneAutoRelease unrelated_scene = obs_scene_create("Controller unused layout");
	settle();
	expect(!obs_sceneitem_visible(main) && current(f.id, "content") == "front",
	       "creating an unrelated scene reset a manually hidden active member");
	obs_source_set_name(obs_scene_get_source(unrelated_scene), "Controller unused layout renamed");
	settle();
	expect(!obs_sceneitem_visible(main), "an unrelated scene rename reset a manual visibility choice");
	obs_source_remove(obs_scene_get_source(unrelated_scene));
	unrelated_scene = nullptr;
	settle();
	obs_sceneitem_set_visible(camera_b, false);
	expect(until([&] { return !obs_sceneitem_visible(frame); }), "Any lead follow did not hide its follower");
	expect(press(f.id, "overlays", "on") &&
		       until([&] { return obs_sceneitem_visible(overlay_a) && obs_sceneitem_visible(overlay_b); }),
	       "Together On did not show both items");
	obs_sceneitem_set_visible(overlay_a, false);
	expect(until([&] { return !obs_sceneitem_visible(overlay_b); }), "Together did not follow a source eye");

	OBSDataAutoRelease state = get(f.id);
	config = obs_data_get_obj(state, "config");
	OBSDataArrayAutoRelease views = obs_data_array_create();
	add_view(views, "main", "Main and camera A", {{"content", "front"}, {"camera", a}, {"overlays", "on"}});
	add_view(views, "detail", "Detail and camera B", {{"content", "detail"}, {"camera", b}});
	obs_data_set_array(config, "views", views);
	expect(configure(f.id, config), "combined views were rejected");
	expect(until([&] {
		       OBSDataAutoRelease next = get(f.id);
		       return bool(row(next, "views", "detail"));
	       }),
	       "combined views were not exposed");
	expect(press(f.id, "", "", "detail") && until([&] {
		       return current(f.id, "content") == "detail" && obs_sceneitem_visible(detail) &&
			      !obs_sceneitem_visible(main) && obs_sceneitem_visible(camera_b) &&
			      !obs_sceneitem_visible(camera_a);
	       }),
	       "combined view did not apply all selections");
	state = get(f.id);
	OBSDataAutoRelease active = row(state, "views", "detail");
	expect(obs_data_get_bool(active, "active"), "saved view did not report its actual active state");
	expect(!press(f.id, "camera", "missing") && !press(f.id, "", "", "missing") &&
		       !press(f.id, "camera", a.c_str(), "main"),
	       "invalid or ambiguous commands were accepted");
	settle();
	expect(current(f.id, "content") == "detail" && obs_sceneitem_visible(camera_b),
	       "a rejected command partially changed the selection");

	// Invalid configurations must be rejected before replacing the running model.
	OBSDataAutoRelease bad = obs_data_create_from_json(obs_data_get_json(config));
	OBSDataArrayAutoRelease bad_groups = obs_data_get_array(bad, "groups");
	OBSDataAutoRelease duplicate = obs_data_array_item(bad_groups, 1);
	obs_data_set_string(duplicate, "id", "content");
	expect(!configure(f.id, bad), "duplicate group IDs were accepted");
	expect(current(f.id, "content") == "detail", "rejected configuration replaced the active model");
	state = get(f.id);
	config = obs_data_get_obj(state, "config");
	OBSDataAutoRelease camera_group = row(config, "groups", "camera");
	obs_data_set_bool(camera_group, "enabled", false);
	expect(configure(f.id, config), "could not disable an independent group");
	settle();
	expect(!press(f.id, "", "", "main"), "combined view accepted a disabled member group");
	settle();
	expect(current(f.id, "content") == "detail", "rejected combined view partially changed another group");
	obs_data_set_bool(camera_group, "enabled", true);
	expect(configure(f.id, config), "could not restore the independent group");
	settle();
	obs_source_set_enabled(f.controller, false);
	settle();
	state = get(f.id);
	expect(!obs_data_get_bool(state, "enabled") && !press(f.id, "", "", "main"),
	       "disabled controller accepted a command or reported enabled");
	obs_source_set_enabled(f.controller, true);
	expect(until([&] {
		       OBSDataAutoRelease next = get(f.id);
		       return obs_data_get_bool(next, "enabled");
	       }),
	       "controller did not resume");

	state = get(f.id);
	config = obs_data_get_obj(state, "config");
	OBSDataAutoRelease frame_group = row(config, "groups", "frame");
	OBSDataAutoRelease frame_settings = obs_data_get_obj(frame_group, "settings");
	list(frame_settings, "leads", {"Controller camera A", "Controller unrelated"});
	obs_data_set_int(frame_settings, "lead_rule", 1);
	expect(configure(f.id, config), "All leads configuration was rejected");
	settle();
	expect(press(f.id, "camera", a.c_str()) && until([&] { return obs_sceneitem_visible(frame); }),
	       "All leads did not show its follower with both leads visible");
	obs_sceneitem_set_visible(unrelated, false);
	expect(until([&] { return !obs_sceneitem_visible(frame); }), "All leads ignored one hidden lead");
	obs_sceneitem_set_visible(unrelated, true);
	expect(until([&] { return obs_sceneitem_visible(frame); }), "All leads did not resume after restoring a lead");

	// Source and scene renames keep IDs and both the active and saved references.
	obs_source_set_name(obs_sceneitem_get_source(camera_a), "Controller camera A renamed");
	obs_source_set_name(obs_scene_get_source(f.shared), "Controller other layout renamed");
	expect(until([&] { return choice(f.id, "camera", "Controller camera A renamed") == a; }),
	       "renaming a source changed its choice identity");
	expect(press(f.id, "", "", "main") && until([&] {
		       return current(f.id, "content") == "front" && obs_sceneitem_visible(camera_a) &&
			      obs_sceneitem_visible(shared_main) && !obs_sceneitem_visible(shared_detail);
	       }),
	       "saved view or shared scope broke after a rename");

	// Round-trip the actual source record, including UUID, rather than creating
	// an unrelated controller with a copied display name.
	OBSDataAutoRelease saved = obs_save_source(f.controller);
	f.remove_controller();
	settle();
	f.controller = obs_load_source(saved);
	if (f.controller) {
		obs_source_filter_add(obs_scene_get_source(f.scene), f.controller);
		obs_source_load(f.controller);
	}
	expect(f.controller && f.id == obs_source_get_uuid(f.controller), "source reload lost controller identity");
	expect(until([&] { return current(f.id, "content") == "front"; }) &&
		       choice(f.id, "camera", "Controller camera A renamed") == a && press(f.id, "", "", "detail"),
	       "saved selections, views or choice IDs did not survive source reload");
	if (!until([&] {
		    return current(f.id, "content") == "detail" && obs_sceneitem_visible(detail) &&
			   !obs_sceneitem_visible(main);
	    })) {
		expect(false, "could not prepare removal during a controller command");
		return;
	}

	// OBS can remove a filter synchronously from a visibility listener while
	// its command is applying. Keep the fixture's source reference throughout;
	// only detach inside the callback, without frontend or vendor work there.
	struct RemoveOnShow {
		obs_source_t *scene, *controller;
		obs_sceneitem_t *item;
		std::atomic<bool> entered{false}, completed{false};
	} removal{obs_scene_get_source(f.scene), f.controller, main};
	const auto remove_on_show = [](void *opaque, calldata_t *cd) {
		auto *test = static_cast<RemoveOnShow *>(opaque);
		if (calldata_ptr(cd, "item") != test->item || !calldata_bool(cd, "visible") ||
		    test->entered.exchange(true))
			return;
		obs_source_filter_remove(test->scene, test->controller);
		test->completed = true;
	};
	auto *signal_handler = obs_source_get_signal_handler(removal.scene);
	signal_handler_connect(signal_handler, "item_visible", remove_on_show, &removal);
	const bool accepted = press(f.id, "content", "front");
	const bool removed = accepted && until([&] { return removal.completed.load(); });
	expect(removed, "removing the controller from item_visible did not complete");
	if (!removed)
		blog(LOG_ERROR, "[Scene Controller test] FAIL: callback removal accepted=%d entered=%d completed=%d",
		     int(accepted), int(removal.entered.load()), int(removal.completed.load()));
	signal_handler_disconnect(signal_handler, "item_visible", remove_on_show, &removal);
	if (removed) {
		std::atomic<bool> fenced{false};
		obs_queue_task(
			OBS_TASK_GRAPHICS, [](void *opaque) { static_cast<std::atomic<bool> *>(opaque)->store(true); },
			&fenced, true);
		expect(fenced, "graphics work did not continue after callback removal");
		OBSDataAutoRelease detached = get(f.id);
		expect(obs_filter_get_parent(f.controller) == nullptr && obs_data_get_bool(detached, "success") &&
			       !obs_data_get_bool(detached, "available") && !press(f.id, "content", "detail"),
		       "a detached controller remained available or accepted another command");
		const bool unrelated_before = obs_sceneitem_visible(unrelated);
		obs_sceneitem_set_visible(main, false);
		obs_sceneitem_set_visible(detail, true);
		const auto completed_frames = [] {
			const uint64_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
			return total - std::min(total, lagged);
		};
		const auto frames = completed_frames();
		expect(until([&] { return completed_frames() >= frames + 2; }),
		       "rendered frames did not advance after callback removal");
		expect(!obs_sceneitem_visible(main) && obs_sceneitem_visible(detail) &&
			       obs_sceneitem_visible(unrelated) == unrelated_before,
		       "a retired controller changed manual visibility after callback removal");
	}

	if (issues.size() == before)
		blog(LOG_INFO,
		     "[Scene Controller test] PASS: independent groups, nested items, shared scenes, Together/Follow, combined views, invalid/disabled requests, renames, source reload and removal inside a visibility callback");
}

inline bool legacy_press(obs_scene_t *scene, obs_source_t *filter, const char *mode)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "scene", obs_source_get_name(obs_scene_get_source(scene)));
	obs_data_set_string(data, "filter", obs_source_get_name(filter));
	obs_data_set_string(data, "mode", mode);
	obs_data_set_string(data, "how", "on");
	OBSDataAutoRelease result = request("SetSceneMode", data);
	return obs_data_get_bool(result, "success");
}

inline OBSDataAutoRelease import_legacy(const std::string &controller, obs_source_t *legacy, bool activate)
{
	OBSDataAutoRelease data = obs_data_create();
	obs_data_set_string(data, "controllerId", controller.c_str());
	obs_data_set_string(data, "legacyFilterUuid", obs_source_get_uuid(legacy));
	obs_data_set_bool(data, "activate", activate);
	return request("ImportSceneController", data);
}

inline OBSDataAutoRelease imported_group(const std::string &controller, obs_source_t *legacy)
{
	OBSDataAutoRelease state = get(controller);
	OBSDataAutoRelease config = obs_data_get_obj(state, "config");
	OBSDataArrayAutoRelease groups = obs_data_get_array(config, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups); ++i) {
		OBSDataAutoRelease group = obs_data_array_item(groups, i);
		OBSDataAutoRelease origin = obs_data_get_obj(group, "origin");
		if (std::strcmp(obs_data_get_string(origin, "filterUuid"), obs_source_get_uuid(legacy)) == 0)
			return group;
	}
	return nullptr;
}

inline void check_shared_import(QStringList &issues)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("Scene Controller shared import: ") + QString::fromUtf8(message));
	};
	Fixture f;
	if (!f.controller) {
		expect(false, "controller was unavailable");
		return;
	}
	auto *first = f.add("Shared import first capture", true);
	auto *second = f.add("Shared import second capture");
	auto *shared_first = obs_scene_add(f.shared, obs_sceneitem_get_source(first));
	auto *shared_second = obs_scene_add(f.shared, obs_sceneitem_get_source(second));
	obs_sceneitem_set_visible(shared_first, false);
	obs_sceneitem_set_visible(shared_second, true);
	OBSDataAutoRelease link_settings = obs_data_create();
	list(link_settings, "items", {"Shared import first capture", "Shared import second capture"});
	obs_data_set_int(link_settings, "mode", 0);
	obs_data_set_bool(link_settings, "lock_hidden", true);
	obs_data_set_string(link_settings, "transition", "none");
	obs_data_set_string(link_settings, "hide_transition", "none");
	OBSSourceAutoRelease legacy =
		obs_source_create("stagekit_visibility_link", "Shared import original", link_settings, nullptr);
	OBSSourceAutoRelease other_legacy =
		obs_source_create("stagekit_visibility_link", "Other layout original", link_settings, nullptr);
	obs_source_filter_add(obs_scene_get_source(f.scene), legacy);
	obs_source_filter_add(obs_scene_get_source(f.shared), other_legacy);
	settle();
	OBSDataAutoRelease imported = import_legacy(f.id, legacy, true);
	expect(obs_data_get_bool(imported, "success") && until([&] { return bool(imported_group(f.id, legacy)); }),
	       "active Choose-one import was rejected");
	OBSDataAutoRelease definition = imported_group(f.id, legacy);
	const std::string group = obs_data_get_string(definition, "id");
	const std::string first_id = choice(f.id, group.c_str(), "Shared import first capture");
	const std::string second_id = choice(f.id, group.c_str(), "Shared import second capture");
	if (group.empty() || first_id.empty() || second_id.empty()) {
		expect(false, "imported choices were unavailable");
		return;
	}
	OBSDataAutoRelease state = get(f.id);
	OBSDataAutoRelease config = obs_data_get_obj(state, "config");
	definition = row(config, "groups", group);
	list(definition, "scope", {obs_source_get_name(obs_scene_get_source(f.shared))});
	expect(configure(f.id, config) && until([&] {
		       OBSDataAutoRelease next = imported_group(f.id, legacy);
		       OBSDataArrayAutoRelease scope = obs_data_get_array(next, "scope");
		       return obs_data_array_count(scope) == 1 && current(f.id, group.c_str()).empty();
	       }),
	       "saved Other scenes scope did not include the differing secondary selection");
	{
		// The controller's owner is offscreen while the scoped layout is
		// showing. Normal OBS ticks, not an explicit runtime tick, must drain
		// commands and update both layouts. The frontend fixture separately
		// exercises this with the actual Program scene selector.
		struct Showing {
			obs_source_t *owner, *other;
			Showing(obs_source_t *owner, obs_source_t *other) : owner(owner), other(other)
			{
				obs_source_dec_showing(owner);
				obs_source_inc_showing(other);
			}
			~Showing()
			{
				obs_source_dec_showing(other);
				obs_source_inc_showing(owner);
			}
		} showing(obs_scene_get_source(f.scene), obs_scene_get_source(f.shared));
		expect(until([&] { return !obs_source_showing(showing.owner) && obs_source_showing(showing.other); }),
		       "could not make the controller owner offscreen and its other layout visible");
		auto selected = [&](bool want_first) {
			return obs_sceneitem_visible(first) == want_first &&
			       obs_sceneitem_visible(shared_first) == want_first &&
			       obs_sceneitem_visible(second) != want_first &&
			       obs_sceneitem_visible(shared_second) != want_first &&
			       current(f.id, group.c_str()) == (want_first ? first_id : second_id);
		};
		expect(press(f.id, group.c_str(), first_id.c_str()) && until([&] { return selected(true); }),
		       "an offscreen imported controller did not select its current source in the other layout");
		expect(press(f.id, group.c_str(), second_id.c_str()) && until([&] { return selected(false); }),
		       "an offscreen imported controller did not change the choice in both layouts");
		settle();
		expect(selected(false), "the existing secondary Link Visibility rule reverted the shared choice");
		obs_sceneitem_set_visible(shared_first, true);
		expect(until([&] { return !obs_sceneitem_visible(shared_second); }) &&
			       press(f.id, group.c_str(), second_id.c_str()) && until([&] { return selected(false); }),
		       "reselecting a source did not repair a differing secondary eye selection");
		OBSDataAutoRelease saved = obs_save_source(f.controller);
		f.remove_controller();
		settle();
		f.controller = obs_load_source(saved);
		if (f.controller) {
			obs_source_filter_add(showing.owner, f.controller);
			obs_source_load(f.controller);
		}
		expect(f.controller && f.id == obs_source_get_uuid(f.controller) &&
			       until([&] { return bool(imported_group(f.id, legacy)); }) &&
			       choice(f.id, group.c_str(), "Shared import first capture") == first_id &&
			       press(f.id, group.c_str(), first_id.c_str()) && until([&] { return selected(true); }),
		       "source-record reload lost the shared import scope or offscreen command handling");
	}
	obs_source_filter_remove(obs_scene_get_source(f.scene), legacy);
	obs_source_filter_remove(obs_scene_get_source(f.shared), other_legacy);
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[Scene Controller test] PASS: imported Choose-one group covers both layouts while its owner is offscreen, preserves secondary native rules, repairs mixed visibility and reloads stable choices");
}

inline void check_original_removal(QStringList &issues)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("Scene Controller original removal: ") +
				      QString::fromUtf8(message));
	};
	Fixture f;
	if (!f.controller) {
		expect(false, "controller was unavailable");
		return;
	}
	auto *main = f.add("Cleanup main picture");
	auto *detail = f.add("Cleanup detail picture");
	auto *first = f.add("Cleanup first camera", true);
	auto *second = f.add("Cleanup second camera");
	std::string mode_group, link_group, first_choice, second_choice, mode_uuid, link_uuid;
	{
		OBSDataAutoRelease mode_settings = modes("Cleanup main picture", "Cleanup detail picture");
		obs_data_set_int(mode_settings, "mode|detail|place_ms", 725);
		OBSSourceAutoRelease legacy =
			obs_source_create("stagekit_scene_modes", "Cleanup original modes", mode_settings, nullptr);
		OBSDataAutoRelease link_settings = obs_data_create();
		list(link_settings, "items", {"Cleanup first camera", "Cleanup second camera"});
		obs_data_set_int(link_settings, "mode", 0);
		obs_data_set_string(link_settings, "transition", "none");
		obs_data_set_string(link_settings, "hide_transition", "none");
		OBSSourceAutoRelease link = obs_source_create("stagekit_visibility_link", "Cleanup original cameras",
							      link_settings, nullptr);
		obs_source_filter_add(obs_scene_get_source(f.scene), legacy);
		obs_source_filter_add(obs_scene_get_source(f.scene), link);
		mode_uuid = obs_source_get_uuid(legacy);
		link_uuid = obs_source_get_uuid(link);
		OBSDataAutoRelease imported_modes = import_legacy(f.id, legacy, true);
		OBSDataAutoRelease imported_links = import_legacy(f.id, link, true);
		expect(obs_data_get_bool(imported_modes, "success") && obs_data_get_bool(imported_links, "success"),
		       "active imports were rejected");
		expect(until([&] { return bool(imported_group(f.id, legacy)) && bool(imported_group(f.id, link)); }),
		       "active groups were not stored");
		OBSDataAutoRelease mode_definition = imported_group(f.id, legacy);
		OBSDataAutoRelease link_definition = imported_group(f.id, link);
		mode_group = obs_data_get_string(mode_definition, "id");
		link_group = obs_data_get_string(link_definition, "id");
		first_choice = choice(f.id, link_group.c_str(), "Cleanup first camera");
		second_choice = choice(f.id, link_group.c_str(), "Cleanup second camera");
		expect(!mode_group.empty() && !link_group.empty() && !first_choice.empty() && !second_choice.empty(),
		       "imported controls did not have stable IDs");
		expect(press(f.id, mode_group.c_str(), "detail") &&
			       press(f.id, link_group.c_str(), second_choice.c_str()) && until([&] {
				       return obs_sceneitem_visible(detail) && !obs_sceneitem_visible(main) &&
					      obs_sceneitem_visible(second) && !obs_sceneitem_visible(first);
			       }),
		       "imported controls did not start");

		// OBS undo retains detached filters. Their stored controller groups
		// must remain valid even while UUID lookup still finds those originals.
		obs_source_set_enabled(link, false);
		obs_source_filter_remove(obs_scene_get_source(f.scene), legacy);
		obs_source_filter_remove(obs_scene_get_source(f.scene), link);
		expect(!obs_filter_get_parent(legacy) && !obs_filter_get_parent(link),
		       "original filters were not detached");
		OBSDataAutoRelease state = get(f.id);
		OBSDataAutoRelease config = obs_data_get_obj(state, "config");
		OBSDataAutoRelease switched = row(config, "groups", link_group);
		expect(!obs_data_get_bool(switched, "enabled"),
		       "removal lost an original's enabled change made immediately before deletion");
		obs_data_set_bool(switched, "enabled", true);
		expect(configure(f.id, config), "retained detached originals invalidated their existing groups");
		expect(until([&] {
			       OBSDataAutoRelease now = get(f.id);
			       OBSDataAutoRelease group = row(now, "groups", link_group);
			       return obs_data_get_bool(group, "enabled");
		       }) && !obs_source_enabled(link),
		       "the removed original's switch overrode a surviving group's enabled edit");
		expect(press(f.id, mode_group.c_str(), "front") &&
			       press(f.id, link_group.c_str(), first_choice.c_str()) && until([&] {
				       return obs_sceneitem_visible(main) && !obs_sceneitem_visible(detail) &&
					      obs_sceneitem_visible(first) && !obs_sceneitem_visible(second);
			       }),
		       "controls stopped after removing originals retained for undo");
		state = get(f.id);
		config = obs_data_get_obj(state, "config");
		switched = row(config, "groups", link_group);
		obs_data_set_bool(switched, "enabled", false);
		expect(configure(f.id, config) && until([&] {
			       OBSDataAutoRelease now = get(f.id);
			       OBSDataAutoRelease group = row(now, "groups", link_group);
			       return !obs_data_get_bool(group, "enabled");
		       }) && !press(f.id, link_group.c_str(), second_choice.c_str()),
		       "a surviving group could not be disabled after removing its original");
		obs_data_set_bool(switched, "enabled", true);
		expect(configure(f.id, config) && until([&] {
			       OBSDataAutoRelease now = get(f.id);
			       OBSDataAutoRelease group = row(now, "groups", link_group);
			       return obs_data_get_bool(group, "enabled");
		       }),
		       "a surviving group could not be enabled again after removing its original");
		OBSDataAutoRelease invalid = obs_data_create_from_json(obs_data_get_json(config));
		OBSDataAutoRelease changed = row(invalid, "groups", mode_group);
		obs_data_set_string(changed, "id", "new-detached-import");
		expect(!configure(f.id, invalid), "a new group could claim an already detached original");
	}
	// Releasing the last original handles represents deletion without undo.
	expect(until([&] {
		       OBSSourceAutoRelease mode_original = obs_get_source_by_uuid(mode_uuid.c_str());
		       OBSSourceAutoRelease link_original = obs_get_source_by_uuid(link_uuid.c_str());
		       return !mode_original && !link_original;
	       }),
	       "removed original handles remained alive");
	expect(press(f.id, mode_group.c_str(), "detail") && press(f.id, link_group.c_str(), second_choice.c_str()) &&
		       until([&] {
			       return current(f.id, mode_group.c_str()) == "detail" && obs_sceneitem_visible(detail) &&
				      !obs_sceneitem_visible(main) && obs_sceneitem_visible(second) &&
				      !obs_sceneitem_visible(first);
		       }),
	       "controls stopped after the originals were fully released");
	settle();
	OBSDataAutoRelease saved = obs_save_source(f.controller);
	f.remove_controller();
	settle();
	f.controller = obs_load_source(saved);
	if (f.controller) {
		obs_source_filter_add(obs_scene_get_source(f.scene), f.controller);
		obs_source_load(f.controller);
	}
	expect(f.controller && f.id == obs_source_get_uuid(f.controller) &&
		       obs_source_filter_count(obs_scene_get_source(f.scene)) == 1 &&
		       until([&] { return current(f.id, mode_group.c_str()) == "detail"; }) &&
		       choice(f.id, link_group.c_str(), "Cleanup second camera") == second_choice,
	       "source reload without originals lost controller, group, mode or choice identity");
	OBSDataAutoRelease state = get(f.id);
	OBSDataAutoRelease config = obs_data_get_obj(state, "config");
	OBSDataAutoRelease mode_definition = row(config, "groups", mode_group);
	OBSDataAutoRelease mode_settings = obs_data_get_obj(mode_definition, "settings");
	expect(obs_data_get_int(mode_settings, "mode|detail|place_ms") == 725,
	       "source reload lost imported native settings");
	expect(press(f.id, mode_group.c_str(), "front") && press(f.id, link_group.c_str(), first_choice.c_str()) &&
		       until([&] {
			       return obs_sceneitem_visible(main) && !obs_sceneitem_visible(detail) &&
				      obs_sceneitem_visible(first) && !obs_sceneitem_visible(second);
		       }),
	       "reloaded groups could not execute without their originals");
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[Scene Controller test] PASS: active imports survive retained original removal, full deletion and source-record reload with stable controls");
}

inline void check_migration(QStringList &issues)
{
	check_shared_import(issues);
	check_original_removal(issues);
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("Scene Controller import: ") + QString::fromUtf8(message));
	};
	Fixture f;
	if (!f.controller) {
		expect(false, "controller was unavailable");
		return;
	}
	auto *first = f.add("Imported main picture");
	auto *second = f.add("Imported detail picture");
	OBSDataAutoRelease settings = modes("Imported main picture", "Imported detail picture");
	obs_data_set_int(settings, "mode|detail|place_ms", 725);
	obs_data_set_int(settings, "mode|detail|place_ease_curve", 3);
	obs_data_set_string(settings, "mode|detail|leave_to", "front");
	obs_data_set_string(settings, "for|Imported main picture|transition", "fade_transition");
	obs_data_set_int(settings, "for|Imported main picture|transition_ms", 140);
	list(settings, "mode|detail|place", {"Imported detail picture"});
	vec2 place{123.5f, 77.0f};
	obs_sceneitem_set_pos(second, &place);
	OBSSourceAutoRelease legacy =
		obs_source_create("stagekit_scene_modes", "Existing content modes", settings, nullptr);
	obs_source_filter_add(obs_scene_get_source(f.scene), legacy);
	expect(until([&] { return obs_sceneitem_visible(first) && !obs_sceneitem_visible(second); }),
	       "legacy baseline did not start");
	obs_properties_t *properties = obs_source_properties(legacy);
	expect(obs_properties_get(properties, "mode_now") && !obs_properties_get(properties, "controller_configure"),
	       "ordinary Scene Modes properties lost their native editor");
	obs_property_t *save_places = obs_properties_get(properties, "mode|detail|place_save");
	expect(save_places != nullptr, "legacy saved placement control was unavailable");
	if (save_places)
		obs_property_button_clicked(save_places, legacy);
	obs_properties_destroy(properties);
	const size_t filters_before = obs_source_filter_count(obs_scene_get_source(f.scene));
	OBSDataAutoRelease copied = import_legacy(f.id, legacy, false);
	expect(obs_data_get_bool(copied, "success"), "inactive copy was rejected");
	expect(until([&] { return bool(imported_group(f.id, legacy)); }), "copied group was not stored");
	OBSDataAutoRelease imported = imported_group(f.id, legacy);
	OBSDataAutoRelease preserved = obs_data_get_obj(imported, "settings");
	OBSDataAutoRelease preserved_xf = obs_data_get_obj(preserved, "mode|detail|xf");
	OBSDataAutoRelease preserved_scene =
		obs_data_get_obj(preserved_xf, obs_source_get_name(obs_scene_get_source(f.scene)));
	OBSDataAutoRelease preserved_location = obs_data_get_obj(preserved_scene, "Imported detail picture");
	expect(imported && !obs_data_get_bool(imported, "enabled") &&
		       obs_data_get_int(preserved, "mode|detail|place_ms") == 725 &&
		       obs_data_get_int(preserved, "mode|detail|place_ease_curve") == 3 &&
		       std::strcmp(obs_data_get_string(preserved, "mode|detail|leave_to"), "front") == 0 &&
		       std::strcmp(obs_data_get_string(preserved, "for|Imported main picture|transition"),
				   "fade_transition") == 0 &&
		       obs_data_get_int(preserved, "for|Imported main picture|transition_ms") == 140 &&
		       obs_data_get_double(preserved_location, "x") == 123.5 &&
		       obs_data_get_double(preserved_location, "y") == 77.0,
	       "copy lost placement, leave or per-source transition settings");
	expect(legacy_press(f.scene, legacy, "detail") &&
		       until([&] { return obs_sceneitem_visible(second) && !obs_sceneitem_visible(first); }),
	       "inactive import interfered with the original rule");

	// A review copy cannot execute alongside the original. Its explicit
	// ownership control transfers execution before the enabled group can run.
	const std::string group = obs_data_get_string(imported, "id");
	OBSDataAutoRelease state = get(f.id);
	OBSDataAutoRelease activation = obs_data_get_obj(state, "config");
	OBSDataAutoRelease draft = row(activation, "groups", group);
	obs_data_set_bool(draft, "enabled", true);
	expect(!configure(f.id, activation), "review copy could execute without transferring ownership");
	OBSDataAutoRelease origin = obs_data_get_obj(draft, "origin");
	obs_data_set_bool(origin, "activate", true);
	expect(configure(f.id, activation), "explicit activation of a reviewed import was rejected");
	expect(until([&] {
		       OBSDataAutoRelease now = get(f.id);
		       OBSDataAutoRelease active_group = row(now, "groups", group);
		       return obs_data_get_bool(active_group, "enabled");
	       }),
	       "reviewed import did not activate");
	expect(!group.empty() && obs_source_filter_count(obs_scene_get_source(f.scene)) == filters_before,
	       "import created hidden filters or lost its stable group ID");
	properties = obs_source_properties(legacy);
	expect(obs_properties_get(properties, "controller_configure") && !obs_properties_get(properties, "mode_now"),
	       "active import still exposed an ineffective legacy mode editor");
	obs_properties_destroy(properties);
	OBSDataAutoRelease imported_record = obs_save_source(f.controller);
	f.remove_controller();
	settle();
	f.controller = obs_load_source(imported_record);
	if (f.controller) {
		obs_source_filter_add(obs_scene_get_source(f.scene), f.controller);
		obs_source_load(f.controller);
	}
	expect(f.controller && until([&] { return bool(imported_group(f.id, legacy)); }),
	       "active import mapping did not survive source reload");
	expect(press(f.id, group.c_str(), "front") && until([&] {
		       return current(f.id, group.c_str()) == "front" && obs_sceneitem_visible(first) &&
			      !obs_sceneitem_visible(second);
	       }),
	       "controller could not select an imported mode");
	// Loading a copied controller must not steal an already active legacy
	// filter, even though loading bypasses the public configuration request.
	{
		OBSDataAutoRelease duplicate_settings = obs_source_get_settings(f.controller);
		OBSSourceAutoRelease duplicate_controller = obs_source_create(
			"stagekit_scene_controller", "Copied active import", duplicate_settings, nullptr);
		obs_source_filter_add(obs_scene_get_source(f.scene), duplicate_controller);
		const std::string duplicate_id = obs_source_get_uuid(duplicate_controller);
		settle();
		OBSDataAutoRelease duplicate_state = get(duplicate_id);
		expect(!obs_data_get_bool(duplicate_state, "available") &&
			       !press(duplicate_id, group.c_str(), "detail"),
		       "a copied controller could own an active legacy filter twice");
		expect(legacy_press(f.scene, legacy, "detail") &&
			       until([&] { return current(f.id, group.c_str()) == "detail"; }),
		       "a copied controller stole the original legacy key");
		obs_source_filter_remove(obs_scene_get_source(f.scene), duplicate_controller);
		expect(legacy_press(f.scene, legacy, "front") &&
			       until([&] { return current(f.id, group.c_str()) == "front"; }),
		       "removing an unavailable copy cleared another controller's forwarding");
	}
	place = {42.0f, 11.0f};
	obs_sceneitem_set_pos(second, &place);
	expect(legacy_press(f.scene, legacy, "detail") && until([&] {
		       vec2 restored{};
		       obs_sceneitem_get_pos(second, &restored);
		       return current(f.id, group.c_str()) == "detail" && obs_sceneitem_visible(second) &&
			      !obs_sceneitem_visible(first) && std::fabs(restored.x - 123.5f) < 0.1f &&
			      std::fabs(restored.y - 77.0f) < 0.1f;
	       }),
	       "an existing Scene Mode key did not forward to the imported group");
	f.remove_controller();
	settle();
	expect(legacy_press(f.scene, legacy, "front") &&
		       until([&] { return obs_sceneitem_visible(first) && !obs_sceneitem_visible(second); }),
	       "removing the controller did not restore the original mode runtime");
	properties = obs_source_properties(legacy);
	expect(obs_properties_get(properties, "mode_now") && !obs_properties_get(properties, "controller_configure"),
	       "removing the controller did not restore native mode properties");
	obs_properties_destroy(properties);
	obs_source_filter_remove(obs_scene_get_source(f.scene), legacy);

	// Existing Link Visibility keys operate the original filter's enabled state.
	f.controller =
		obs_source_create("stagekit_scene_controller", "Imported visibility controller", nullptr, nullptr);
	obs_source_filter_add(obs_scene_get_source(f.scene), f.controller);
	f.id = obs_source_get_uuid(f.controller);
	OBSDataAutoRelease link_settings = obs_data_create();
	list(link_settings, "items", {"Imported main picture", "Imported detail picture"});
	obs_data_set_int(link_settings, "mode", 1);
	obs_data_set_string(link_settings, "transition", "none");
	obs_data_set_string(link_settings, "for|Imported detail picture|hide_transition", "fade_transition");
	obs_data_set_int(link_settings, "for|Imported detail picture|hide_ms", 110);
	OBSSourceAutoRelease link =
		obs_source_create("stagekit_visibility_link", "Existing linked overlays", link_settings, nullptr);
	obs_source_filter_add(obs_scene_get_source(f.scene), link);
	settle();
	OBSDataAutoRelease moved = import_legacy(f.id, link, true);
	expect(obs_data_get_bool(moved, "success"), "Link Visibility import was rejected");
	expect(until([&] { return bool(imported_group(f.id, link)); }), "Link Visibility group was not stored");
	imported = imported_group(f.id, link);
	preserved = obs_data_get_obj(imported, "settings");
	const std::string link_group = obs_data_get_string(imported, "id");
	properties = obs_source_properties(link);
	expect(obs_properties_get(properties, "controller_configure") && !obs_properties_get(properties, "mode"),
	       "active Link Visibility import still exposed an ineffective native editor");
	obs_properties_destroy(properties);
	expect(std::strcmp(obs_data_get_string(preserved, "for|Imported detail picture|hide_transition"),
			   "fade_transition") == 0 &&
		       obs_data_get_int(preserved, "for|Imported detail picture|hide_ms") == 110,
	       "Link Visibility lost a per-source transition override");
	expect(press(f.id, link_group.c_str(), "off") &&
		       until([&] { return !obs_sceneitem_visible(first) && !obs_sceneitem_visible(second); }),
	       "imported Together Off did not hide the set");
	obs_source_set_enabled(link, false);
	settle();
	obs_sceneitem_set_visible(first, true);
	settle();
	expect(obs_sceneitem_visible(first) && !obs_sceneitem_visible(second),
	       "existing Link Visibility disable key did not stop the imported rule");
	obs_source_set_enabled(link, true);
	settle();
	obs_sceneitem_set_visible(first, false);
	settle();
	obs_sceneitem_set_visible(first, true);
	expect(until([&] { return obs_sceneitem_visible(second); }),
	       "existing Link Visibility enable key did not resume the imported rule");
	f.remove_controller();
	settle();
	obs_sceneitem_set_visible(first, false);
	expect(until([&] { return !obs_sceneitem_visible(second); }),
	       "removing the controller did not restore the original visibility runtime");
	properties = obs_source_properties(link);
	expect(obs_properties_get(properties, "mode") && !obs_properties_get(properties, "controller_configure"),
	       "removing the controller did not restore native visibility properties");
	obs_properties_destroy(properties);
	obs_source_filter_remove(obs_scene_get_source(f.scene), link);
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[Scene Controller test] PASS: complete legacy settings, inactive import, active handoff, old mode/visibility keys, no hidden filters and removal rollback");
}

} // namespace scene_controller_test
