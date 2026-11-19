/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "../common/scene-util.hpp"

#include <obs.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

// Lining items up, spacing them out and snapping them together, measured
// either by an item's box or by what is actually visible in it.

struct Measure {
	bool visible_pixels = true;    // the drawn picture, not the transparent room around it
	bool leave_out_shadows = true; // a Shadow & Backdrop's shadow does not count as picture
};

enum class Edge { Left, Center, Right, Top, Middle, Bottom };
// Saved in settings: append only. First and Last are the first and last item
// selected, which then stays put while the others come to it.
enum class Against { Screen, Selection, First, Last };

// How many times measuring went on without an answer from OBS's drawing
// thread (it then goes by boxes). For saying so after a button press.
int measuring_gave_up();

// OBS is closing: from now on measuring answers with boxes at once and hands
// nothing to the drawing thread, and everything that watches on a timer
// should do nothing.
void measuring_stop();
bool measuring_stopped();

// Where an item is on its scene's canvas, by its box or by what shows.
Box measure_item(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how);

// The same, for something that may be in another item's way: false when the
// item is measured by what shows and nothing does (it is then not there).
bool measure_seen(obs_source_t *scene, obs_sceneitem_t *item, const Measure &how, Box &box);

// The items selected in a scene, bottom-most first. A selected group is one
// item; in a group that is not selected, its selected items count.
std::vector<obs_sceneitem_t *> selected_items(obs_source_t *scene);

// Moves an item so the measured picture shifts by (dx, dy) canvas pixels.
void shift_item(obs_source_t *scene, obs_sceneitem_t *item, double dx, double dy);

// Lines items up on one edge or centre line. Returns how many moved.
// `line`, if given, receives where they were lined up: an x for left, centre
// and right, a y for top, middle and bottom.
int align_items(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, Edge edge, Against against,
		const Measure &how, double *line = nullptr);

// Spaces three or more items so the gaps between them are equal.
int distribute_items(obs_source_t *scene, std::vector<obs_sceneitem_t *> items, bool across, const Measure &how);

// Makes the other items as wide and/or as tall as the top-most one looks.
// keep_shape scales both directions alike; without it only the directions
// asked for change, which stretches the picture.
// Like OBS's Copy Transform and Paste Transform, for every scene at once:
// each item's place, size, rotation, crop and bounding box go to every copy of
// its source in the other scenes, on the same spot of the screen whether a
// copy sits in a group or not. Locked copies are left alone. Returns how many
// copies changed.
// With `only`, just the copies in the scenes of those names.
// With `to_sources` (and it is not empty), the transform goes to the items
// showing the sources of those names instead of to copies of the same source,
// in this scene as well as the others; with several items selected, the last
// in the list is the one copied.
// With `allow`, each copy is asked about once it has its new transform (its
// scene, the copy, its box before and after): a copy refused is put back as
// it was and not counted.
using CopyAllowed = std::function<bool(obs_source_t *scene, obs_sceneitem_t *copy, const Box &from, const Box &to)>;
int copy_to_every_scene(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items,
			const std::vector<std::string> *only = nullptr,
			const std::vector<std::string> *to_sources = nullptr, bool even_locked = false,
			const CopyAllowed &allow = {});

// Gives every copy of `item`'s source in the scenes of these names, while
// they are not on screen, the transform it has here, locked copies too, and hands each to a Fit to
// Space in its scene as placed on purpose. Scenes being shown are left as
// they are: nothing jumps in view. For a filter that keeps an item the same
// in every scene, so that it is already there when the scene is switched to.
// Says how many copies changed.
// `home`: the point (x, y on the screen) the sharing filter looks for its
// space around, which the other scenes' filters take for theirs; null when
// it has none.
int share_place(obs_source_t *scene, obs_sceneitem_t *item, const std::vector<std::string> &scenes,
		const double *home = nullptr);

// Locks or unlocks scene items, groups' items included, and says how many
// changed. `sources`: only items showing a source of one of these names
// (null: every item). `scenes`: only in the scenes of these names (null:
// every scene).
int set_items_locked(bool lock, const std::vector<std::string> *sources, const std::vector<std::string> *scenes);

// Gives one filter's settings to the filters of the same kind on other
// sources (scenes and groups count as sources), and says how many filters
// were changed. `to`: only the sources of these names (null: every source).
// On a source with several filters of that kind, the one with the same name
// is taken, or all of them when none has it. `add_missing`: a source with
// none gets a copy of the filter; `added` counts those. What a filter keeps
// for its own source (which item it works on, where it put things, what it
// has learned) is left as it is on each.
int copy_filter_settings(obs_source_t *from, const char *filter_name, const std::vector<std::string> *to,
			 bool add_missing, int &added);
// The names of the sources shown as items in these scenes (null: no scene
// in particular, so nothing), groups' items and the scenes themselves included.
std::vector<std::string> sources_in_scenes(const std::vector<std::string> &scenes);

