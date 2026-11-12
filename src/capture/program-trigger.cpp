/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// Program Trigger: "while this program is open, show these items and switch
// these filters on". Add it to a scene to show and hide that scene's items,
// or to any source to switch that source's other filters. When the program
// goes away, everything is put back the other way.
//
// It acts only when the answer changes (the program appears or goes), so a
// change made by hand in between is left alone until then.
//
// Finding windows uses the Windows API (shared with Auto Capture). On other
// systems the filter loads and does nothing.

#include "program-trigger.hpp"

#include "auto-capture.hpp"
#include "../common/scene-util.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

enum class When : int {
	Open = 0,    // the program has a window open
	InFront = 1, // the program's window is the one in front
};

std::string lowered(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return text;
}

std::vector<std::string> list_in(obs_data_t *settings, const char *name)
{
	std::vector<std::string> names;
	obs_data_array_t *list = obs_data_get_array(settings, name);
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		const std::string value = obs_data_get_string(entry, "value");
		if (!value.empty() && std::find(names.begin(), names.end(), value) == names.end())
			names.push_back(value);
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	return names;
}

class ProgramTrigger {
public:
	ProgramTrigger(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		update(settings);
		// For the tests: stand-in windows, one per line, "title|exe|front".
		proc_handler_add(
			obs_source_get_proc_handler(source_), "void set_test_windows(in string windows)",
			[](void *data, calldata_t *cd) {
				auto *self = static_cast<ProgramTrigger *>(data);
				const char *text = calldata_string(cd, "windows");
				std::vector<OpenWindow> windows;
				std::vector<std::string> fields(1);
				auto flush = [&] {
					fields.resize(3);
					if (!fields[1].empty())
						windows.push_back({fields[0], fields[1], fields[2] == "front"});
					fields.assign(1, std::string());
				};
				for (const char *p = text ? text : ""; *p; p++) {
					if (*p == '\n')
						flush();
					else if (*p == '|')
						fields.emplace_back();
					else
						fields.back() += *p;
				}
				flush();
				std::lock_guard lock(self->mutex_);
				self->test_windows_ = windows;
				self->use_test_windows_ = true;
				self->timer_ = 0.0;
			},
			this);
	}

	void update(obs_data_t *settings)
	{
		const std::vector<std::string> items = list_in(settings, "items");
		const std::vector<std::string> filters = list_in(settings, "filters");
		std::lock_guard lock(mutex_);
		when_ = static_cast<When>(obs_data_get_int(settings, "when"));
		program_ = lowered(obs_data_get_string(settings, "program"));
		if (program_.size() > 4 && program_.compare(program_.size() - 4, 4, ".exe") == 0)
			program_.resize(program_.size() - 4);
		title_ = lowered(obs_data_get_string(settings, "title"));
		opposite_ = obs_data_get_bool(settings, "opposite");
		items_ = items;
		filters_ = filters;
		known_ = false; // settings changed: apply the answer afresh
		timer_ = 0.0;
	}

	void tick(float seconds)
	{
		if (!parent_ || !obs_source_enabled(source_))
			return;
		When when;
		std::string program, title;
		std::vector<std::string> items, filters;
		std::vector<OpenWindow> windows;
		bool opposite, use_test, known, was;
		{
			std::lock_guard lock(mutex_);
			timer_ -= seconds;
			if (timer_ > 0.0)
				return;
			timer_ = 1.0;
			when = when_;
			program = program_;
			title = title_;
			opposite = opposite_;
			items = items_;
			filters = filters_;
			use_test = use_test_windows_;
			if (use_test)
				windows = test_windows_;
			known = known_;
			was = matched_;
		}
		if (program.empty() && title.empty())
			return; // nothing to look for yet
		if (!use_test)
			windows = open_windows();

		bool matched = false;
		for (const OpenWindow &w : windows) {
			if (!program.empty() && lowered(w.executable).find(program) == std::string::npos)
				continue;
			if (!title.empty() && lowered(w.title).find(title) == std::string::npos)
				continue;
			if (when == When::InFront && !w.foreground)
				continue;
			matched = true;
			break;
		}
		if (known && matched == was)
			return;
		{
			std::lock_guard lock(mutex_);
			known_ = true;
			matched_ = matched;
		}

		const bool on = matched != opposite;
		for (const std::string &name : items) {
			obs_sceneitem_t *item = find_item(parent_, name);
			if (item && obs_sceneitem_visible(item) != on)
				obs_sceneitem_set_visible(item, on);
		}
		for (const std::string &name : filters) {
			obs_source_t *filter = obs_source_get_filter_by_name(parent_, name.c_str());
			if (filter && filter != source_ && obs_source_enabled(filter) != on)
				obs_source_set_enabled(filter, on);
			obs_source_release(filter);
		}
		obs_log(LOG_INFO, "Program Trigger '%s': %s", obs_source_get_name(source_),
			matched ? "the program is here" : "the program has gone");
	}

