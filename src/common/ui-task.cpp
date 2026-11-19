/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ui-task.hpp"

#include <obs-frontend-api.h>

#include <atomic>

namespace stagekit {

namespace {

// -1 until asked the first time (OBS's window is there long before any
// plugin is loaded, so the first answer holds), or until a test says.
std::atomic<int> g_there{-1};

} // namespace

bool ui_thread_there()
{
	int there = g_there.load();
	if (there < 0) {
		int unknown = -1;
		g_there.compare_exchange_strong(unknown, obs_frontend_get_main_window() ? 1 : 0);
		there = g_there.load();
	}
	return there == 1;
}

bool on_ui_thread_later(obs_task_t task, void *param)
{
	if (!ui_thread_there())
		return false;
	obs_queue_task(OBS_TASK_UI, task, param, false);
	return true;
}

void ui_task_add_test_calls()
{
	static std::atomic<bool> added{false};
	if (added.exchange(true))
		return;
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_ui_tasks(in bool there)",
		[](void *, calldata_t *cd) { g_there = calldata_bool(cd, "there") ? 1 : 0; }, nullptr);
}

} // namespace stagekit
