/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "item-transition.hpp"
#include "pick-list.hpp"
#include "scene-filter-ids.hpp"
#include "scene-util.hpp"
#include "../transition/morph-paint.hpp"
#include "../transition/item-morph.hpp"
#include "../transition/morph.hpp"
#include "tooltip.hpp"

#include <obs-module.h>

#include <algorithm>
#include <functional>
#include <atomic>
#include <cstring>
#include <vector>

namespace stagekit {

namespace {
std::atomic<bool (*)(obs_sceneitem_t *)> g_owns{nullptr};
bool owned(obs_sceneitem_t *item)
{
	auto owns = g_owns.load();
	return item && owns && owns(item);
}
std::atomic<SceneLookup> g_scene_lookup{nullptr};
std::atomic<int> g_overriding{0}; // filters with "Override other filters" ticked
void apply_way(obs_sceneitem_t *item, bool show, const ItemWay &way, obs_source_t *scene);

// An item transition that still holds a source has a change under way, or
// one OBS never finished: ended here. Stopping it says so to OBS, which takes
// it off the scene's list of what is in use; clearing it lets go of the
// source it held. One that holds nothing is left alone (see
// end_stale_transition).
bool end_held_change(obs_source_t *transition)
{
	if (!transition)
		return false;
	obs_source_t *a = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_A);
	obs_source_t *b = obs_transition_get_source(transition, OBS_TRANSITION_SOURCE_B);
	const bool holds = a || b;
	obs_source_release(a);
	obs_source_release(b);
	if (!holds)
		return false;
	obs_transition_force_stop(transition);
	obs_transition_clear(transition);
	return true;
}
} // namespace

void item_transitions_owner(bool (*owns)(obs_sceneitem_t *item))
{
	g_owns.store(owns);
}

bool item_transitions_owned(obs_sceneitem_t *item)
{
	return owned(item);
}

void item_transitions_scene(SceneLookup lookup)
{
	g_scene_lookup.store(lookup);
}

void item_transitions_scene_overrides(int change)
{
	g_overriding += change;
}

namespace {
// What the item's scene's Scene Transitions says for it, and whether it
// overrides other filters.
bool scene_says(obs_sceneitem_t *item, obs_source_t *scene, ItemChange &theirs, bool &overrides)
{
	overrides = false;
	SceneLookup lookup = g_scene_lookup.load();
	if (!lookup || !item)
		return false;
	obs_source_t *where = scene ? scene : obs_scene_get_source(obs_sceneitem_get_scene(item));
	return where && lookup(where, item, theirs, overrides);
}

// The scene overrides other filters for this item: its choice is the change,
// whatever was asked; never for an item a Standby owns. (The lookup has
// already passed over private sources, the item Morph's layer, ignored
// sources and the rest the scene does not cover.)
bool overridden(obs_sceneitem_t *item, obs_source_t *scene, ItemChange &theirs)
{
	bool overrides = false;
	return g_overriding.load() > 0 && !owned(item) && scene_says(item, scene, theirs, overrides) && overrides;
}

ItemChange resolve_change(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene, bool inherit_item)
{
	const bool show = change.show.kind == kTransitionScene, hide = change.hide.kind == kTransitionScene;
	if (!show && !hide && g_overriding.load() <= 0)
		return change;
	ItemChange theirs;
	bool overrides = false;
	const bool found = scene_says(item, scene, theirs, overrides);
	if (found && overrides && !owned(item))
		return theirs;
	if (!show && !hide)
		return change;
	ItemChange out = change;
	// Show in Scenes inherits an uncovered item's actual OBS transition, including its
	// settings and duration. Other filters retain their historical fade fallback.
	auto inherited = [&](bool appearing, const ItemWay &way, const ItemWay &scene_way) {
		if (found && scene_way.kind != kTransitionScene)
			return scene_way;
		if (inherit_item && obs_sceneitem_get_transition(item, appearing))
			return ItemWay{kTransitionKeep, way.ms, {}};
		return ItemWay{kTransitionFade, way.ms, {}};
	};
	if (show)
		out.show = inherited(true, change.show, theirs.show);
	if (hide)
		out.hide = inherited(false, change.hide, theirs.hide);
	return out;
}
} // namespace

ItemChange resolve_item_change(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene)
{
	return resolve_change(item, change, scene, false);
}

void cut_item_transitions(obs_sceneitem_t *item)
{
	if (!item || owned(item))
		return;
	for (bool show : {true, false})
		if (obs_sceneitem_get_transition(item, show))
			obs_sceneitem_set_transition(item, show, nullptr);
}

