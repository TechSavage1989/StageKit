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

#include <functional>
#include <string>
#include <vector>

namespace stagekit {

// One look for every list a user builds up, in every filter and source:
// a heading, a box to choose from that adds what is chosen, then a row for
// each entry with its own remove button.

struct PickList {
	const char *setting;       // where the list is kept in the settings
	bool as_text = false;      // kept as "a, b, c" in one string, not as an array of {value}
	const char *title;         // locale key: the heading
	const char *add = nullptr; // locale key: the label of the box to choose from; nullptr for no box
	const char *tip = nullptr; // locale key: a longer explanation, shown on the heading's rows
	// Fills the box with what can be chosen. Entries already in the list are left out for you.
	std::function<void(std::vector<std::string> &choices)> choices;
	// The heading as it is to be shown, when it is made up at run time
	// ("Mode: Game"). Wins over `title` when not empty.
	std::string title_text;
};

// The entries of a list, in order.
std::vector<std::string> pick_list_read(obs_data_t *settings, const char *setting, bool as_text);
void pick_list_write(obs_data_t *settings, const char *setting, bool as_text, const std::vector<std::string> &entries);

// Adds the list to a properties window, as a group of its own at the top
// level. `owner` is the filter or source the settings belong to.
void pick_list_add(obs_properties_t *props, obs_source_t *owner, const PickList &list);

// Something to call whenever a list in a properties window has had an entry
// added or taken off, for a control beside it that shows the list's entries.
// One for the whole plugin (the fuller Show/Hide transition's rebuild puts
// itself there every time a window is built); it gets the window's
// properties, the list's owner and the owner's settings.
void pick_list_on_change(void (*changed)(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings));

// The same, for one kind of owner only (its unversioned id). Called after the
// one above, never instead of it, so a kind that also has the fuller
// Show/Hide transition keeps that control's rebuild. One per kind.
void pick_list_on_change_for(const char *kind,
			     void (*changed)(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings));

// Rebuilds a list's rows after something other than the list itself changed it.
void pick_list_refresh(obs_properties_t *top, obs_source_t *owner, const char *setting, bool as_text);

} // namespace stagekit
