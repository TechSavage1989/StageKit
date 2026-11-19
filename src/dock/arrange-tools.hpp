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
#include <graphics/vec2.h>

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
	// The copy tools also change copies that are locked. Off as it comes.
	bool copy_locked = false;
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
	// The dock's Auto-fit switch acts on the selected sources only (their
	// Fit to Space filters and Scene Fit entries in the scene being edited),
	// not on every fit filter of that scene. Off as it comes.
	bool auto_fit_sources = false;
	// The Place tab's edge distance: how far short of the screen's edges and
	// of other items the slides stop and Fit to space leaves its item.
	int place_margin = 30;
	// The Place tab's alignment grid for Fit to space, for a source with no
	// fit of its own (Align in the space by its saved number, fit::kTowardX;
	// 0 the middle, as it comes).
	int place_toward = 0;
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
	FitSpace,       // each item as large as the free space round it lets it be (fit_to_space)
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
	// The items a tool put somewhere on purpose (Fit to space), for their
	// Fit to Space filters to be told once they are there. Valid for the
	// call; run_arrange tells them itself.
	std::vector<obs_sceneitem_t *> placed;
	// Fit to space: what it held back so that no other fit gives up room
	// (see `fit_to_space`), and, with In every scene, the scenes its result
	// was not copied to for that reason, each with the fitted source there
	// ("scene: source"), and those whose fits have not worked their spaces
	// out yet.
	FitToSpaceNote fit;
	std::vector<std::string> not_copied, not_known;
};

// Runs an action on the given items of a scene with the shared options.
ArrangeOutcome run_arrange(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items);

// The same without an undo step, for callers that record their own.
ArrangeOutcome run_arrange_now(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items);

// Where an item was before a tool that glides changed it (Fit to space), for
// the dock to take it back there and glide it to its new place, as it does
// a snap on release. The dock gives the handler, on the window's thread,
// where the tools run; it is called with the items changed, each valid for
// the call, after the change is in OBS's undo list. An item with `hand_over`
// is to be told to its Fit to Space (`fit_item_placed`, in `scene`) once its
// glide ends, never before: the filter reads the item's place and size when
// told. The handler tells it at once when it does not glide.
struct ArrangeStart {
	obs_sceneitem_t *item;
	vec2 pos, scale, bounds;
	bool hand_over;
};
using ArrangeGlide = std::function<void(obs_source_t *scene, const std::vector<ArrangeStart> &)>;
// `owner` says whose handler it is: a null handler with an owner only clears
// the handler that owner set; with no owner (OBS closing) it clears any.
void set_arrange_glide(ArrangeGlide handler, const void *owner);

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

// The dock's Bounds list: the bounds type the items share, or -1 when they
// differ or there are none.
int bounds_type_of(const std::vector<obs_sceneitem_t *> &items);
struct BoundsOutcome {
	int changed = 0;
	// "source\tfilter" for each item a fit filter of the scene sets the
	// bounds of (an enabled Fit to Space, or a switched-on Scene Fit entry).
	std::vector<std::string> fitted;
};
// Sets the bounds type of each item, as OBS's Edit Transform does: from no
// bounds the bounds become the picture's size on screen, back to none the
// scale keeps that size, and the picture stays where it is. The bounds
// alignment is kept. One step in OBS's undo list.
BoundsOutcome set_bounds_type(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, obs_bounds_type type);
// The fit filter of `scene` that fits `source` ("" for none).
std::string fitted_by(obs_source_t *scene, const std::string &source);

// The dock's Auto-fit switch. `sources` null: every Fit to Space and Scene
// Fit on the scene; otherwise only those sources' Fit to Space filters and
// their Scene Fit entries. -1 when there are none, 1 when any is on, else 0.
int auto_fit_state(obs_source_t *scene, const std::vector<std::string> *sources);
// Switches them all on or off. Returns how many were changed.
int set_auto_fit(obs_source_t *scene, bool on, const std::vector<std::string> *sources);
// The sources the items show, each once, in order.
std::vector<std::string> sources_of(const std::vector<obs_sceneitem_t *> &items);
// The switch on the scene being edited and its selection, at the chosen
// level (for the hotkey): on when all are off, else off.
void toggle_auto_fit_on_selection();

void register_arrange_hotkeys();
void arrange_save_load(obs_data_t *save_data, bool saving);

} // namespace stagekit