void set_item_transitions(obs_sceneitem_t *item, const std::string &kind, uint32_t ms)
{
	if (!item || owned(item))
		return;
	// Whatever kind was asked, a scene that overrides other filters gives
	// its own.
	if (ItemChange theirs; overridden(item, nullptr, theirs)) {
		apply_way(item, true, theirs.show, nullptr);
		apply_way(item, false, theirs.hide, nullptr);
		return;
	}
	if (kind == kTransitionKeep)
		return;
	if (kind == kTransitionScene) {
		ItemChange asked;
		asked.show = ItemWay{kTransitionScene, ms, {}};
		asked.hide = asked.show;
		const ItemChange change = resolve_item_change(item, asked, nullptr);
		apply_way(item, true, change.show, nullptr);
		apply_way(item, false, change.hide, nullptr);
		return;
	}
	for (bool show : {true, false}) {
		obs_source_t *now = obs_sceneitem_get_transition(item, show);
		if (kind == kTransitionCut) {
			if (now)
				obs_sceneitem_set_transition(item, show, nullptr);
			continue;
		}
		const char *now_id = now ? obs_source_get_unversioned_id(now) : nullptr;
		if (!now_id || kind != now_id) {
			const char *of = obs_source_get_name(obs_sceneitem_get_source(item));
			const std::string name =
				std::string(of ? of : "") + (show ? " Show Transition" : " Hide Transition");
			obs_source_t *made = obs_source_create_private(kind.c_str(), name.c_str(), nullptr);
			if (!made)
				continue; // that kind of transition is not installed
			obs_sceneitem_set_transition(item, show, made);
			obs_source_release(made);
		}
		if (obs_sceneitem_get_transition_duration(item, show) != ms)
			obs_sceneitem_set_transition_duration(item, show, ms);
	}
}

bool set_item_visible(obs_sceneitem_t *item, bool visible)
{
	if (!item)
		return false;
	// The scene the item is straight in; for an item in a group, the group.
	// A group is shown while a scene that shows it is.
	obs_source_t *holder = obs_scene_get_source(obs_sceneitem_get_scene(item));
	const bool in_view = holder && obs_source_showing(holder);
	if (owned(item) || obs_sceneitem_visible(item) == visible)
		return obs_sceneitem_set_visible(item, visible);
	// The change made here is the item's change now, so its other way's
	// transition holds nothing any more. OBS stops that one at a change but
	// only says so: one cut off when its scene was left part-way keeps the
	// source. Left there, it is a hide transition under way on a hidden
	// item once the item is hidden again, which OBS counts in when the scene
	// comes on (and end_stale_transition takes for a change made now): the
	// source was in use until that transition was drawn to its end; in the
	// tests, which have no sound, for good. Link Visibility's half-second
	// look caught it only while the item was shown in between (one run in
	// twenty under load). (This way's transition is not ended out of view
	// while it is off the item: done so, OBS's counts of what is in use went
	// wrong and the source stayed in use for good.)
	const auto end_other = [&] {
		end_held_change(obs_sceneitem_get_transition(item, !visible));
	};
	if (in_view) {
		const bool changed = obs_sceneitem_set_visible(item, visible);
		if (changed)
			end_other();
		return changed;
	}
	// Out of view: the item's transition for this change is taken off for
	// the change and put back after it, so OBS starts none. One started and
	// then stopped from outside (the first way, during beta147's making)
	// left a Morph unable to end the next change made in view: the source
	// it hid stayed in use for good.
	obs_source_t *held = obs_source_get_ref(obs_sceneitem_get_transition(item, visible));
	const uint32_t ms = obs_sceneitem_get_transition_duration(item, visible);
	if (held)
		obs_sceneitem_set_transition(item, visible, nullptr);
	const bool changed = obs_sceneitem_set_visible(item, visible);
	if (changed)
		end_other();
	if (held) {
		obs_sceneitem_set_transition(item, visible, held);
		obs_sceneitem_set_transition_duration(item, visible, ms);
		obs_source_release(held);
	}
	return changed;
}

bool end_stale_transition(obs_sceneitem_t *item)
{
	if (!item || owned(item))
		return false;
	obs_source_t *other = obs_sceneitem_get_transition(item, !obs_sceneitem_visible(item));
	if (!other || !obs_source_active(other))
		return false;
	// Only one that still holds a source is under way. OBS can leave a
	// finished item transition counted as in use for good (a scene counts its
	// item transitions in when it comes on only while they run, and out when
	// it goes only while they still run), so "in use" alone made this end the
	// same finished transition twice a second for the rest of the session
	// (the owner's log, Ending Text, 2,526 times).
	return end_held_change(other);
}

// ---------------------------------------------------------------- the fuller control

namespace {

inline constexpr const char *kUsual = "usual"; // a listed source: as the filter's usual change
inline constexpr const char *kSame = "same";   // hiding: the same way as showing
// The links a filter keeps for Morph: `kMorphPrefix` + "links", made with
// the boxes `add_morph_link_properties` adds under that prefix.
inline constexpr const char *kMorphPrefix = "morph_";
inline constexpr const char *kMorphLinks = "morph_links";

// Settings are kept under these names. `scope` is empty for the filter's
// usual change and "for|<source>|" for a listed source's own.
std::string key(const std::string &scope, const char *name)
{
	return scope + name;
}

// A kind's own settings: "<scope>show|<kind>|<its setting>".
std::string own_prefix(const std::string &scope, bool show, const std::string &kind)
{
	return scope + (show ? "show|" : "hide|") + kind + "|";
}

std::string scope_of(const char *source)
{
	return source && *source ? std::string("for|") + source + "|" : std::string();
}

// The settings of a kind that were set in the filter, under their own names.
std::string own_json(obs_data_t *settings, const std::string &prefix)
{
	obs_data_t *out = obs_data_create();
	bool any = false;
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		if (!name || strncmp(name, prefix.c_str(), prefix.size()) != 0 || !obs_data_item_has_user_value(item))
			continue;
		const char *as = name + prefix.size();
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			obs_data_set_string(out, as, obs_data_item_get_string(item));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(out, as, obs_data_item_get_bool(item));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_DOUBLE)
				obs_data_set_double(out, as, obs_data_item_get_double(item));
			else
				obs_data_set_int(out, as, obs_data_item_get_int(item));
			break;
		default:
			continue;
		}
		any = true;
	}
	const char *json = any ? obs_data_get_json(out) : nullptr;
	std::string text = json ? json : "";
	obs_data_release(out);
	return text;
}

