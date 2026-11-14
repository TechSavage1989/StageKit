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

#include <cstdint>
#include <string>

namespace stagekit {

// How a scene item appears and disappears when one of StageKit's filters
// shows or hides it: any transition OBS has, a cut, or whatever the item is
// set to already. Every filter that shows and hides items offers the same
// two controls, kept in the settings "transition" and "transition_ms".
inline constexpr const char *kTransitionCut = "none";  // at once; the item's transitions are taken off
inline constexpr const char *kTransitionKeep = "keep"; // whatever is set on the item is left alone
inline constexpr const char *kTransitionFade = "fade_transition";

// Gives an item the show and hide transition asked for, if it does not have
// them already. Call it before showing or hiding the item.
void set_item_transitions(obs_sceneitem_t *item, const std::string &kind, uint32_t ms);

// Adds the "Change with" list and its "Lasting" time to a properties window.
void add_item_transition_properties(obs_properties_t *props);

// Fade, 300 ms.
void item_transition_defaults(obs_data_t *settings);

// Reads the two settings.
std::string item_transition_kind(obs_data_t *settings);
uint32_t item_transition_ms(obs_data_t *settings);

// ---------------------------------------------------------------------------
// The fuller control: showing and hiding each have their own transition, a
// transition's own settings (the way a slide goes, a fade's colour) are set
// in the filter, and each source on a filter's list can have its own in
// place of the filter's usual ones. The owner: "must allow you to select the
// specific settings for the transition type in the filter setting, like left
// right up or down for slide ... for linked sources also allow each one to
// have its specific setting set independently".

// One way of appearing, or of disappearing.
struct ItemWay {
	std::string kind = kTransitionFade;
	uint32_t ms = 300;
	std::string own; // the kind's own settings that were set, as JSON; empty for none
};

struct ItemChange {
	ItemWay show, hide;
};

// What the settings say: the filter's usual change (`source` null or empty),
// or the one for a source on its list, which falls back on the usual one
// wherever it is left as "the same".
ItemChange item_change_read(obs_data_t *settings, const char *source = nullptr);

// Gives an item its show and its hide transition, each with its own settings.
// `scene` is the scene the item is in (the one holding its group, for an item
// in a group): a slide or a swipe set to "Off the nearest edge" goes by where
// the item lies in it.
void apply_item_change(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene = nullptr);

// Adds the control. `list_setting` names the PickList of sources the filter
// acts on (null for a filter without one): with it, a "Settings for" box
// chooses between the usual change and each listed source's own.
void add_item_change_properties(obs_properties_t *props, obs_source_t *owner, const char *list_setting);

// Fade, 300 ms, hiding the same way as showing.
void item_change_defaults(obs_data_t *settings);

} // namespace stagekit
