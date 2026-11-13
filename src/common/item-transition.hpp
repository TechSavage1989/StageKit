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

} // namespace stagekit