ItemWay way_read(obs_data_t *settings, const std::string &scope, bool show)
{
	ItemWay way;
	way.kind = obs_data_get_string(settings, key(scope, show ? "transition" : "hide_transition").c_str());
	way.ms = (uint32_t)std::clamp<long long>(
		obs_data_get_int(settings, key(scope, show ? "transition_ms" : "hide_ms").c_str()), 0, 10000);
	if (way.kind != kTransitionCut && way.kind != kTransitionKeep && way.kind != kUsual && way.kind != kSame &&
	    way.kind != kTransitionScene)
		way.own = own_json(settings, own_prefix(scope, show, way.kind));
	// A Morph also gets the links made in this filter: which sources change
	// into each other.
	if (way.kind == kMorphId) {
		obs_data_array_t *links = obs_data_get_array(settings, kMorphLinks);
		if (links && obs_data_array_count(links) > 0) {
			obs_data_t *own = way.own.empty() ? obs_data_create()
							  : obs_data_create_from_json(way.own.c_str());
			if (own) {
				obs_data_set_array(own, "links", links);
				const char *json = obs_data_get_json(own);
				way.own = json ? json : "";
				obs_data_release(own);
			}
		}
		obs_data_array_release(links);
	}
	return way;
}

// A direction of StageKit's own in the lists of a slide and a swipe: towards
// whichever edge of the screen the item is nearest to. The owner: "for slide
// show in scenes filter, slide off the nearest edge should be an option".
inline constexpr const char *kNearest = "nearest";

bool has_direction(const std::string &kind)
{
	return kind == "slide_transition" || kind == "swipe_transition";
}

// Which way an item goes off the nearest edge of its scene, in a slide's
// words. To hide it moves towards that edge; to show it comes in from it,
// which is moving the other way.
const char *off_nearest_edge(obs_sceneitem_t *item, obs_source_t *scene, bool show, bool as_it_is = false)
{
	if (!scene)
		scene = obs_scene_get_source(obs_sceneitem_get_scene(item));
	const double w = scene ? obs_source_get_width(scene) : 0.0, h = scene ? obs_source_get_height(scene) : 0.0;
	if (w < 1.0 || h < 1.0)
		return show ? "right" : "left";
	// (`as_it_is`: by the box OBS last worked out, with nothing signalled; for
	// a caller on OBS's drawing thread. See `refresh_item`.)
	if (!as_it_is)
		refresh_item(item);
	const Box box = scene_box(scene, item, as_it_is);
	// How far each way until it is all off the screen.
	const double left = box.x + box.w, right = w - box.x, up = box.y + box.h, down = h - box.y;
	const double least = std::min({left, right, up, down});
	if (least == left)
		return show ? "right" : "left";
	if (least == right)
		return show ? "left" : "right";
	if (least == up)
		return show ? "down" : "up";
	return show ? "up" : "down";
}

// A way's own settings as they are to be set on this item: "Off the
// nearest edge" made a direction. Null for none; released by the caller.
obs_data_t *own_for(obs_sceneitem_t *item, bool show, const ItemWay &way, obs_source_t *scene)
{
	obs_data_t *own = way.own.empty() ? nullptr : obs_data_create_from_json(way.own.c_str());
	if (own && has_direction(way.kind) && strcmp(obs_data_get_string(own, "direction"), kNearest) == 0) {
		obs_data_set_string(own, "direction", off_nearest_edge(item, scene, show));
		// (A swipe only moves the item on its way in when told it is one.)
		if (way.kind == "swipe_transition")
			obs_data_set_bool(own, "swipe_in", show);
	}
	return own;
}

// Whether one value of two settings is the same.
bool same_value(obs_data_t *a, obs_data_t *b, const char *name)
{
	obs_data_item_t *x = obs_data_item_byname(a, name);
	obs_data_item_t *y = obs_data_item_byname(b, name);
	bool same = x && y && obs_data_item_gettype(x) == obs_data_item_gettype(y);
	if (same) {
		switch (obs_data_item_gettype(x)) {
		case OBS_DATA_STRING:
			same = strcmp(obs_data_item_get_string(x), obs_data_item_get_string(y)) == 0;
			break;
		case OBS_DATA_BOOLEAN:
			same = obs_data_item_get_bool(x) == obs_data_item_get_bool(y);
			break;
		case OBS_DATA_NUMBER:
			same = obs_data_item_get_double(x) == obs_data_item_get_double(y);
			break;
		default: {
			// An array or an object: compared written out.
			obs_data_t *wa = obs_data_create(), *wb = obs_data_create();
			if (obs_data_item_gettype(x) == OBS_DATA_ARRAY) {
				obs_data_array_t *ax = obs_data_item_get_array(x), *ay = obs_data_item_get_array(y);
				obs_data_set_array(wa, "v", ax);
				obs_data_set_array(wb, "v", ay);
				obs_data_array_release(ax);
				obs_data_array_release(ay);
			} else {
				obs_data_t *ox = obs_data_item_get_obj(x), *oy = obs_data_item_get_obj(y);
				obs_data_set_obj(wa, "v", ox);
				obs_data_set_obj(wb, "v", oy);
				obs_data_release(ox);
				obs_data_release(oy);
			}
			const char *ja = obs_data_get_json(wa), *jb = obs_data_get_json(wb);
			same = ja && jb && strcmp(ja, jb) == 0;
			obs_data_release(wa);
			obs_data_release(wb);
			break;
		}
		}
	}
	obs_data_item_release(&x);
	obs_data_item_release(&y);
	return same;
}

