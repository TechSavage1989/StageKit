/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "managed-filter.hpp"

#include "scene-util.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace stagekit {

namespace {

// One scene filter, as it last said. Copied out under the lock; no lock is
// held while OBS is asked anything.
struct Entry {
	std::string kind;
	obs_source_t *self = nullptr; // a key only, never used
	std::string scene_name;
	bool enabled = false;
	std::set<std::string> kept, covered;
	std::string look_json;
};

std::mutex g_mutex;
std::map<obs_source_t *, Entry> g_entries;

std::vector<Entry> entries_of(const char *kind)
{
	std::vector<Entry> found;
	std::lock_guard lock(g_mutex);
	for (const auto &[self, entry] : g_entries)
		if (entry.kind == kind)
			found.push_back(entry);
	return found;
}

// Which scene filter a source's filter takes its look from: the one in the
// scene it names, or else the first that keeps it (switched on before off,
// then by scene name). nullptr for none.
const Entry *owner_of(const std::vector<Entry> &entries, const std::string &source, const std::string &managed_by)
{
	const Entry *first = nullptr;
	for (const Entry &entry : entries) {
		if (!entry.kept.count(source))
			continue;
		if (entry.scene_name == managed_by)
			return &entry;
		if (!first || (entry.enabled && !first->enabled) ||
		    (entry.enabled == first->enabled && entry.scene_name < first->scene_name))
			first = &entry;
	}
	return first;
}

bool same_value(obs_data_t *a, obs_data_t *b, const LookKey &key)
{
	switch (key.type) {
	case LookType::Int:
		return obs_data_get_int(a, key.name) == obs_data_get_int(b, key.name);
	case LookType::Double:
		return std::fabs(obs_data_get_double(a, key.name) - obs_data_get_double(b, key.name)) < 1e-9;
	case LookType::Bool:
		return obs_data_get_bool(a, key.name) == obs_data_get_bool(b, key.name);
	case LookType::String:
		return std::strcmp(obs_data_get_string(a, key.name), obs_data_get_string(b, key.name)) == 0;
	}
	return true;
}

void copy_value(obs_data_t *into, obs_data_t *from, const LookKey &key)
{
	switch (key.type) {
	case LookType::Int:
		obs_data_set_int(into, key.name, obs_data_get_int(from, key.name));
		break;
	case LookType::Double:
		obs_data_set_double(into, key.name, obs_data_get_double(from, key.name));
		break;
	case LookType::Bool:
		obs_data_set_bool(into, key.name, obs_data_get_bool(from, key.name));
		break;
	case LookType::String:
		obs_data_set_string(into, key.name, obs_data_get_string(from, key.name));
		break;
	}
}

// The source's filters of a kind, each with a reference: the user's own and
// the ones a scene filter made.
struct Filters {
	std::vector<obs_source_t *> managed, own;
	~Filters()
	{
		for (obs_source_t *f : managed)
			obs_source_release(f);
		for (obs_source_t *f : own)
			obs_source_release(f);
	}
};

void filters_of(obs_source_t *source, const char *kind, Filters &out)
{
	struct Find {
		const char *kind;
		Filters *out;
	} find{kind, &out};
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto *f = static_cast<Find *>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (!id || std::strcmp(id, f->kind) != 0)
				return;
			obs_source_t *held = obs_source_get_ref(filter);
			if (!held)
				return;
			obs_data_t *settings = obs_source_get_settings(filter);
			const bool managed = obs_data_get_bool(settings, "managed");
			obs_data_release(settings);
			(managed ? f->out->managed : f->out->own).push_back(held);
		},
		&find);
}

} // namespace

std::string text_with(const char *key, const std::string &value)
{
	std::string text = obs_module_text(key);
	const size_t at = text.find("%1");
	if (at != std::string::npos)
		text.replace(at, 2, value);
	return text;
}

void ManagedSet::publish(obs_source_t *scene, bool enabled, const std::set<std::string> &kept,
			 const std::set<std::string> &covered, obs_data_t *look)
{
	kept_ = kept;
	Entry entry;
	entry.kind = spec_.scene_kind;
	entry.self = self_;
	const char *name = scene ? obs_source_get_name(scene) : nullptr;
	entry.scene_name = name ? name : "";
	entry.enabled = enabled;
	entry.kept = kept;
	entry.covered = covered;
	const char *json = look ? obs_data_get_json(look) : nullptr;
	entry.look_json = json ? json : "{}";
	std::lock_guard lock(g_mutex);
	g_entries[self_] = std::move(entry);
}

void ManagedSet::withdraw()
{
	std::lock_guard lock(g_mutex);
	g_entries.erase(self_);
}

