/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "source-rename.hpp"
#include "hid-items.hpp"
#include "scene-filter-ids.hpp"
#include "../ambient/ambient-background.hpp"
#include "../capture/active-app.hpp"
#include "../capture/app-capture.hpp"
#include "../capture/auto-capture.hpp"
#include "../capture/program-trigger.hpp"
#include "../placement/show-in-scenes.hpp"
#include "../placement/visibility-link.hpp"
#include "../transition/morph.hpp"
#include "../transition/morph-paint.hpp"
#include "../view/cropped-view.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace stagekit {

namespace {

// Copy settings before erasing their previous keys. Transition-specific options
// can contain lists and objects as well as numbers and strings.
void move_prefix(obs_data_t *data, const std::string &from, const std::string &to, bool &changed)
{
	std::vector<std::string> keys;
	for (obs_data_item_t *it = obs_data_first(data); it; obs_data_item_next(&it)) {
		const char *key = obs_data_item_get_name(it);
		if (key && strncmp(key, from.c_str(), from.size()) == 0 && obs_data_item_has_user_value(it))
			keys.emplace_back(key);
	}
	for (const auto &key : keys) {
		const std::string next = to + key.substr(from.size());
		obs_data_item_t *it = obs_data_item_byname(data, key.c_str());
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING:
			obs_data_set_string(data, next.c_str(), obs_data_item_get_string(it));
			break;
		case OBS_DATA_BOOLEAN:
			obs_data_set_bool(data, next.c_str(), obs_data_item_get_bool(it));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_DOUBLE)
				obs_data_set_double(data, next.c_str(), obs_data_item_get_double(it));
			else
				obs_data_set_int(data, next.c_str(), obs_data_item_get_int(it));
			break;
		case OBS_DATA_OBJECT: {
			obs_data_t *object = obs_data_item_get_obj(it);
			obs_data_set_obj(data, next.c_str(), object);
			obs_data_release(object);
			break;
		}
		case OBS_DATA_ARRAY: {
			obs_data_array_t *array = obs_data_item_get_array(it);
			obs_data_set_array(data, next.c_str(), array);
			obs_data_array_release(array);
			break;
		}
		default:
			obs_data_item_release(&it);
			continue;
		}
		obs_data_item_release(&it);
		obs_data_erase(data, key.c_str());
		changed = true;
	}
}

struct Rename {
	const char *was, *now;
	bool scene;
	bool changed = false;

	void scalar(obs_data_t *data, const char *key)
	{
		if (obs_data_has_user_value(data, key) && strcmp(obs_data_get_string(data, key), was) == 0) {
			obs_data_set_string(data, key, now);
			changed = true;
		}
	}

	// Change only complete names. An editable-list row also stores its selected
	// and hidden flags, so leave the row itself in place.
	void list(obs_data_t *data, const char *key, const char *mark = nullptr, bool rule = false)
	{
		obs_data_array_t *rows = obs_data_get_array(data, key);
		for (size_t i = 0; i < obs_data_array_count(rows); ++i) {
			obs_data_t *row = obs_data_array_item(rows, i);
			if (!mark) {
				scalar(row, "value");
			} else {
				const std::string line = obs_data_get_string(row, "value");
				const size_t at = line.find(mark);
				if (at != std::string::npos) {
					const auto a = line.substr(0, at), b = line.substr(at + strlen(mark));
					if (a == was || b == was) {
						const std::string renamed =
							(a == was ? now : a) + mark + (b == was ? now : b);
						if (rule)
							move_prefix(data, "rule|" + line + "|", "rule|" + renamed + "|",
								    changed);
						obs_data_set_string(row, "value", renamed.c_str());
						changed = true;
					}
				}
			}
			obs_data_release(row);
		}
		obs_data_array_release(rows);
		if (!mark)
			scalar(data, (std::string(key) + "|apick").c_str());
	}

