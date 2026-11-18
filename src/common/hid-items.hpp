/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The capture filters' "Hide or show the capture" command, when there is no
// Standby Image to cover with: the hide hides the capture's items that are
// shown, in every scene and group, and writes down which; the show brings
// back only those, and only while they are still hidden, passing over an
// item a mode not showing holds hidden. A show of an item the command never
// hid is not a show: the owner's camera, hidden on purpose in every mode but
// one, came up full-screen over the others at the second press.
//
// The record is kept in the filter's own settings (`kHidItemsKey`, a source's
// own: the copy tools pass over it), so a restart between the two presses
// still shows only what was hidden. Items are known by name and id, never
// held.

#pragma once

#include <obs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace stagekit {

inline constexpr const char *kHidItemsKey = "hid_items";

struct HidItem {
	std::string scene; // the scene it is shown in (the one holding its group)
	std::string in;    // the scene or group it is straight in
	int64_t id = 0;
};

// How the items change: with their own transitions (`kind` empty), or with
// `kind` for `ms` set on them first (`set_item_transitions`).
struct HidChange {
	std::string kind;
	uint32_t ms = 300;
};

// The record in `settings`.
std::vector<HidItem> hid_items_read(obs_data_t *settings);
void hid_items_write(obs_data_t *settings, const std::vector<HidItem> &items);

// Whether `filter`'s settings hold a record.
bool hid_items_pending(obs_source_t *filter);

// One press of the command for `capture`, from the tick of `filter` (whose
// settings hold the record). With a record: the items in it still hidden are
// shown, and the record is cleared. Without one: every shown item of the
// capture is hidden and written down; when none is shown, nothing happens.
// `who` names the filter kind for the log. Says whether the capture is now
// hidden by the command (a record is pending).
bool hid_items_press(obs_source_t *filter, obs_source_t *capture, const HidChange &change, const char *who);

} // namespace stagekit