// Whether an item carries a way already: its kind, its length, and exactly
// the settings the way sets, no more.
bool way_matches(obs_sceneitem_t *item, bool show, const ItemWay &way, obs_source_t *scene)
{
	if (way.kind == kTransitionKeep || way.kind.empty())
		return true;
	obs_source_t *now = obs_sceneitem_get_transition(item, show);
	if (way.kind == kTransitionCut)
		return !now;
	const char *now_id = now ? obs_source_get_unversioned_id(now) : nullptr;
	if (!now_id || way.kind != now_id || obs_sceneitem_get_transition_duration(item, show) != way.ms)
		return false;
	obs_data_t *own = own_for(item, show, way, scene);
	obs_data_t *theirs = obs_source_get_settings(now);
	bool same = true;
	obs_data_item_t *it = own ? obs_data_first(own) : nullptr;
	for (; it && same; obs_data_item_next(&it))
		same = same_value(own, theirs, obs_data_item_get_name(it));
	obs_data_item_release(&it);
	// (and nothing set that the way does not set)
	for (it = obs_data_first(theirs); it && same; obs_data_item_next(&it))
		same = !obs_data_item_has_user_value(it) ||
		       (own && obs_data_has_user_value(own, obs_data_item_get_name(it)));
	obs_data_item_release(&it);
	obs_data_release(theirs);
	obs_data_release(own);
	return same;
}

void apply_way(obs_sceneitem_t *item, bool show, const ItemWay &way, obs_source_t *scene)
{
	if (way.kind == kTransitionKeep || way.kind.empty() || way.kind == kTransitionScene)
		return;
	obs_source_t *now = obs_sceneitem_get_transition(item, show);
	if (way.kind == kTransitionCut) {
		if (now)
			obs_sceneitem_set_transition(item, show, nullptr);
		return;
	}
	obs_data_t *own = own_for(item, show, way, scene);
	const char *now_id = now ? obs_source_get_unversioned_id(now) : nullptr;
	if (!now_id || way.kind != now_id) {
		const char *of = obs_source_get_name(obs_sceneitem_get_source(item));
		const std::string name = std::string(of ? of : "") + (show ? " Show Transition" : " Hide Transition");
		obs_source_t *made = obs_source_create_private(way.kind.c_str(), name.c_str(), own);
		if (made) { // (none: that kind of transition is not installed)
			obs_sceneitem_set_transition(item, show, made);
			obs_source_release(made);
		}
	} else if (own) {
		// The same kind as it has: its settings are made these, and no others.
		obs_source_reset_settings(now, own);
	}
	obs_data_release(own);
	if (obs_sceneitem_get_transition(item, show) && obs_sceneitem_get_transition_duration(item, show) != way.ms)
		obs_sceneitem_set_transition_duration(item, show, way.ms);
}

// The kinds of transition there are to choose from: As the scene sets
// first, unless the list is the Scene Transitions filter's own.
void add_kinds(obs_property_t *list, bool scene_entry)
{
	if (scene_entry)
		obs_property_list_add_string(list, obs_module_text("Change.Scene"), kTransitionScene);
	obs_property_list_add_string(list, obs_module_text("Change.Cut"), kTransitionCut);
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		// (Not the ones that need a file of their own.)
		if (!id || strcmp(id, "cut_transition") == 0 || strcmp(id, "obs_stinger_transition") == 0)
			continue;
		const char *shown_as = obs_source_get_display_name(id);
		obs_property_list_add_string(list, shown_as ? shown_as : id, id);
	}
	obs_property_list_add_string(list, obs_module_text("Change.Keep"), kTransitionKeep);
}

