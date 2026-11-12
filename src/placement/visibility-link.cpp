/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Visibility Link: a filter you add to a scene. It names a set of the
// scene's items and ties their eye icons together, one of two ways:
//
//   Show only one   showing one of them hides the others (a camera frame
//                   per console, one of several overlays)
//   Together        showing or hiding one does the same to the rest (a
//                   camera with its frame and its label)
//
// Whatever shows or hides an item counts: the eye icon, a Stream Deck OBS
// plugin "Source" key, a hotkey, obs-websocket. It replaces the macros that
// watched one item and switched the others.

#include "visibility-link.hpp"

#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

enum class LinkMode : int {
	OnlyOne = 0,
	Together = 1,
};

std::vector<std::string> names_in(obs_data_t *settings)
{
	std::vector<std::string> names;
	obs_data_array_t *list = obs_data_get_array(settings, "items");
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const std::string name = obs_data_get_string(entry, "value");
		if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	return names;
}

class VisibilityLink {
public:
	VisibilityLink(obs_data_t *settings, obs_source_t *source) : source_(source) { update(settings); }
	~VisibilityLink() { detach(); }

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> names = names_in(settings);
		std::lock_guard lock(mutex_);
		mode_ = static_cast<LinkMode>(obs_data_get_int(settings, "mode"));
		names_ = names;
	}

	void attach(obs_source_t *parent)
	{
		detach();
		parent_ = parent;
		signal_handler_connect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb, this);
	}

	void detach()
	{
		if (parent_)
			signal_handler_disconnect(obs_source_get_signal_handler(parent_), "item_visible", visible_cb,
						  this);
		parent_ = nullptr;
	}

	// The others are switched on the next frame, not inside the signal: OBS
	// has not finished showing or hiding the item while the signal runs.
	void tick()
	{
		std::vector<std::pair<std::string, bool>> changes;
		LinkMode mode;
		std::vector<std::string> names;
		{
			std::lock_guard lock(mutex_);
			changes.swap(pending_);
			mode = mode_;
			names = names_;
		}
		if (changes.empty() || !parent_ || !obs_source_enabled(source_))
			return;
		applying_ = true;
		for (const auto &[changed, visible] : changes) {
			if (mode == LinkMode::OnlyOne && !visible)
				continue; // hiding one says nothing about the others
			for (const std::string &name : names) {
				if (name == changed)
					continue;
				obs_sceneitem_t *item = find_item(parent_, name);
				const bool want = mode == LinkMode::Together ? visible : false;
				if (item && obs_sceneitem_visible(item) != want)
					obs_sceneitem_set_visible(item, want);
			}
		}
		applying_ = false;
	}

	// Adds the item chosen in the "pick" box to the list.
	bool add_picked()
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::string picked = obs_data_get_string(settings, "pick");
		std::vector<std::string> names = names_in(settings);
		const bool add = !picked.empty() && std::find(names.begin(), names.end(), picked) == names.end();
		if (add) {
			obs_data_array_t *list = obs_data_get_array(settings, "items");
			if (!list) {
				list = obs_data_array_create();
				obs_data_set_array(settings, "items", list);
			}
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "value", picked.c_str());
			obs_data_array_push_back(list, entry);
			obs_data_release(entry);
			obs_data_array_release(list);
			obs_source_update(source_, settings);
		}
		obs_data_release(settings);
		return add;
	}

	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }

private:
	static void visible_cb(void *data, calldata_t *cd)
	{
		auto *self = static_cast<VisibilityLink *>(data);
		if (self->applying_)
			return; // our own doing
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		const char *name = item ? obs_source_get_name(obs_sceneitem_get_source(item)) : nullptr;
		if (!name)
			return;
		std::lock_guard lock(self->mutex_);
		if (std::find(self->names_.begin(), self->names_.end(), name) != self->names_.end())
			self->pending_.emplace_back(name, calldata_bool(cd, "visible"));
	}

	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	LinkMode mode_ = LinkMode::OnlyOne;
	std::vector<std::string> names_;
	std::vector<std::pair<std::string, bool>> pending_;
	std::atomic<bool> applying_{false};
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<VisibilityLink *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, "mode", obs_module_text("Link.Mode"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.OnlyOne"), (int)LinkMode::OnlyOne);
	obs_property_list_add_int(mode, obs_module_text("Link.Mode.Together"), (int)LinkMode::Together);

	PickList items;
	items.setting = "items";
	items.title = "Link.Items";
	items.add = "List.AddItem";
	items.choices = [self](std::vector<std::string> &names) {
		scene_item_names(self ? self->parent() : nullptr, names);
	};
	pick_list_add(props, self ? self->source() : nullptr, items);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Link.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_visibility_link()
{
	obs_source_info info = {};
	info.id = kVisibilityLinkId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Link.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new VisibilityLink(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<VisibilityLink *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<VisibilityLink *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "mode", (int)LinkMode::OnlyOne);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float) {
		static_cast<VisibilityLink *>(data)->tick();
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<VisibilityLink *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<VisibilityLink *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<VisibilityLink *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
