/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "holds-hidden.hpp"

#include <mutex>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

std::mutex g_lock;
std::vector<std::pair<HoldsHidden, HoldsHidden>> g_kinds;

// A copy of the answers, so that no lock is held while a filter is asked:
// the filters asked look at each other from their ticks.
std::vector<std::pair<HoldsHidden, HoldsHidden>> kinds()
{
	std::lock_guard<std::mutex> hold(g_lock);
	return g_kinds;
}

} // namespace

void holds_hidden_register(HoldsHidden holds, HoldsHidden settling)
{
	if (!holds && !settling)
		return;
	std::lock_guard<std::mutex> hold(g_lock);
	for (const auto &kind : g_kinds)
		if (kind.first == holds && kind.second == settling)
			return;
	g_kinds.emplace_back(holds, settling);
}

bool mode_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item)
{
	for (const auto &kind : kinds())
		if (kind.first && kind.first(scene, item))
			return true;
	return false;
}

bool mode_settling(obs_source_t *scene, obs_sceneitem_t *item)
{
	for (const auto &kind : kinds())
		if (kind.second && kind.second(scene, item))
			return true;
	return false;
}

} // namespace stagekit