// A kind's own settings, copied from that kind's own settings window into
// this one under names of this filter's, with the kind's own defaults.
void add_own(obs_properties_t *into, obs_data_t *settings, const std::string &prefix, const std::string &kind)
{
	obs_properties_t *theirs = obs_get_source_properties(kind.c_str());
	if (!theirs)
		return;
	obs_data_t *defaults = obs_get_source_defaults(kind.c_str());
	// (Controls inside a transition's own headings are copied too, Morph's
	// for one; never a list of rows, Morph's rules, its links or the group
	// its own Unpaired transition's settings go in.)
	std::vector<obs_property_t *> all;
	const std::function<void(obs_properties_t *)> gather = [&](obs_properties_t *level) {
		for (obs_property_t *p = obs_properties_first(level); p; obs_property_next(&p)) {
			const char *name = obs_property_name(p);
			if (obs_property_get_type(p) != OBS_PROPERTY_GROUP) {
				all.push_back(p);
				continue;
			}
			const size_t len = name ? strlen(name) : 0;
			const bool rows = len >= 6 && (strcmp(name + len - 6, "|alist") == 0 ||
						       strcmp(name + len - 6, "|tlist") == 0);
			if (!name || !obs_property_visible(p) || rows || strcmp(name, "unpaired|g") == 0 ||
			    strcmp(name, "rule_group") == 0 || strcmp(name, "links_group") == 0)
				continue;
			gather(obs_property_group_content(p));
		}
	};
	gather(theirs);
	for (obs_property_t *p : all) {
		const char *their_name = obs_property_name(p);
		const char *text = obs_property_description(p);
		if (!their_name || !obs_property_visible(p))
			continue;
		// Morph's links are made once for the whole filter, further down;
		// and what it calls "Other items" (items in one scene only, in a
		// change of scene) never happens to an item: *Unpaired sources* is
		// its way here.
		const bool morph = kind == kMorphId;
		if (morph && (strcmp(their_name, "link_a") == 0 || strcmp(their_name, "link_b") == 0 ||
			      strcmp(their_name, "others") == 0))
			continue;
		// Make Room, as an item's own, never pairs: its pairing and links
		// are for a change of scene only.
		if (kind == kMakeRoomId &&
		    (strcmp(their_name, "link_a") == 0 || strcmp(their_name, "link_b") == 0 ||
		     strcmp(their_name, "pair_main") == 0 || strcmp(their_name, "visible_pixels") == 0 ||
		     strcmp(their_name, "no_shadows") == 0))
			continue;
		const std::string name = prefix + their_name;
		obs_property_t *made = nullptr;
		switch (obs_property_get_type(p)) {
		case OBS_PROPERTY_BOOL:
			made = obs_properties_add_bool(into, name.c_str(), text);
			if (defaults)
				obs_data_set_default_bool(settings, name.c_str(),
							  obs_data_get_bool(defaults, their_name));
			break;
		case OBS_PROPERTY_INT:
			made = obs_property_int_type(p) == OBS_NUMBER_SLIDER
				       ? obs_properties_add_int_slider(into, name.c_str(), text,
								       obs_property_int_min(p), obs_property_int_max(p),
								       obs_property_int_step(p))
				       : obs_properties_add_int(into, name.c_str(), text, obs_property_int_min(p),
								obs_property_int_max(p), obs_property_int_step(p));
			if (obs_property_int_suffix(p))
				obs_property_int_set_suffix(made, obs_property_int_suffix(p));
			if (defaults)
				obs_data_set_default_int(settings, name.c_str(),
							 obs_data_get_int(defaults, their_name));
			break;
		case OBS_PROPERTY_FLOAT:
			made = obs_property_float_type(p) == OBS_NUMBER_SLIDER
				       ? obs_properties_add_float_slider(into, name.c_str(), text,
									 obs_property_float_min(p),
									 obs_property_float_max(p),
									 obs_property_float_step(p))
				       : obs_properties_add_float(into, name.c_str(), text, obs_property_float_min(p),
								  obs_property_float_max(p),
								  obs_property_float_step(p));
			if (obs_property_float_suffix(p))
				obs_property_float_set_suffix(made, obs_property_float_suffix(p));
			if (defaults)
				obs_data_set_default_double(settings, name.c_str(),
							    obs_data_get_double(defaults, their_name));
			break;
		case OBS_PROPERTY_COLOR:
		case OBS_PROPERTY_COLOR_ALPHA:
			made = obs_property_get_type(p) == OBS_PROPERTY_COLOR
				       ? obs_properties_add_color(into, name.c_str(), text)
				       : obs_properties_add_color_alpha(into, name.c_str(), text);
			if (defaults)
				obs_data_set_default_int(settings, name.c_str(),
							 obs_data_get_int(defaults, their_name));
			break;
		case OBS_PROPERTY_LIST: {
			const obs_combo_format format = obs_property_list_format(p);
			if (format != OBS_COMBO_FORMAT_STRING && format != OBS_COMBO_FORMAT_INT &&
			    format != OBS_COMBO_FORMAT_FLOAT)
				break;
			made = obs_properties_add_list(into, name.c_str(), text, OBS_COMBO_TYPE_LIST, format);
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				const char *item = obs_property_list_item_name(p, i);
				if (format == OBS_COMBO_FORMAT_STRING)
					obs_property_list_add_string(made, item, obs_property_list_item_string(p, i));
				else if (format == OBS_COMBO_FORMAT_INT)
					obs_property_list_add_int(made, item, obs_property_list_item_int(p, i));
				else
					obs_property_list_add_float(made, item, obs_property_list_item_float(p, i));
			}
			// One more way to go, for a slide and a swipe.
			if (format == OBS_COMBO_FORMAT_STRING && has_direction(kind) &&
			    strcmp(their_name, "direction") == 0)
				obs_property_list_add_string(made, obs_module_text("Change.Nearest"), kNearest);
			if (!defaults)
				break;
			if (format == OBS_COMBO_FORMAT_STRING)
				obs_data_set_default_string(settings, name.c_str(),
							    obs_data_get_string(defaults, their_name));
			else if (format == OBS_COMBO_FORMAT_INT)
				obs_data_set_default_int(settings, name.c_str(),
							 obs_data_get_int(defaults, their_name));
			else
				obs_data_set_default_double(settings, name.c_str(),
							    obs_data_get_double(defaults, their_name));
			break;
		}
		default:
			break; // a file, a font, a button: not offered here
		}
		if (made && morph && strcmp(their_name, "pair_main") == 0)
			set_tip(made, obs_module_text("Morph.Item.Tip"));
		else if (made && obs_property_long_description(p))
			set_tip(made, obs_property_long_description(p));
	}
	obs_data_release(defaults);
	obs_properties_destroy(theirs);
	// A filter made before *Unpaired sources* had "Other items" (shown as
	// "With nothing to change into") in its place: Fade and zoom there reads
	// as Fade and zoom here, as Morph itself reads it.
	if (kind == kMorphId && obs_data_has_user_value(settings, (prefix + "others").c_str()) &&
	    !obs_data_has_user_value(settings, (prefix + "unpaired").c_str()))
		obs_data_set_default_string(
			settings, (prefix + "unpaired").c_str(),
			obs_data_get_int(settings, (prefix + "others").c_str()) == 1 ? "morph_zoom" : "morph_fade");
}

