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
#include "scene-util.hpp"
#include "../transition/morph-paint.hpp"
#include "../transition/morph.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace stagekit {

void set_item_transitions(obs_sceneitem_t *item, const std::string &kind, uint32_t ms)
{
	if (!item || kind == kTransitionKeep)
		return;
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

void add_item_transition_properties(obs_properties_t *props)
{
	// Any transition OBS has, except the ones that need a file of their own.
	obs_property_t *transition = obs_properties_add_list(props, "transition", obs_module_text("Change.With"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(transition, obs_module_text("Change.Cut"), kTransitionCut);
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &id); i++) {
		if (!id || strcmp(id, "cut_transition") == 0 || strcmp(id, "obs_stinger_transition") == 0)
			continue;
		const char *shown_as = obs_source_get_display_name(id);
		obs_property_list_add_string(transition, shown_as ? shown_as : id, id);
	}
	obs_property_list_add_string(transition, obs_module_text("Change.Keep"), kTransitionKeep);
	obs_property_set_long_description(transition, obs_module_text("Change.Tip"));
	obs_property_set_modified_callback(transition, [](obs_properties_t *p, obs_property_t *, obs_data_t *settings) {
		const std::string kind = obs_data_get_string(settings, "transition");
		obs_property_set_visible(obs_properties_get(p, "transition_ms"),
					 kind != kTransitionCut && kind != kTransitionKeep);
		return true;
	});
	obs_property_t *time =
		obs_properties_add_int_slider(props, "transition_ms", obs_module_text("Change.Time"), 50, 5000, 50);
	obs_property_int_set_suffix(time, " ms");
}

void item_transition_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "transition", kTransitionFade);
	obs_data_set_default_int(settings, "transition_ms", 300);
}

std::string item_transition_kind(obs_data_t *settings)
{
	return obs_data_get_string(settings, "transition");
}

