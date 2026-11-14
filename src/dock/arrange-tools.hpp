/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "arrange.hpp"

#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace stagekit {

// The arrange tools as one set of actions with one set of options, shared by
// the dock, the hotkeys and the right-click menu in OBS's preview.

struct ArrangeOptions {
	Against against = Against::Screen;
	Measure how;
	bool keep_shape = true; // resizing keeps an item's proportions; off stretches one direction
	bool live_snap = false; // snap after an item is dragged and let go
	// After an item is resized by hand, pull it back to stop `limit_px` short
	// of the screen's edge and of other items.
	bool limit_resize = false;
	int limit_px = 30;
	int within = 20;     // how close counts for snapping, in pixels
	bool guides = false; // draw guide lines over OBS's preview
	// Not an arrange option, but kept and saved the same way: the dock's
	// Send to buttons, and the keys it makes, move the item's source in every
	// scene it is in. Off: only in the scene being worked on.
	bool send_every_scene = true;
	// The arrange tools also put each item they moved in the same place in
	// every other scene showing its source. Off as it comes.
	bool arrange_every_scene = false;
	// The Send to arrows slide the item to the next edge instead of sending it to a spot.
	bool send_slide = false;
	// How the dock's Send to moves glide: the curve and which end it shapes
	// (EaseCurve and EaseMode as numbers; cubic, in and out).
	int send_curve = 3;
	int send_mode = 0;
	// Copy transform to chosen scenes: the scenes it copies to, by name.
	std::vector<std::string> copy_scenes;
	// Both copy tools: the sources that take the transform, by name. Empty
	// (as it comes): the copies of the selected item's own source.
	std::vector<std::string> copy_sources;
};

ArrangeOptions arrange_options();
void set_arrange_options(const ArrangeOptions &options);
// Changes whenever the options do, so anything showing them can follow.
uint64_t arrange_options_revision();

// The dock's slide (Send to's arrows with "Slide to the next edge"), for a
// key or a remote control: `direction` is 0 to 8, row by row from up-left,
// 4 the centre. The dock gives the handler and takes it away again; both,
// and the slide itself, happen on the window's thread.
void set_slide_handler(std::function<void(int)> handler);
// Asks for a slide from any thread. False when no dock is there to do it.
bool request_slide(int direction);

enum class ArrangeAction {
	Left,
	Center,
	Right,
	Top,
	Middle,
	Bottom,
	SpaceAcross,
	SpaceDown,
	SnapNow,
	SameWidth,
	SameHeight,
	SameSize,
	SameEverywhere, // the selected items' place, size, rotation and crop for every copy of their sources
	SameChosen,     // the same, only in the scenes chosen in ArrangeOptions::copy_scenes
	Count
};

struct ArrangeInfo {
	const char *name;   // for hotkeys and object names
	const char *text;   // locale key of the short label
	const char *hotkey; // locale key of the name in Settings → Hotkeys
	size_t needs;       // how many items it takes
};
const ArrangeInfo &arrange_info(ArrangeAction action);

struct ArrangeOutcome {
	size_t had = 0, needs = 1;
	int changed = 0;
};

// Runs an action on the given items of a scene with the shared options.
ArrangeOutcome run_arrange(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items);

// The same without an undo step, for callers that record their own.
ArrangeOutcome run_arrange_now(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items);

// Every item's place and size in a scene, as OBS's own undo keeps them.
std::string transform_snapshot(obs_source_t *scene);
// The same for every scene at once, for a change that reaches into several.
std::string transform_snapshot_all();
// Adds a step to OBS's Edit → Undo that goes back to `before` and forward to `after`.
void add_transform_undo(const char *name, const std::string &before, const std::string &after);
// For the tests: puts the scene back as before the last step (or forward again).
void replay_last_arrange(bool redo);

// The selected items of a scene in the order they were selected, as OBS
// reported each selection (see track_selection_order).
std::vector<obs_sceneitem_t *> selection_in_order(obs_source_t *scene);
// Starts noting when each item is selected, in every scene and group. Once, at load.
void track_selection_order();

// The line the last line-up tool put things on, for showing it in the
// preview. `serial` goes up each time a tool lines something up.
struct ArrangeGuide {
	uint64_t serial = 0;
	bool vertical = false; // an x across the canvas; otherwise a y
	double at = 0.0;
};
ArrangeGuide last_arrange_guide();

// Runs it on whatever is selected in the scene being edited in OBS.
ArrangeOutcome run_arrange_on_selection(ArrangeAction action);

// The scene being edited: the preview in Studio Mode, else the current scene.
// Returns a new reference, or nullptr.
obs_source_t *editing_scene();

void register_arrange_hotkeys();
void arrange_save_load(obs_data_t *save_data, bool saving);

} // namespace stagekit
