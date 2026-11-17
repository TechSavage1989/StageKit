/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "go-to-scene.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <callback/proc.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace stagekit {

namespace {

std::mutex g_test_mutex;
bool g_test_switching = false;
std::string g_test_program, g_test_switched;

// True when `scene` holds an item of one of `sources`, straight in it or in
// one of its groups.
bool scene_has_any(obs_source_t *scene, const std::vector<std::string> &sources)
{
	obs_scene_t *s = scene ? obs_scene_from_source(scene) : nullptr;
	if (!s || sources.empty())
		return false;
	struct Look {
		const std::vector<std::string> *sources;
		bool found = false;
		void at(obs_sceneitem_t *item)
		{
			const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
			if (name && std::find(sources->begin(), sources->end(), name) != sources->end())
				found = true;
		}
	} look{&sources};
	obs_scene_enum_items(
		s,
		[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
			auto *l = static_cast<Look *>(param);
			l->at(item);
			if (!l->found && obs_sceneitem_is_group(item))
				obs_sceneitem_group_enum_items(
					item,
					[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
						static_cast<Look *>(p)->at(inner);
						return !static_cast<Look *>(p)->found;
					},
					param);
			return !l->found;
		},
		&look);
	return look.found;
}

bool staying_in(obs_source_t *current, const std::vector<std::string> &stay_in)
{
	const char *name = current ? obs_source_get_name(current) : nullptr;
	return name && std::find(stay_in.begin(), stay_in.end(), name) != stay_in.end();
}

} // namespace

obs_source_t *scene_to_go_to(obs_source_t *current, const GoTo &go)
{
	obs_source_t *scene = obs_get_source_by_name(go.target.c_str());
	if (!scene)
		return nullptr;
	if (!obs_scene_from_source(scene) || obs_source_is_group(scene) || scene == current ||
	    staying_in(current, go.stay_in) || scene_has_any(current, go.sources)) {
		obs_source_release(scene);
		return nullptr;
	}
	return scene;
}

void go_to_scene(GoTo go)
{
	{
		std::lock_guard lock(g_test_mutex);
		if (g_test_switching) {
			obs_source_t *current = obs_get_source_by_name(g_test_program.c_str());
			obs_source_t *scene = scene_to_go_to(current, go);
			if (scene) {
				if (!g_test_switched.empty())
					g_test_switched += "|";
				g_test_switched += go.target;
				obs_source_release(scene);
			}
			obs_source_release(current);
			return;
		}
	}
	if (!obs_frontend_get_main_window())
		return;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			GoTo *go = static_cast<GoTo *>(param);
			const bool studio = obs_frontend_preview_program_mode_active();
			obs_source_t *current = studio ? obs_frontend_get_current_preview_scene()
						       : obs_frontend_get_current_scene();
			obs_source_t *scene = scene_to_go_to(current, *go);
			if (scene) {
				if (studio)
					obs_frontend_set_current_preview_scene(scene);
				else
					obs_frontend_set_current_scene(scene);
				obs_log(LOG_INFO, "%s '%s': switched to scene '%s'", go->kind.c_str(),
					go->filter.c_str(), go->target.c_str());
				obs_source_release(scene);
			}
			obs_source_release(current);
			delete go;
		},
		new GoTo(std::move(go)), false);
}

void go_to_scene_add_test_calls()
{
	static std::atomic<bool> added{false};
	if (added.exchange(true))
		return;
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_scene_switch(in string program)",
		[](void *, calldata_t *cd) {
			const char *program = calldata_string(cd, "program");
			std::lock_guard lock(g_test_mutex);
			g_test_switching = true;
			g_test_program = program ? program : "";
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_scene_switched(out string scenes)",
		[](void *, calldata_t *cd) {
			std::lock_guard lock(g_test_mutex);
			calldata_set_string(cd, "scenes", g_test_switched.c_str());
			g_test_switched.clear();
		},
		nullptr);
}

} // namespace stagekit