// Property names: "change|g|<list setting>" is the group, "change|for" the
// "Settings for" box; everything else in the group is rebuilt whenever the
// box, or one of the two kinds, changes.
inline constexpr const char *kGroupStart = "change|g|";
inline constexpr const char *kForName = "change|for";

bool rebuild_cb(void *priv, obs_properties_t *top, obs_property_t *, obs_data_t *settings);

// The group the controls are in, wherever it is: straight in the window or
// inside another group (Scene Standby's "When a source is hidden").
obs_property_t *find_group(obs_properties_t *top, std::string &list_setting)
{
	for (obs_property_t *p = obs_properties_first(top); p; obs_property_next(&p)) {
		const char *name = obs_property_name(p);
		if (name && strncmp(name, kGroupStart, strlen(kGroupStart)) == 0) {
			list_setting = name + strlen(kGroupStart);
			return p;
		}
		if (obs_property_get_type(p) != OBS_PROPERTY_GROUP)
			continue;
		if (obs_property_t *inside = find_group(obs_property_group_content(p), list_setting))
			return inside;
	}
	return nullptr;
}

void build(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings)
{
	std::string list_setting;
	obs_property_t *group = find_group(top, list_setting);
	obs_properties_t *content = group ? obs_property_group_content(group) : nullptr;
	if (!content || !settings)
		return;
	// The listed sources, in the "Settings for" box.
	// (`list_setting` may name several lists, with commas between: Link
	// Visibility's own items and its sources in other scenes.)
	std::vector<std::string> listed;
	for (size_t start = 0; start < list_setting.size();) {
		size_t comma = list_setting.find(',', start);
		if (comma == std::string::npos)
			comma = list_setting.size();
		for (const std::string &name :
		     pick_list_read(settings, list_setting.substr(start, comma - start).c_str(), false))
			if (std::find(listed.begin(), listed.end(), name) == listed.end())
				listed.push_back(name);
		start = comma + 1;
	}
	const char *kind = owner ? obs_source_get_unversioned_id(owner) : nullptr;
	if (kind && strcmp(kind, kSceneTransitionsId) == 0 && obs_data_get_int(settings, "cover") == 0) {
		std::vector<std::string> scene_names;
		scene_item_names(obs_filter_get_parent(owner), scene_names);
		for (const std::string &name : scene_names)
			if (std::find(listed.begin(), listed.end(), name) == listed.end())
				listed.push_back(name);
	}
	std::string chosen = obs_data_get_string(settings, kForName);
	if (std::find(listed.begin(), listed.end(), chosen) == listed.end())
		chosen.clear();
	if (obs_property_t *for_box = obs_properties_get(content, kForName)) {
		obs_property_list_clear(for_box);
		obs_property_list_add_string(for_box, obs_module_text("Change.For.All"), "");
		for (const std::string &name : listed)
			obs_property_list_add_string(for_box, name.c_str(), name.c_str());
		obs_property_set_visible(for_box, !listed.empty());
	}
	const std::string scope = scope_of(chosen.c_str());
	if (!scope.empty()) {
		obs_data_set_default_string(settings, key(scope, "transition").c_str(), kUsual);
		obs_data_set_default_int(settings, key(scope, "transition_ms").c_str(), 300);
		obs_data_set_default_string(settings, key(scope, "hide_transition").c_str(), kSame);
		obs_data_set_default_int(settings, key(scope, "hide_ms").c_str(), 300);
	}
	// What the controls are made for: the source chosen, the two kinds and,
	// under Morph, the transition chosen for Unpaired sources. Already made
	// for exactly that, nothing is made again. (OBS calls every control's
	// callback once whenever it builds a window, from a list it took
	// beforehand: a control taken off by an earlier callback would be called
	// after it was gone.)
	// The mark is added once everything is made: making the controls sets
	// defaults, and a mark taken before them could fail to match at OBS's
	// first round of callbacks, which then took the controls off while it
	// was still going through them (a crash, beta162).
	auto signature = [&]() {
		std::string sig = "change|sig|" + chosen;
		for (const bool show : {true, false}) {
			const std::string kind = obs_data_get_string(
				settings, key(scope, show ? "transition" : "hide_transition").c_str());
			sig += "|" + kind;
			if (kind == kMorphId)
				sig += "|" + std::string(obs_data_get_string(
						     settings, (own_prefix(scope, show, kind) + "unpaired").c_str()));
		}
		return sig;
	};
	if (obs_properties_get(content, signature().c_str()))
		return;
	// Everything but that box goes, and is made again for what is chosen.
	std::vector<std::string> old;
	for (obs_property_t *p = obs_properties_first(content); p; obs_property_next(&p))
		if (strcmp(obs_property_name(p), kForName) != 0)
			old.push_back(obs_property_name(p));
	for (const std::string &name : old)
		obs_properties_remove_by_name(content, name.c_str());

	// The Scene Transitions filter is what "As the scene sets" asks; its own
	// lists do not offer it.
	const char *owner_id = owner ? obs_source_get_unversioned_id(owner) : nullptr;
	const bool scene_entry = !owner_id || strcmp(owner_id, kSceneTransitionsId) != 0;
	const bool visibility_switch = owner_id && strcmp(owner_id, kShowInScenesId) == 0;
	for (const bool show : {true, false}) {
		const std::string kind_name = key(scope, show ? "transition" : "hide_transition");
		const std::string ms_name = key(scope, show ? "transition_ms" : "hide_ms");
		obs_property_t *kinds = obs_properties_add_list(
			content, kind_name.c_str(), obs_module_text(show ? "Change.ShowWith" : "Change.HideWith"),
			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		if (show && !scope.empty())
			obs_property_list_add_string(kinds, obs_module_text("Change.Usual"), kUsual);
		if (!show)
			obs_property_list_add_string(kinds, obs_module_text("Change.HideSame"), kSame);
		add_kinds(kinds, scene_entry);
		std::string tip = obs_module_text(show ? "Change.ShowWith.Tip" : "Change.HideWith.Tip");
		if (scene_entry)
			tip = std::string(obs_module_text(visibility_switch ? "ShowIn.SceneTransition.Tip" : "Change.Scene.Tip")) + " " + tip;
		set_tip(kinds, tip.c_str());
		obs_property_set_modified_callback2(kinds, rebuild_cb, owner);
		const std::string kind = obs_data_get_string(settings, kind_name.c_str());
		const bool timed = kind != kTransitionCut && kind != kTransitionKeep && kind != kUsual &&
				   kind != kSame && !kind.empty();
		if (!timed)
			continue;
		obs_property_t *time = obs_properties_add_int_slider(
			content, ms_name.c_str(), obs_module_text(show ? "Change.ShowTime" : "Change.HideTime"), 50,
			5000, 50);
		obs_property_int_set_suffix(time, " ms");
		set_tip(time, obs_module_text("Change.Duration.Tip"));
		if (kind == kTransitionScene) {
			set_tip(time, obs_module_text(visibility_switch ? "ShowIn.FallbackTime.Tip" : "Change.Time.Tip"));
			continue; // (its own settings are the scene's)
		}
		const std::string prefix = own_prefix(scope, show, kind);
		add_own(content, settings, prefix, kind);
		if (kind != kMorphId)
			continue;
		// Controls that do nothing here are hidden (they stay, by name).
		// Make Room's two only count with Make room under "Unpaired: show or
		// hide" (the controls are made again when that changes); and how
		// two sources are matched is read from the one being shown, so a
		// Morph for hiding does not repeat it.
		const bool room = strcmp(obs_data_get_string(settings, (prefix + "unpaired").c_str()), kUnpairedRoom) ==
				  0;
		for (const char *name : {"room_way", "room_fade"})
			if (obs_property_t *p = obs_properties_get(content, (prefix + name).c_str()))
				obs_property_set_visible(p, room);
		if (!show)
			for (const char *name : {"pair_main", "visible_pixels", "no_shadows"})
				if (obs_property_t *p = obs_properties_get(content, (prefix + name).c_str()))
					obs_property_set_visible(p, false);
		// Morph's *Unpaired sources*: another transition, with its own
		// settings under it, rebuilt in place when it changes.
		const std::string unpaired_name = prefix + "unpaired";
		if (obs_property_t *unpaired = obs_properties_get(content, unpaired_name.c_str()))
			obs_property_set_modified_callback2(unpaired, rebuild_cb, owner);
		const std::string fallback = obs_data_get_string(settings, unpaired_name.c_str());
		if (item_transition_is_fallback_kind(fallback.c_str())) {
			// A line that says whose settings follow, when it has any.
			const std::string own = prefix + "unpaired|" + fallback + "|";
			const std::string heading = own + "heading";
			obs_property_t *line = obs_properties_add_text(
				content, heading.c_str(), obs_module_text("Morph.AloneOwn"), OBS_TEXT_INFO);
			add_own(content, settings, own, fallback);
			obs_property_t *last = line;
			obs_property_next(&last);
			if (!last)
				obs_properties_remove_by_name(content, heading.c_str());
		}
	}
	// For Morph: which sources change into each other. In the group, after
	// the transitions they belong to, and shown only while Morph is chosen
	// (show_links). Made again with the rest; what is chosen in them is in
	// the settings.
	add_morph_link_properties(content, owner, kMorphPrefix);
	// (A control of no kind that shows nothing: the mark of what was made.)
	obs_property_set_visible(obs_properties_add_text(content, signature().c_str(), "", OBS_TEXT_INFO), false);
}

// The boxes and the list that Morph's links are made with show only while
// Morph is chosen for something in this filter.
void show_links(obs_properties_t *top, obs_data_t *settings)
{
	bool any = false;
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		const char *name = obs_data_item_get_name(item);
		const size_t len = name ? strlen(name) : 0;
		// "transition", "hide_transition", and the same after "for|<source>|".
		if (len < 10 || strcmp(name + len - 10, "transition") != 0 ||
		    obs_data_item_gettype(item) != OBS_DATA_STRING)
			continue;
		any = any || strcmp(obs_data_item_get_string(item), kMorphId) == 0;
	}
	const std::string prefix = kMorphPrefix;
	for (const std::string &name :
	     {prefix + "link_a", prefix + "link_b", prefix + "link_add", std::string(kMorphLinks) + "|alist"})
		if (obs_property_t *p = obs_properties_get(top, name.c_str()))
			obs_property_set_visible(p, any);
}

