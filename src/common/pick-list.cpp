/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "pick-list.hpp"

#include <obs-module.h>

#include <algorithm>
#include <cstdlib>

namespace stagekit {

namespace {

// Property names: "<setting>|list" is the group, "<setting>|pick" the box,
// "<setting>|row|<n>" a row and "<setting>|none" the line shown when the list
// is empty. A 't' after the first bar marks a list kept as text.
std::string name_of(const char *setting, bool as_text, const char *part)
{
	return std::string(setting) + (as_text ? "|t" : "|a") + part;
}

struct Parsed {
	std::string setting;
	bool as_text = false;
	int row = -1;
};

Parsed parse(const char *property_name)
{
	Parsed out;
	const std::string name = property_name ? property_name : "";
	const size_t bar = name.find('|');
	if (bar == std::string::npos || bar + 1 >= name.size())
		return out;
	out.setting = name.substr(0, bar);
	out.as_text = name[bar + 1] == 't';
	const size_t row = name.find("row|", bar);
	if (row != std::string::npos)
		out.row = atoi(name.c_str() + row + 4);
	return out;
}

std::string trimmed(const std::string &text)
{
	const size_t first = text.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		return {};
	const size_t last = text.find_last_not_of(" \t\r\n");
	return text.substr(first, last - first + 1);
}

bool remove_row(obs_properties_t *top, obs_property_t *property, void *data);

// Puts one row per entry under the heading, replacing whatever rows were there.
void build_rows(obs_properties_t *top, obs_source_t *owner, obs_data_t *settings, const char *setting, bool as_text)
{
	obs_property_t *group = obs_properties_get(top, name_of(setting, as_text, "list").c_str());
	obs_properties_t *content = group ? obs_property_group_content(group) : nullptr;
	if (!content)
		return;
	const std::string row_prefix = name_of(setting, as_text, "row|");
	const std::string none_name = name_of(setting, as_text, "none");
	std::vector<std::string> old;
	for (obs_property_t *p = obs_properties_first(content); p; obs_property_next(&p)) {
		const std::string name = obs_property_name(p);
		if (name.rfind(row_prefix, 0) == 0 || name == none_name)
			old.push_back(name);
	}
	for (const std::string &name : old)
		obs_properties_remove_by_name(content, name.c_str());

	const std::vector<std::string> entries = pick_list_read(settings, setting, as_text);
	if (entries.empty()) {
		obs_properties_add_text(content, none_name.c_str(), obs_module_text("List.Empty"), OBS_TEXT_INFO);
		return;
	}
	for (size_t i = 0; i < entries.size(); i++) {
		const std::string name = row_prefix + std::to_string(i);
		// The cross and the entry's name are the button: pressing it takes
		// that entry off the list.
		const std::string text = std::string("\xE2\x9C\x95   ") + entries[i];
		obs_property_t *row =
			obs_properties_add_button2(content, name.c_str(), text.c_str(), remove_row, owner);
		obs_property_set_long_description(row, obs_module_text("List.Remove.Tip"));
	}
}

bool remove_row(obs_properties_t *top, obs_property_t *property, void *data)
{
	auto *owner = static_cast<obs_source_t *>(data);
	const Parsed which = parse(obs_property_name(property));
	if (!owner || which.setting.empty() || which.row < 0)
		return false;
	obs_data_t *settings = obs_source_get_settings(owner);
	std::vector<std::string> entries = pick_list_read(settings, which.setting.c_str(), which.as_text);
	if ((size_t)which.row < entries.size()) {
		entries.erase(entries.begin() + which.row);
		pick_list_write(settings, which.setting.c_str(), which.as_text, entries);
		obs_source_update(owner, settings);
	}
	build_rows(top, owner, settings, which.setting.c_str(), which.as_text);
	obs_data_release(settings);
	return true;
}

// Choosing something in the box adds it and puts the box back to its first line.
bool picked(void *priv, obs_properties_t *top, obs_property_t *property, obs_data_t *settings)
{
	auto *owner = static_cast<obs_source_t *>(priv);
	const std::string pick_name = obs_property_name(property);
	const Parsed which = parse(pick_name.c_str());
	const std::string chosen = trimmed(obs_data_get_string(settings, pick_name.c_str()));
	if (which.setting.empty() || chosen.empty())
		return false;
	std::vector<std::string> entries = pick_list_read(settings, which.setting.c_str(), which.as_text);
	if (std::find(entries.begin(), entries.end(), chosen) == entries.end()) {
		entries.push_back(chosen);
		pick_list_write(settings, which.setting.c_str(), which.as_text, entries);
	}
	obs_data_set_string(settings, pick_name.c_str(), "");
	build_rows(top, owner, settings, which.setting.c_str(), which.as_text);
	return true;
}

} // namespace

std::vector<std::string> pick_list_read(obs_data_t *settings, const char *setting, bool as_text)
{
	std::vector<std::string> out;
	auto add = [&](const std::string &raw) {
		const std::string entry = trimmed(raw);
		if (!entry.empty() && std::find(out.begin(), out.end(), entry) == out.end())
			out.push_back(entry);
	};
	if (as_text) {
		const std::string text = obs_data_get_string(settings, setting);
		size_t start = 0;
		while (start <= text.size()) {
			size_t comma = text.find(',', start);
			if (comma == std::string::npos)
				comma = text.size();
			add(text.substr(start, comma - start));
			start = comma + 1;
		}
		return out;
	}
	obs_data_array_t *list = obs_data_get_array(settings, setting);
	for (size_t i = 0; i < obs_data_array_count(list); i++) {
		obs_data_t *entry = obs_data_array_item(list, i);
		add(obs_data_get_string(entry, "value"));
		obs_data_release(entry);
	}
	obs_data_array_release(list);
	return out;
}

void pick_list_write(obs_data_t *settings, const char *setting, bool as_text, const std::vector<std::string> &entries)
{
	if (as_text) {
		std::string text;
		for (const std::string &entry : entries)
			text += (text.empty() ? "" : ", ") + entry;
		obs_data_set_string(settings, setting, text.c_str());
		return;
	}
	obs_data_array_t *list = obs_data_array_create();
	for (const std::string &value : entries) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "value", value.c_str());
		obs_data_set_bool(entry, "selected", false);
		obs_data_set_bool(entry, "hidden", false);
		obs_data_array_push_back(list, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(settings, setting, list);
	obs_data_array_release(list);
}

void pick_list_add(obs_properties_t *props, obs_source_t *owner, const PickList &list)
{
	obs_properties_t *content = obs_properties_create();
	obs_data_t *settings = owner ? obs_source_get_settings(owner) : nullptr;
	if (list.add) {
		const std::string pick_name = name_of(list.setting, list.as_text, "pick");
		obs_property_t *pick = obs_properties_add_list(content, pick_name.c_str(), obs_module_text(list.add),
							       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(pick, obs_module_text("List.Choose"), "");
		std::vector<std::string> choices;
		if (list.choices)
			list.choices(choices);
		std::sort(choices.begin(), choices.end());
		choices.erase(std::unique(choices.begin(), choices.end()), choices.end());
		const std::vector<std::string> have = settings ? pick_list_read(settings, list.setting, list.as_text)
							       : std::vector<std::string>();
		for (const std::string &choice : choices) {
			if (!choice.empty() && std::find(have.begin(), have.end(), choice) == have.end())
				obs_property_list_add_string(pick, choice.c_str(), choice.c_str());
		}
		if (list.tip)
			obs_property_set_long_description(pick, obs_module_text(list.tip));
		obs_property_set_modified_callback2(pick, picked, owner);
		if (settings)
			obs_data_set_string(settings, pick_name.c_str(), "");
	}
	obs_properties_add_group(props, name_of(list.setting, list.as_text, "list").c_str(),
				 obs_module_text(list.title), OBS_GROUP_NORMAL, content);
	if (settings) {
		build_rows(props, owner, settings, list.setting, list.as_text);
		obs_data_release(settings);
	}
}

void pick_list_refresh(obs_properties_t *top, obs_source_t *owner, const char *setting, bool as_text)
{
	if (!owner)
		return;
	obs_data_t *settings = obs_source_get_settings(owner);
	build_rows(top, owner, settings, setting, as_text);
	obs_data_release(settings);
}

} // namespace stagekit