uint32_t item_transition_ms(obs_data_t *settings)
{
	return (uint32_t)std::clamp<long long>(obs_data_get_int(settings, "transition_ms"), 0, 10000);
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
	if (way.kind != kTransitionCut && way.kind != kTransitionKeep && way.kind != kUsual && way.kind != kSame)
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
const char *off_nearest_edge(obs_sceneitem_t *item, obs_source_t *scene, bool show)
{
	if (!scene)
		scene = obs_scene_get_source(obs_sceneitem_get_scene(item));
	const double w = scene ? obs_source_get_width(scene) : 0.0, h = scene ? obs_source_get_height(scene) : 0.0;
	if (w < 1.0 || h < 1.0)
		return show ? "right" : "left";
	refresh_item(item);
	const Box box = scene_box(scene, item);
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

void apply_way(obs_sceneitem_t *item, bool show, const ItemWay &way, obs_source_t *scene)
{
	if (way.kind == kTransitionKeep || way.kind.empty())
		return;
	obs_source_t *now = obs_sceneitem_get_transition(item, show);
	if (way.kind == kTransitionCut) {
		if (now)
			obs_sceneitem_set_transition(item, show, nullptr);
		return;
	}
	obs_data_t *own = way.own.empty() ? nullptr : obs_data_create_from_json(way.own.c_str());
	if (own && has_direction(way.kind) && strcmp(obs_data_get_string(own, "direction"), kNearest) == 0) {
		obs_data_set_string(own, "direction", off_nearest_edge(item, scene, show));
		// (A swipe only moves the item on its way in when told it is one.)
		if (way.kind == "swipe_transition")
			obs_data_set_bool(own, "swipe_in", show);
	}
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

// The kinds of transition there are to choose from.
void add_kinds(obs_property_t *list)
{
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
	for (obs_property_t *p = obs_properties_first(theirs); p; obs_property_next(&p)) {
		const char *their_name = obs_property_name(p);
		const char *text = obs_property_description(p);
		if (!their_name || !obs_property_visible(p))
			continue;
		// Morph's links are made once for the whole filter, further down;
		// and what it calls "Other items" is here the item by itself.
		const bool morph = kind == kMorphId;
		if (morph && (strcmp(their_name, "link_a") == 0 || strcmp(their_name, "link_b") == 0))
			continue;
		if (morph && strcmp(their_name, "others") == 0)
			text = obs_module_text("Morph.Alone");
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
		if (made && morph && strcmp(their_name, "others") == 0)
			obs_property_set_long_description(made, obs_module_text("Morph.Alone.Tip"));
		else if (made && morph && strcmp(their_name, "pair_main") == 0)
			obs_property_set_long_description(made, obs_module_text("Morph.Item.Tip"));
		else if (made && obs_property_long_description(p))
			obs_property_set_long_description(made, obs_property_long_description(p));
	}
	obs_data_release(defaults);
	obs_properties_destroy(theirs);
}

// Property names: "change|g|<list setting>" is the group, "change|for" the
// "Settings for" box; everything else in the group is rebuilt whenever the
// box, or one of the two kinds, changes.
inline constexpr const char *kGroupStart = "change|g|";
inline constexpr const char *kForName = "change|for";

bool rebuild_cb(void *priv, obs_properties_t *top, obs_property_t *, obs_data_t *settings);

obs_property_t *find_group(obs_properties_t *top, std::string &list_setting)
{
	for (obs_property_t *p = obs_properties_first(top); p; obs_property_next(&p)) {
		const char *name = obs_property_name(p);
		if (name && strncmp(name, kGroupStart, strlen(kGroupStart)) == 0) {
			list_setting = name + strlen(kGroupStart);
			return p;
		}
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
	std::vector<std::string> listed;
	if (!list_setting.empty())
		listed = pick_list_read(settings, list_setting.c_str(), false);
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
	// Everything but that box goes, and is made again for what is chosen.
	std::vector<std::string> old;
	for (obs_property_t *p = obs_properties_first(content); p; obs_property_next(&p))
		if (strcmp(obs_property_name(p), kForName) != 0)
			old.push_back(obs_property_name(p));
	for (const std::string &name : old)
		obs_properties_remove_by_name(content, name.c_str());

	const std::string scope = scope_of(chosen.c_str());
	if (!scope.empty()) {
		obs_data_set_default_string(settings, key(scope, "transition").c_str(), kUsual);
		obs_data_set_default_int(settings, key(scope, "transition_ms").c_str(), 300);
		obs_data_set_default_string(settings, key(scope, "hide_transition").c_str(), kSame);
		obs_data_set_default_int(settings, key(scope, "hide_ms").c_str(), 300);
	}
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
		add_kinds(kinds);
		obs_property_set_long_description(kinds, obs_module_text(show ? "Change.ShowWith.Tip"
									      : "Change.HideWith.Tip"));
		obs_property_set_modified_callback2(kinds, rebuild_cb, owner);
		const std::string kind = obs_data_get_string(settings, kind_name.c_str());
		const bool timed = kind != kTransitionCut && kind != kTransitionKeep && kind != kUsual &&
				   kind != kSame && !kind.empty();
		if (!timed)
			continue;
		obs_property_t *time = obs_properties_add_int_slider(content, ms_name.c_str(),
								     obs_module_text("Change.Time"), 50, 5000, 50);
		obs_property_int_set_suffix(time, " ms");
		add_own(content, settings, own_prefix(scope, show, kind), kind);
	}
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

void apply_item_change(obs_sceneitem_t *item, const ItemChange &change, obs_source_t *scene)
{
	if (!item)
		return;
	apply_way(item, true, change.show, scene);
	apply_way(item, false, change.hide, scene);
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
	obs_property_set_long_description(for_box, obs_module_text("Change.For.Tip"));
	obs_property_set_modified_callback2(for_box, rebuild_cb, owner);
	const std::string group_name = std::string(kGroupStart) + (list_setting ? list_setting : "");
	obs_properties_add_group(props, group_name.c_str(), obs_module_text("Change.Group"), OBS_GROUP_NORMAL, content);
	// For Morph: which sources change into each other.
	add_morph_link_properties(props, owner, kMorphPrefix);
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	build(props, owner, settings);
	if (settings)
		show_links(props, settings);
	obs_data_release(settings);
}

void item_change_defaults(obs_data_t *settings)
{
	item_transition_defaults(settings);
	obs_data_set_default_string(settings, "hide_transition", kSame);
	obs_data_set_default_int(settings, "hide_ms", 300);
	obs_data_set_default_string(settings, kForName, "");
}

} // namespace stagekit