bool rebuild_cb(void *priv, obs_properties_t *top, obs_property_t *, obs_data_t *settings)
{
	build(top, static_cast<obs_source_t *>(priv), settings);
	show_links(top, settings);
	return true;
}

} // namespace

ItemChange item_change_read(obs_data_t *settings, const char *source)
{
	ItemChange usual;
	usual.show = way_read(settings, "", true);
	usual.hide = way_read(settings, "", false);
	if (usual.show.kind.empty() || usual.show.kind == kUsual)
		usual.show = ItemWay{};
	if (usual.hide.kind.empty() || usual.hide.kind == kSame)
		usual.hide = usual.show;
	const std::string scope = scope_of(source);
	if (scope.empty())
		return usual;
	ItemChange own;
	own.show = way_read(settings, scope, true);
	own.hide = way_read(settings, scope, false);
	const bool show_usual = own.show.kind.empty() || own.show.kind == kUsual;
	const bool hide_same = own.hide.kind.empty() || own.hide.kind == kSame;
	if (show_usual)
		own.show = usual.show;
	// Hiding "the same": as this source shows, when that is its own; with
	// nothing of its own at all, as the filter usually hides.
	if (hide_same)
		own.hide = show_usual ? usual.hide : own.show;
	return own;
}

void apply_item_change(obs_sceneitem_t *item, const ItemChange &asked, obs_source_t *scene)
{
	if (!item || owned(item))
		return;
	const ItemChange change = resolve_item_change(item, asked, scene);
	apply_way(item, true, change.show, scene);
	apply_way(item, false, change.hide, scene);
}

