/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "scene-rev.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace stagekit {

namespace {

std::atomic<uint64_t> g_rev{1};
std::once_flag g_once;
std::atomic<bool> g_watching{false};

constexpr const char *kSignals[] = {"item_add", "item_remove", "item_visible", "item_transform", "reorder", "refresh"};
constexpr size_t kKinds = sizeof(kSignals) / sizeof(kSignals[0]);
// How many of each signal, for the tests and the harness (what keeps
// changing in a collection at rest).
std::atomic<uint64_t> g_seen[kKinds];

thread_local int t_quiet = 0;

void changed(void *param, calldata_t *)
{
	if (t_quiet)
		return;
	g_rev.fetch_add(1, std::memory_order_relaxed);
	g_seen[(size_t)(uintptr_t)param].fetch_add(1, std::memory_order_relaxed);
}

void watch_scene(obs_source_t *source)
{
	if (!source || obs_source_get_type(source) != OBS_SOURCE_TYPE_SCENE)
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	// OBS connects one callback with one parameter once, however often asked.
	for (size_t i = 0; i < kKinds; i++)
		signal_handler_connect(sh, kSignals[i], changed, (void *)(uintptr_t)i);
	g_rev.fetch_add(1, std::memory_order_relaxed);
}

void created(void *, calldata_t *cd)
{
	watch_scene(static_cast<obs_source_t *>(calldata_ptr(cd, "source")));
}

void start()
{
	std::call_once(g_once, [] {
		g_watching = true;
		signal_handler_connect(obs_get_signal_handler(), "source_create", created, nullptr);
		obs_enum_scenes(
			[](void *, obs_source_t *scene) {
				watch_scene(scene);
				return true;
			},
			nullptr);
	});
}

} // namespace

uint64_t scene_revision()
{
	if (!g_watching.load(std::memory_order_relaxed))
		start();
	return g_rev.load(std::memory_order_relaxed);
}

void scene_revision_bump()
{
	g_rev.fetch_add(1, std::memory_order_relaxed);
}

void scene_signals_quiet(bool on)
{
	t_quiet += on ? 1 : -1;
}

uint64_t scene_signals_seen(const char *signal)
{
	for (size_t i = 0; i < kKinds; i++)
		if (strcmp(kSignals[i], signal) == 0)
			return g_seen[i].load(std::memory_order_relaxed);
	return 0;
}

void scene_revision_stop()
{
	if (!g_watching.exchange(false))
		return;
	signal_handler_disconnect(obs_get_signal_handler(), "source_create", created, nullptr);
	obs_enum_scenes(
		[](void *, obs_source_t *scene) {
			signal_handler_t *sh = obs_source_get_signal_handler(scene);
			for (size_t i = 0; i < kKinds; i++)
				signal_handler_disconnect(sh, kSignals[i], changed, (void *)(uintptr_t)i);
			return true;
		},
		nullptr);
}

} // namespace stagekit