int match_size(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, bool width, bool height,
	       bool keep_shape, const Measure &how);

// How far an item slides in a direction to the next edge it meets, in canvas
// pixels: its leading edge stops `gap` short of the near side of another
// visible item in its path, or of that item's far side if it is already
// past the near side, or of the screen's edge. So each press goes one edge
// further. dx and dy are -1, 0 or 1: a diagonal slides across first, then
// up or down from there. Both 0 centres the item in the free space around
// it. Items in `leave_out` (one brought along, say) are not in the way.
// Nothing takes it past the screen's edge. `from`, when given, is where the
// item is taken to be instead of where it is now (it is still on its way
// there from the last slide).
struct Offset {
	double x = 0.0, y = 0.0;
};
Offset slide_offset(obs_source_t *scene, obs_sceneitem_t *item, int dx, int dy, double gap, const Measure &how,
		    const std::vector<obs_sceneitem_t *> &leave_out, const Box *from = nullptr);

// Fit the selected items in stacking order using fit::free_space. A fit
// entry supplies its own gap, alignment and measuring options even when off;
// `toward` (Align in the space, by its saved number) is for an item with none.
// Other sources keep their image position and size.
// The caller hands changed items to their filters after any glide finishes.
struct FitToSpaceNote {
	bool busy = false;
	int shrunk = 0, refused = 0;          // items kept smaller, items left as they were
	std::vector<std::string> gave_way_to; // the fitted sources they gave way to
	// Items left to their own fit, which could not be told to fit them now:
	// the filter, then the source.
	std::vector<std::pair<std::string, std::string>> own_fit;
	std::vector<std::string> refitted;   // the fits told to fit their item now
	std::vector<std::string> own_area;   // fits that keep their item at its size in an area of their own
	std::vector<std::string> keeps_size; // items that keep their size, their space smaller than they are
	std::vector<std::string> unknown;    // fitted sources not worked out with the item shown
};
int fit_to_space(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, double gap, const Measure &how,
		 std::vector<obs_sceneitem_t *> *fitted = nullptr, FitToSpaceNote *note = nullptr, int toward = 0);

// An item of a scene drawn elsewhere, for the checks: a part of it before
// (`from`) and after (`to`), its box or, round a shadow sized in screen
// pixels (`keeps_outside`), the image in its box: what another fit sees of
// it inside that part scales with it, what lies outside keeps its size.
// `now`: its box before. `claim`: where its own fit will then take it.
// `home`: with `has_claim`, that fit's home once the item is put there
// (where it is put: FitEngine::placed), when it has one (`has_home`); its
// claim is elsewhere when it does not keep the gap there.
// `keeps`, `held`: with `has_claim`, whether that fit keeps the item's size,
// and what it then holds for it whatever the other fits do (its size where
// it is put, `fit::Claim::least`): what a fit that resizes its item goes by.
struct MovedBox {
	int64_t id = 0;
	Box from, to;
	bool keeps_outside = false;
	Box now;
	bool has_claim = false;
	Box claim;
	bool has_home = false;
	Box home;
	bool keeps = false;
	Box held;
};
enum class FitCheck { Kept, Changes, Unknown };
// A copy given another transform (its box in its scene `from`, then `to`):
// whether every fit of the free space in its scene keeps the space it holds
// (within half a pixel), the copy's own fit taking it where it would.
// Unknown when one of them has not worked its space out with the copy shown
// (its scene not shown since OBS started). `who` names the fitted source.
FitCheck copy_keeps_fits(obs_source_t *scene, obs_sceneitem_t *copy, const Box &from, const Box &to,
			 std::string *who = nullptr);

// Moves an item the last few pixels so an edge or centre meets the screen's
// or another item's. Returns true when it moved.
// After an item was resized by hand: pulls back any side that was dragged
// to within `gap` of the screen's edge or of another item, so it stops that
// far short. `before` is its box when the drag began. Measured as the
// Arrange options say. Returns whether it changed anything.
bool limit_resize(obs_source_t *scene, obs_sceneitem_t *item, const Box &before, double gap, const Measure &how);
bool snap_item(obs_source_t *scene, obs_sceneitem_t *item, double within, const Measure &how);

// The lines an item can meet: the screen's edges and centre, and the edges
// and centres of the other visible items. Gathering them measures every item
// once, so it is done when a drag starts or a button is pressed, never while
// something is moving.
struct SnapLines {
	std::vector<double> xs, ys;
};
SnapLines snap_lines(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &leave_out, const Measure &how);

// The nearest of those lines that a box has an edge or its centre within
// reach of, across and down: which line, and how far the box is from it.
struct Meeting {
	bool across = false, down = false;
	double line_x = 0, line_y = 0;
	double dx = 0, dy = 0;
};
Meeting meet_lines(const Box &box, const SnapLines &lines, double within);

} // namespace stagekit