	void apply(obs_source_t *owner)
	{
		const std::string id = obs_source_get_unversioned_id(owner);
		obs_data_t *data = obs_source_get_settings(owner);
		changed = false;
		if (id == kCroppedViewId || id == kSceneStandbyId)
			scalar(data, "source");
		if (id == kAmbientBackgroundId) {
			scalar(data, "base_source");
			scalar(data, "live_source");
		}
		if (id == kShowInScenesId) {
			list(data, "sources");
			if (scene) {
				list(data, "scenes");
				scalar(data, "go_to");
			}
			if (show_in_rename_record(data, was, now))
				changed = true;
		}
		if (id == kVisibilityLinkId)
			for (const char *key : {"items", "leads", "elsewhere"})
				list(data, key);
		if (id == kSceneStandbyId)
			for (const char *key : {"sources", "skip", "hidden"})
				list(data, key);
		if (id == kSceneTransitionsId)
			for (const char *key : {"sources", "except"})
				list(data, key);
		if (id == kProgramTriggerId || id == kActiveAppId)
			list(data, "items");
		if (id == kActiveAppId)
			list(data, "by_hand");
		if ((id == kAppCaptureId || id == kAutoCaptureId) && scene) {
			auto record = hid_items_read(data);
			bool touched = false;
			for (auto &item : record) {
				for (auto *name : {&item.scene, &item.in}) {
					if (*name == was) {
						*name = now;
						touched = true;
					}
				}
			}
			if (touched) {
				hid_items_write(data, record);
				changed = true;
			}
		}
		if (id == kMorphId || id == kMakeRoomId) {
			list(data, "links", kLinkMark);
			scalar(data, "link_a");
			scalar(data, "link_b");
			if (scene && strcmp(was, kAnyScene) != 0) {
				scalar(data, "rule_from");
				scalar(data, "rule_to");
				list(data, "rules", kRuleMark, true);
			}
		}

		// The full Show/Hide transition group uses the same setting names in
		// every filter. Its per-source options include third-party settings.
		move_prefix(data, std::string("for|") + was + "|", std::string("for|") + now + "|", changed);
		scalar(data, "change|for");
		scalar(data, "morph_link_a");
		scalar(data, "morph_link_b");
		list(data, "morph_links", kLinkMark);
		std::vector<std::string> scoped_links;
		for (obs_data_item_t *it = obs_data_first(data); it; obs_data_item_next(&it)) {
			const std::string key = obs_data_item_get_name(it);
			if (key.compare(0, 4, "for|") == 0 && key.size() > 12 &&
			    key.compare(key.size() - 12, 12, "|morph_links") == 0 &&
			    obs_data_item_gettype(it) == OBS_DATA_ARRAY)
				scoped_links.push_back(key);
		}
		for (const auto &key : scoped_links)
			list(data, key.c_str(), kLinkMark);
		if (changed)
			obs_source_update(owner, data);
		obs_data_release(data);
	}
};

} // namespace

void source_references_renamed(void *, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	const char *was = calldata_string(cd, "prev_name"), *now = calldata_string(cd, "new_name");
	if (!source || !was || !now || !*was || !*now || strcmp(was, now) == 0 || obs_obj_is_private(source))
		return;
	const bool scene = obs_group_or_scene_from_source(source) != nullptr;
	if (!scene && obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT)
		return;
	// OBS holds the global source list lock while enumerating. Hold each owner,
	// then update it after enumeration so other filters can safely query OBS.
	std::vector<obs_source_t *> owners;
	obs_enum_all_sources(
		[](void *param, obs_source_t *candidate) {
			const char *id = obs_source_get_unversioned_id(candidate);
			if (!id)
				return true;
			for (const char *kind :
			     {kCroppedViewId, kAmbientBackgroundId, kMorphId, kMakeRoomId, kShowInScenesId,
			      kVisibilityLinkId, kSceneStandbyId, kSceneTransitionsId, kSceneModesId, kStandbyFilterId,
			      kActiveAppId, kAppCaptureId, kAutoCaptureId, kProgramTriggerId}) {
				if (strcmp(id, kind) == 0) {
					if (obs_source_t *held = obs_source_get_ref(candidate))
						static_cast<std::vector<obs_source_t *> *>(param)->push_back(held);
					break;
				}
			}
			return true;
		},
		&owners);
	Rename rename{was, now, scene};
	for (obs_source_t *owner : owners) {
		rename.apply(owner);
		obs_source_release(owner);
	}
}

} // namespace stagekit