void ManagedSet::sync(obs_source_t *scene, double now)
{
	const std::vector<Entry> entries = entries_of(spec_.scene_kind);
	const Entry *mine = nullptr;
	for (const Entry &entry : entries)
		if (entry.self == self_)
			mine = &entry;
	if (!mine || !scene)
		return;
	const std::string scene_name = mine->scene_name;
	const bool enabled = mine->enabled;
	const bool switched = last_enabled_ != (enabled ? 1 : 0);
	last_enabled_ = enabled ? 1 : 0;
	obs_data_t *look = obs_data_create_from_json(mine->look_json.c_str());
	if (!look)
		look = obs_data_create();
	own_ones_.clear();

	for (const std::string &name : kept_) {
		obs_source_t *source = obs_get_source_by_name(name.c_str());
		if (!source || source == scene || obs_source_get_type(source) == OBS_SOURCE_TYPE_FILTER) {
			obs_source_release(source);
			continue;
		}
		Filters filters;
		filters_of(source, spec_.filter_kind, filters);
		if (!filters.own.empty()) {
			// The user's own: left as it is, and no second one.
			own_ones_.insert(name);
		} else if (filters.managed.empty()) {
			obs_data_t *settings = obs_data_create();
			for (size_t i = 0; i < spec_.look_count; i++)
				copy_value(settings, look, spec_.look[i]);
			obs_data_set_bool(settings, "managed", true);
			obs_data_set_string(settings, "managed_by", scene_name.c_str());
			obs_source_t *made = obs_source_create(spec_.filter_kind, obs_module_text(spec_.name_key),
							       settings, nullptr);
			obs_data_release(settings);
			if (made) {
				if (spec_.mirror_enabled && !enabled)
					obs_source_set_enabled(made, false);
				obs_source_filter_add(source, made);
				obs_log(LOG_INFO, "%s '%s': added '%s' to '%s'", spec_.log_name, scene_name.c_str(),
					obs_source_get_name(made), name.c_str());
				obs_source_release(made);
			}
		} else {
			obs_source_t *filter = filters.managed.front();
			obs_data_t *settings = obs_source_get_settings(filter);
			const std::string managed_by = obs_data_get_string(settings, "managed_by");
			const Entry *owner = owner_of(entries, name, managed_by);
			if (owner && owner->self == self_) {
				obs_data_t *diff = obs_data_create();
				bool any = false;
				for (size_t i = 0; i < spec_.look_count; i++) {
					if (same_value(settings, look, spec_.look[i]))
						continue;
					copy_value(diff, look, spec_.look[i]);
					any = true;
				}
				if (managed_by != scene_name) {
					obs_data_set_string(diff, "managed_by", scene_name.c_str());
					obs_log(LOG_INFO, "%s '%s': takes over '%s' on '%s' (was set in '%s')",
						spec_.log_name, scene_name.c_str(), obs_source_get_name(filter),
						name.c_str(), managed_by.c_str());
					any = true;
				}
				if (any)
					obs_source_update(filter, diff);
				obs_data_release(diff);
				if (spec_.mirror_enabled && switched && obs_source_enabled(filter) != enabled)
					obs_source_set_enabled(filter, enabled);
			}
			obs_data_release(settings);
		}
		obs_source_release(source);
	}
	obs_data_release(look);

	// Taken off: a filter made by a scene filter whose source no scene
	// filter of the kind keeps any more, after a grace time (a source being
	// moved between scenes, a list being edited).
	std::vector<std::string> names;
	scene_item_names(scene, names);
	std::set<std::string> look_at(names.begin(), names.end());
	look_at.insert(seen_.begin(), seen_.end());
	seen_ = kept_;
	for (const std::string &name : look_at) {
		bool listed = false;
		for (const Entry &entry : entries)
			listed = listed || entry.kept.count(name);
		if (listed) {
			uncovered_since_.erase(name);
			continue;
		}
		obs_source_t *source = obs_get_source_by_name(name.c_str());
		Filters filters;
		if (source)
			filters_of(source, spec_.filter_kind, filters);
		if (filters.managed.empty()) {
			uncovered_since_.erase(name);
			obs_source_release(source);
			continue;
		}
		seen_.insert(name); // looked at again until it is gone
		const double since = uncovered_since_.emplace(name, now).first->second;
		if (now - since >= kManagedGraceS) {
			for (obs_source_t *filter : filters.managed) {
				obs_log(LOG_INFO, "%s '%s': took '%s' off '%s', which no %s covers any more",
					spec_.log_name, scene_name.c_str(), obs_source_get_name(filter), name.c_str(),
					spec_.log_name);
				obs_source_filter_remove(source, filter);
			}
			uncovered_since_.erase(name);
		}
		obs_source_release(source);
	}
}

ManagedState managed_state(const char *scene_kind, obs_source_t *source, const char *managed_by)
{
	ManagedState state;
	const char *name = source ? obs_source_get_name(source) : nullptr;
	if (!name)
		return state;
	const std::vector<Entry> entries = entries_of(scene_kind);
	for (const Entry &entry : entries) {
		if (!entry.kept.count(name))
			continue;
		state.listed = true;
		state.covered = state.covered || (entry.enabled && entry.covered.count(name));
	}
	if (const Entry *owner = owner_of(entries, name, managed_by ? managed_by : "")) {
		state.owner_scene = owner->scene_name;
		state.owner_found = true;
	}
	return state;
}

std::set<std::string> managed_scenes(const char *scene_kind, const std::string &source)
{
	std::set<std::string> scenes;
	for (const Entry &entry : entries_of(scene_kind))
		if (entry.kept.count(source))
			scenes.insert(entry.scene_name);
	return scenes;
}

} // namespace stagekit
