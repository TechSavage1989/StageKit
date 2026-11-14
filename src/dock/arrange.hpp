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

#include <string>
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
int copy_to_every_scene(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items,
			const std::vector<std::string> *only = nullptr,
			const std::vector<std::string> *to_sources = nullptr, bool even_locked = false);

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