	// Adds what is chosen in a "pick" box to one of the lists.
	bool add_picked(const char *pick, const char *list_name)
	{
		obs_data_t *settings = obs_source_get_settings(source_);
		const std::string picked = obs_data_get_string(settings, pick);
		const std::vector<std::string> have = list_in(settings, list_name);
		const bool add = !picked.empty() && std::find(have.begin(), have.end(), picked) == have.end();
		if (add) {
			obs_data_array_t *list = obs_data_get_array(settings, list_name);
			if (!list) {
				list = obs_data_array_create();
				obs_data_set_array(settings, list_name, list);
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

	// What it sees now, for the top line of the controls.
	bool matched(bool &known)
	{
		std::lock_guard lock(mutex_);
		known = known_;
		return matched_;
	}

	void attach(obs_source_t *parent) { parent_ = parent; }
	void detach() { parent_ = nullptr; }
	obs_source_t *parent() const { return parent_; }
	obs_source_t *source() const { return source_; }

private:
	obs_source_t *source_;
	obs_source_t *parent_ = nullptr;
	std::mutex mutex_;
	When when_ = When::Open;
	std::string program_, title_;
	bool opposite_ = false;
	std::vector<std::string> items_, filters_;
	double timer_ = 0.0;
	bool known_ = false, matched_ = false;
	std::vector<OpenWindow> test_windows_;
	bool use_test_windows_ = false;
};

obs_properties_t *get_properties(void *data)
{
	auto *self = static_cast<ProgramTrigger *>(data);
	obs_source_t *parent = self ? self->parent() : nullptr;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *status = obs_properties_add_text(props, "status", "", OBS_TEXT_INFO);
	bool known = false;
	const bool matched = self && self->matched(known);
	obs_property_set_description(status, obs_module_text(!known    ? "Trigger.Status.Unknown"
							     : matched ? "Trigger.Status.Here"
								       : "Trigger.Status.Gone"));

	// 1. The program.
	obs_properties_t *watch = obs_properties_create();
	obs_property_t *program = obs_properties_add_list(watch, "program", obs_module_text("Trigger.Program"),
							  OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	for (const OpenWindow &w : open_windows()) {
		bool listed = false;
		for (size_t i = 0; i < obs_property_list_item_count(program); i++)
			listed = listed || w.executable == obs_property_list_item_string(program, i);
		if (!listed)
			obs_property_list_add_string(program, w.executable.c_str(), w.executable.c_str());
	}
	obs_property_t *browse = obs_properties_add_path(watch, "browse", obs_module_text("Trigger.Browse"),
							 OBS_PATH_FILE, "Programs (*.exe);;All files (*.*)", nullptr);
	obs_property_set_modified_callback(browse, [](obs_properties_t *, obs_property_t *, obs_data_t *settings) {
		// Choosing a file fills in the program box above.
		std::string path = obs_data_get_string(settings, "browse");
		if (path.empty())
			return false;
		const size_t slash = path.find_last_of("/\\");
		if (slash != std::string::npos)
			path = path.substr(slash + 1);
		obs_data_set_string(settings, "program", path.c_str());
		obs_data_set_string(settings, "browse", "");
		return true;
	});
	obs_property_t *title =
		obs_properties_add_text(watch, "title", obs_module_text("Trigger.Title"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(title, obs_module_text("Trigger.Title.Tip"));
	obs_property_t *when = obs_properties_add_list(watch, "when", obs_module_text("Trigger.When"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(when, obs_module_text("Trigger.When.Open"), (int)When::Open);
	obs_property_list_add_int(when, obs_module_text("Trigger.When.InFront"), (int)When::InFront);
	obs_properties_add_group(props, "watch_group", obs_module_text("Trigger.WatchGroup"), OBS_GROUP_NORMAL, watch);

	// 2. What it switches.
	obs_properties_t *act = obs_properties_create();
	if (scene_of(parent)) {
		obs_property_t *pick = obs_properties_add_list(act, "pick_item", obs_module_text("Trigger.PickItem"),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(pick, obs_module_text("Link.Pick.None"), "");
		add_scene_items_to_list(parent, pick);
		obs_properties_add_button2(
			act, "add_item", obs_module_text("Trigger.AddItem"),
			[](obs_properties_t *, obs_property_t *, void *d) {
				return d && static_cast<ProgramTrigger *>(d)->add_picked("pick_item", "items");
			},
			data);
		obs_properties_add_editable_list(act, "items", obs_module_text("Trigger.Items"),
						 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
	}
	obs_property_t *pick_filter = obs_properties_add_list(act, "pick_filter", obs_module_text("Trigger.PickFilter"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(pick_filter, obs_module_text("Trigger.PickFilter.None"), "");
	if (parent) {
		struct Fill {
			obs_property_t *list;
			obs_source_t *self;
		} fill{pick_filter, self->source()};
		obs_source_enum_filters(
			parent,
			[](obs_source_t *, obs_source_t *filter, void *param) {
				auto *f = static_cast<Fill *>(param);
				const char *name = obs_source_get_name(filter);
				if (filter != f->self && name && *name)
					obs_property_list_add_string(f->list, name, name);
			},
			&fill);
	}
	obs_properties_add_button2(
		act, "add_filter", obs_module_text("Trigger.AddFilter"),
		[](obs_properties_t *, obs_property_t *, void *d) {
			return d && static_cast<ProgramTrigger *>(d)->add_picked("pick_filter", "filters");
		},
		data);
	obs_properties_add_editable_list(act, "filters", obs_module_text("Trigger.Filters"),
					 OBS_EDITABLE_LIST_TYPE_STRINGS, nullptr, nullptr);
	obs_property_t *opposite = obs_properties_add_bool(act, "opposite", obs_module_text("Trigger.Opposite"));
	obs_property_set_long_description(opposite, obs_module_text("Trigger.Opposite.Tip"));
	obs_properties_add_group(props, "act_group", obs_module_text("Trigger.ActGroup"), OBS_GROUP_NORMAL, act);

	obs_property_t *help = obs_properties_add_text(props, "help", obs_module_text("Trigger.Help"), OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(help, true);
	return props;
}

} // namespace

void register_program_trigger()
{
	obs_source_info info = {};
	info.id = kProgramTriggerId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("Trigger.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new ProgramTrigger(settings, source);
	};
	info.destroy = [](void *data) {
		delete static_cast<ProgramTrigger *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<ProgramTrigger *>(data)->update(settings);
	};
	info.get_defaults = [](obs_data_t *settings) {
		obs_data_set_default_int(settings, "when", (int)When::Open);
		obs_data_set_default_bool(settings, "opposite", false);
	};
	info.get_properties = get_properties;
	info.video_tick = [](void *data, float seconds) {
		static_cast<ProgramTrigger *>(data)->tick(seconds);
	};
	// A controller: it never changes the picture.
	info.video_render = [](void *data, gs_effect_t *) {
		obs_source_skip_video_filter(static_cast<ProgramTrigger *>(data)->source());
	};
	info.filter_add = [](void *data, obs_source_t *parent) {
		static_cast<ProgramTrigger *>(data)->attach(parent);
	};
	info.filter_remove = [](void *data, obs_source_t *) {
		static_cast<ProgramTrigger *>(data)->detach();
	};
	obs_register_source(&info);
}

} // namespace stagekit
