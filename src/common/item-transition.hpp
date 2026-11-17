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
// As the scene sets: the transition a Scene Transitions filter on the item's
// scene gives it, or, where there is none (or it leaves the item alone), a
// fade of the way's own duration. The default in every filter since beta155.
// It is resolved in one place only, `resolve_item_change`, which every call
// below goes through.
inline constexpr const char *kTransitionScene = "scene";

// Gives an item the show and hide transition asked for, if it does not have
// them already. Call it before showing or hiding the item.
void set_item_transitions(obs_sceneitem_t *item, const std::string &kind, uint32_t ms);

// Shows or hides an item, and says whether that changed anything. In a scene
// (or group) that is being shown this is OBS's own call and nothing more: the
// item changes with the transitions it has. In one that is not being shown the
// change is a cut: OBS starts the item's transition all the same, but only
// drawing the scene ends it, so it would still be running whenever that scene
// next came on, and until it had been drawn once a hidden source would count
// as in use there again (a capture card set to let go of its device would
// open it and close it). The transition OBS started is stopped at once; the
// item keeps its transitions for the next change made in view. Items whose
// transitions are another filter's (`item_transitions_owner`) are passed over
// here too: they are shown and hidden as OBS does it.
// Link Visibility and Show in Scenes go through this, after
// `set_item_transitions` or `apply_item_change`. Program Trigger, the two
// switchers, Auto Capture's hide, Show/Hide Animation and Standby's stand-ins
// do not yet. A group's inner items, whose transitions OBS starts when the
// group itself is shown or hidden, are not stopped.
bool set_item_visible(obs_sceneitem_t *item, bool visible);

// Ends a show transition still under way on a hidden item, or a hide
// transition on a shown one. OBS only ends an item's transition by drawing
// it, so one whose scene was left with a cut part-way through is never ended,
// and its source is in use again whenever that scene is on (a capture card
// stays open). The next change in the other direction does not end it either.
// Says whether there was one.
bool end_stale_transition(obs_sceneitem_t *item);

// Adds the "Change with" list and its "Lasting" time to a properties window.
void add_item_transition_properties(obs_properties_t *props);

// As the scene sets, 300 ms (a fade of 300 ms where the scene sets nothing).
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

// Some items have their transitions set for them by one filter of their
// source's own (a Standby filter standing in for the source while it is
// hidden: the source's way out and its stand-in's way in are one change).
// `owns` says which, item by item, so a filter can own a source's item in one
// scene and not in another; for those, `set_item_transitions` and
// `apply_item_change` leave the item alone, so that whatever hides the item
// (Show in Scenes, a switcher) does it that filter's way. The owner itself
// sets them with this. One for the whole plugin.
void item_transitions_owner(bool (*owns)(obs_sceneitem_t *item));
void apply_item_change_as_owner(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene = nullptr);

// Adds the control. `list_setting` names the PickList of sources the filter
// acts on (null for a filter without one): with it, a "Settings for" box
// chooses between the usual change and each listed source's own.
void add_item_change_properties(obs_properties_t *props, obs_source_t *owner, const char *list_setting);

// As the scene sets, 300 ms, hiding the same way as showing.
void item_change_defaults(obs_data_t *settings);

// ---------------------------------------------------------------------------
// As the scene sets (`kTransitionScene`).

// Set by the Scene Transitions filter: what a scene's filter says for an
// item, if one covers it. `scene` is the scene the item is in, or the one
// holding its group. Must answer with no "As the scene sets" way left in
// `out`. One for the whole plugin; nullptr clears it.
using SceneLookup = bool (*)(obs_source_t *scene, obs_sceneitem_t *item, ItemChange &out);
void item_transitions_scene(SceneLookup lookup);

// `change` with every "As the scene sets" way replaced: by the scene's (show
// and hide each on its own), or by a fade of the way's own duration. `scene`
// as for `apply_item_change`; null for the item's own. A Standby filter calls
// it before it swaps a change for its stand-in, so the stand-in comes the way
// the source goes in the scene's terms too.
ItemChange resolve_item_change(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene = nullptr);

// Whether the item already carries `change` (resolved first): the same kinds,
// durations and, for each way, exactly the settings the way sets (a slide
// "Off the nearest edge" by where the item lies now). Lets a caller write
// nothing where nothing would change.
bool item_change_matches(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene = nullptr);

// Whether an item's transitions are another filter's (`item_transitions_owner`).
bool item_transitions_owned(obs_sceneitem_t *item);

} // namespace stagekit