bool set_item_visible(obs_sceneitem_t *item, bool show, const ItemChange &asked, obs_source_t *scene)
{
	if (!item || obs_sceneitem_visible(item) == show)
		return false;
	if (!owned(item)) {
		const ItemChange change = resolve_change(item, asked, scene, true);
		apply_way(item, show, show ? change.show : change.hide, scene);
	}
	return set_item_visible(item, show);
}

void apply_item_change_as_owner(obs_sceneitem_t *item, const ItemChange &asked, obs_source_t *scene)
{
	if (!item)
		return;
	const ItemChange change = resolve_item_change(item, asked, scene);
	apply_way(item, true, change.show, scene);
	apply_way(item, false, change.hide, scene);
}

bool item_change_matches(obs_sceneitem_t *item, const ItemChange &asked, obs_source_t *scene)
{
	if (!item)
		return false;
	const ItemChange change = resolve_item_change(item, asked, scene);
	return way_matches(item, true, change.show, scene) && way_matches(item, false, change.hide, scene);
}

void add_item_change_properties(obs_properties_t *props, obs_source_t *owner, const char *list_setting)
{
	// When the list of sources changes, the "Settings for" box follows.
	pick_list_on_change([](obs_properties_t *top, obs_source_t *list_owner, obs_data_t *settings) {
		build(top, list_owner, settings);
		show_links(top, settings);
	});
	obs_properties_t *content = obs_properties_create();
	obs_property_t *for_box = obs_properties_add_list(content, kForName, obs_module_text("Change.For"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	set_tip(for_box, obs_module_text("Change.For.Tip"));
	obs_property_set_modified_callback2(for_box, rebuild_cb, owner);
	const std::string group_name = std::string(kGroupStart) + (list_setting ? list_setting : "");
	obs_properties_add_group(props, group_name.c_str(), obs_module_text("Change.Group"), OBS_GROUP_NORMAL, content);
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	build(props, owner, settings);
	if (settings)
		show_links(props, settings);
	else
		add_morph_link_properties(content, owner, kMorphPrefix); // (a window of no filter)
	obs_data_release(settings);
}

void refresh_item_change_properties(obs_properties_t *props, obs_source_t *owner, obs_data_t *settings)
{
	build(props, owner, settings);
	show_links(props, settings);
}

void item_transition_add_own(obs_properties_t *into, obs_data_t *settings, const std::string &prefix,
			     const std::string &kind)
{
	add_own(into, settings, prefix, kind);
}

std::string item_transition_own_json(obs_data_t *settings, const std::string &prefix)
{
	return own_json(settings, prefix);
}

const char *item_off_nearest_edge(obs_sceneitem_t *item, obs_source_t *scene, bool show, bool as_it_is)
{
	return off_nearest_edge(item, scene, show, as_it_is);
}

bool item_transition_is_fallback_kind(const char *kind)
{
	if (!kind || !*kind || strcmp(kind, kTransitionCut) == 0 || strcmp(kind, kTransitionKeep) == 0 ||
	    strcmp(kind, kTransitionScene) == 0 || strcmp(kind, kMorphId) == 0 || strcmp(kind, kMakeRoomId) == 0 ||
	    strcmp(kind, "cut_transition") == 0 || strcmp(kind, "obs_stinger_transition") == 0 ||
	    strncmp(kind, "morph_", 6) == 0)
		return false;
	return obs_source_get_display_name(kind) != nullptr;
}

void item_change_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "transition", kTransitionScene);
	obs_data_set_default_int(settings, "transition_ms", 300);
	obs_data_set_default_string(settings, "hide_transition", kSame);
	obs_data_set_default_int(settings, "hide_ms", 300);
	obs_data_set_default_string(settings, kForName, "");
}

} // namespace stagekit
