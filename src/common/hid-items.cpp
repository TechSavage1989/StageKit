/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "hid-items.hpp"

#include "holds-hidden.hpp"
#include "item-transition.hpp"

#include <plugin-support.h>

namespace stagekit {

namespace {

struct Found {
	obs_sceneitem_t *item; // held
	std::string scene, in;
};

// Every item of `capture`, in every scene and in the groups in them, held.
std::vector<Found> items_of(obs_source_t *capture)
{
	struct Look {
		obs_source_t *capture;
		std::string scene;
		std::vector<Found> out;
	} look{capture, {}, {}};
	static const auto visit = [](obs_scene_t *s, obs_sceneitem_t *item, void *param) {
		auto *l = static_cast<Look *>(param);
		if (obs_sceneitem_get_source(item) == l->capture) {
			obs_sceneitem_addref(item);
			const char *in = obs_source_get_name(obs_scene_get_source(s));
			l->out.push_back({item, l->scene, in ? in : ""});
		}
		return true;
	};
	obs_enum_scenes(
		[](void *param, obs_source_t *scene_source) {
			if (obs_source_is_group(scene_source))
				return true; // reached through the scene that holds it
			obs_scene_t *scene = obs_scene_from_source(scene_source);
			if (!scene)
				return true;
			auto *l = static_cast<Look *>(param);
			const char *name = obs_source_get_name(scene_source);
			l->scene = name ? name : "";
			obs_scene_enum_items(
				scene,
				[](obs_scene_t *s, obs_sceneitem_t *item, void *p) {
					visit(s, item, p);
					if (obs_sceneitem_is_group(item))
						obs_sceneitem_group_enum_items(item, visit, p);
					return true;
				},
				param);
			return true;
		},
		&look);
	return look.out;
}

} // namespace

std::vector<HidItem> hid_items_read(obs_data_t *settings)
{
	std::vector<HidItem> out;
	obs_data_array_t *rows = settings ? obs_data_get_array(settings, kHidItemsKey) : nullptr;
	const size_t n = rows ? obs_data_array_count(rows) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *row = obs_data_array_item(rows, i);
		HidItem item;
		item.scene = obs_data_get_string(row, "scene");
		item.in = obs_data_get_string(row, "in");
		item.id = obs_data_get_int(row, "id");
		obs_data_release(row);
		if (!item.in.empty())
			out.push_back(item);
	}
	obs_data_array_release(rows);
	return out;
}

void hid_items_write(obs_data_t *settings, const std::vector<HidItem> &items)
{
	if (!settings)
		return;
	if (items.empty()) {
		obs_data_erase(settings, kHidItemsKey);
		return;
	}
	obs_data_array_t *rows = obs_data_array_create();
	for (const HidItem &item : items) {
		obs_data_t *row = obs_data_create();
		obs_data_set_string(row, "scene", item.scene.c_str());
		obs_data_set_string(row, "in", item.in.c_str());
		obs_data_set_int(row, "id", item.id);
		obs_data_array_push_back(rows, row);
		obs_data_release(row);
	}
	obs_data_set_array(settings, kHidItemsKey, rows);
	obs_data_array_release(rows);
}

bool hid_items_pending(obs_source_t *filter)
{
	obs_data_t *settings = filter ? obs_source_get_settings(filter) : nullptr;
	const bool pending = !hid_items_read(settings).empty();
	obs_data_release(settings);
	return pending;
}

bool hid_items_press(obs_source_t *filter, obs_source_t *capture, const ItemChange &change, const char *who)
{
	if (!filter || !capture)
		return false;
	const char *name = obs_source_get_name(capture);
	obs_data_t *settings = obs_source_get_settings(filter);
	const std::vector<HidItem> record = hid_items_read(settings);
	if (!record.empty()) {
		// The show: only what the hide hid, still hidden, and not held
		// hidden by a mode of its scene that is not showing.
		int shown = 0, held = 0;
		for (const HidItem &hid : record) {
			obs_source_t *in = obs_get_source_by_name(hid.in.c_str());
			obs_scene_t *scene = in ? obs_group_or_scene_from_source(in) : nullptr;
			obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, hid.id) : nullptr;
			if (item && obs_sceneitem_get_source(item) == capture && !obs_sceneitem_visible(item)) {
				obs_source_t *top = obs_get_source_by_name(hid.scene.c_str());
				if (mode_holds_hidden(top ? top : in, item)) {
					held++;
				} else {
					apply_item_change(item, change, top ? top : in);
					set_item_visible(item, true);
					shown++;
				}
				obs_source_release(top);
			}
			obs_source_release(in);
		}
		hid_items_write(settings, {});
		obs_data_release(settings);
		obs_log(LOG_INFO, "%s: '%s' shown again in %d of the %d place(s) it hid it (%d held hidden by a mode)",
			who, name, shown, (int)record.size(), held);
		return false;
	}
	// The hide: every item that shows, written down.
	std::vector<HidItem> hid;
	for (Found &found : items_of(capture)) {
		if (obs_sceneitem_visible(found.item)) {
			obs_source_t *top = obs_get_source_by_name(found.scene.c_str());
			apply_item_change(found.item, change, top);
			obs_source_release(top);
			set_item_visible(found.item, false);
			hid.push_back({found.scene, found.in, obs_sceneitem_get_id(found.item)});
		}
		obs_sceneitem_release(found.item);
	}
	hid_items_write(settings, hid);
	obs_data_release(settings);
	if (hid.empty())
		obs_log(LOG_INFO, "%s: '%s' is shown nowhere and nothing it hid is left to show: nothing done", who,
			name);
	else
		obs_log(LOG_INFO, "%s: '%s' hidden in %d place(s)", who, name, (int)hid.size());
	return !hid.empty();
}

} // namespace stagekit
