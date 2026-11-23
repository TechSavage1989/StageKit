/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>
#include <memory>
#include <string>

namespace stagekit {

inline constexpr const char *kVisibilityLinkId = "stagekit_visibility_link";

// A scene filter that ties the eye icons of several items together:
// "show only one of these" or "show and hide these together".
void register_visibility_link();

// True when an enabled Link Visibility in `scene` set to "Follow sources"
// has the item's source among its followers, and every such filter that
// lists it has its lead sources saying hidden (none shown, or not all with
// "All leads shown"; a filter saved without lead sources: its first entry
// hidden): the item belongs to a mode that is not showing. Show in Scenes passes over such an item when it shows things.
// Safe from any thread.
bool link_visibility_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item);

// True while such a filter has not yet followed a change of what its lead
// sources say: the items it leads are on their way to being shown or hidden.
// Show in Scenes' "Follow what is shown" does not read the switch meanwhile.
// Safe from any thread.
bool link_visibility_settling(obs_source_t *scene, obs_sceneitem_t *item);

// Reuses the visibility rule engine as a native object, not a hidden filter.
class VisibilityLinkRuntime {
public:
	virtual ~VisibilityLinkRuntime() = default;
	virtual void tick(float seconds) = 0;
	virtual void runtime_enabled(bool enabled) = 0;
	// Reuse keeps pending visibility events and the last followed lead state.
	virtual void runtime_update(obs_data_t *settings, bool enabled) = 0;
	// Retire once before releasing an internal engine, including when readers
	// still retain it. New calls never act or hold sources hidden; the last
	// retained call releases its weak subscriptions during destruction.
	virtual void runtime_retire() = 0;
	virtual bool runtime_retired() const = 0;
	virtual std::string runtime_controller_id() = 0;
	// An explicit source choice (only-one), or the whole set (together).
	virtual bool runtime_press(const char *source, bool shown, std::string &why) = 0;
};

std::shared_ptr<VisibilityLinkRuntime> visibility_link_runtime(obs_data_t *settings, obs_source_t *owner,
							       obs_source_t *parent, bool enabled);
bool visibility_link_forward(obs_source_t *legacy, const std::shared_ptr<VisibilityLinkRuntime> &runtime);
bool visibility_link_unforward(obs_source_t *legacy, const std::shared_ptr<VisibilityLinkRuntime> &expected);

} // namespace stagekit
