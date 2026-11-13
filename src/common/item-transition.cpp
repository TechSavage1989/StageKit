/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "item-transition.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cstring>

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

} // namespace stagekit
