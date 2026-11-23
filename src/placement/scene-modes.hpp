/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Scene Modes (StageKit): a filter on a scene that names its modes and the
// sources of each. One mode is on at a time (or none, when allowed): turning
// a mode on shows its sources and hides the other modes' sources. Sources in
// no mode are never touched.

#pragma once

#include "../common/scene-filter-ids.hpp"

#include <obs.h>

#include <string>
#include <memory>
#include <vector>

namespace stagekit {

void register_scene_modes();

// One mode, as the Stream Deck plugin and the tests see it. A mode is known
// by its id, which never changes; its name and type can.
struct SceneModeLook {
	std::string id, name, type;
};

// One Scene Modes filter, as the vendor requests answer for it.
struct SceneModesLook {
	std::string scene, filter, current, previous; // current and previous are mode ids, "" for none
	bool allow_none = false, enabled = false;
	std::vector<SceneModeLook> modes;
	std::vector<std::string> scenes; // "Same modes in scenes"
};

// Every Scene Modes filter, or those on the scene named (null or empty: all).
std::vector<SceneModesLook> scene_modes_list(const char *scene);

// A press, as a key or a hotkey makes it. `how` is "on" (as it comes),
// "toggle", "next" or "none"; `mode` is a mode's id, or its name (the first
// with that name), and is not needed for "next" and "none". `filter` may be
// empty for the first Scene Modes on the scene. Queued and acted on in the
// filter's next tick. Says false, with the reason in `why`, when the scene,
// the filter or the mode is not there, the filter is switched off, or no
// mode is not allowed.
// The panel can opt in to show_scene for "on": show its explicit destination
// without the covered-scene exception, otherwise its actual scene parent.
// Groups do not infer an outer scene. Other callers keep the native default.
bool scene_modes_press(const char *scene, const char *filter, const char *mode, const char *how, std::string &why,
		       bool show_scene = false);

// True when an enabled Scene Modes covering `scene` (its own scene, or one
// listed under "Same modes in scenes") has the item's source in one of its
// modes and the mode on now does not have it (or no mode is on). `scene` is
// the scene holding the item (the one holding its group, for an item in a
// group). Safe from any thread; call with none of your own locks held.
// Registered with holds_hidden_register, so `mode_holds_hidden` asks it.
bool scene_modes_holds_hidden(obs_source_t *scene, obs_sceneitem_t *item);

// True for the tick after a mode change, before the filter has looked again.
bool scene_modes_settling(obs_source_t *scene, obs_sceneitem_t *item);

// The same mode engine, owned by one Scene Controller without creating an
// OBS source. Its private settings never read or write the controller's root.
// Calls that change the runtime are serialized by the controller's tick.
class SceneModesRuntime {
public:
	virtual ~SceneModesRuntime() = default;
	virtual void tick(float seconds) = 0;
	// Retire before the owner drops this engine. A legacy request may still
	// hold it, so it must leave the shared registry before the owner can die.
	virtual void runtime_retire() = 0;
	virtual bool runtime_retired() const = 0;
	// Keep the live selection, first-load state and glides while editing its
	// definitions or refreshing the controller's source inventory.
	virtual void runtime_update(obs_data_t *settings, bool enabled) = 0;
	virtual void runtime_enabled(bool enabled) = 0;
	virtual std::string runtime_controller_id() = 0;
	virtual SceneModesLook runtime_look() = 0;
	virtual obs_data_t *runtime_settings() = 0; // a reference to release
	virtual bool runtime_press(const char *mode, const char *how, std::string &why, bool show_scene = false) = 0;
	// Stage every member of a combined view before ticking any: hidden-state
	// queries then see all target modes, independent of engine execution order.
	virtual bool runtime_stage(const char *mode, bool scene_switch, std::string &why) = 0;
	virtual void runtime_unstage() = 0;
	virtual bool runtime_changed() = 0;
};

std::shared_ptr<SceneModesRuntime> scene_modes_runtime(obs_data_t *settings, obs_source_t *owner, obs_source_t *parent,
						       bool enabled);
// The original filter remains visible and saved. Its existing keys and state
// follow this runtime while it lives; removing the controller restores it.
bool scene_modes_forward(obs_source_t *legacy, const std::shared_ptr<SceneModesRuntime> &runtime);
// A delayed retirement must never clear a newer controller's forwarding.
bool scene_modes_unforward(obs_source_t *legacy, const std::shared_ptr<SceneModesRuntime> &expected);
// The complete existing source/scene rename rules, also used for child settings.
bool scene_modes_rename_settings(obs_data_t *settings, const char *was, const char *now);

} // namespace stagekit
