/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "control-catalog.hpp"
#include "scene-controller.hpp"

#include "../common/go-to-scene.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../placement/scene-modes.hpp"
#include "../placement/show-in-scenes.hpp"
#include "../placement/visibility-link.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <QCoreApplication>
#include <QThread>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {
namespace {

using Data = std::shared_ptr<obs_data_t>;
using Array = std::shared_ptr<obs_data_array_t>;
using Source = std::shared_ptr<obs_source_t>;
Data data(obs_data_t *value = nullptr)
{
	return Data(value ? value : obs_data_create(), obs_data_release);
}
Array array(obs_data_array_t *value = nullptr)
{
	return Array(value ? value : obs_data_array_create(), obs_data_array_release);
}
Source source(obs_source_t *value)
{
	return Source(value, obs_source_release);
}
Data object(obs_data_t *value, const char *key)
{
	return data(obs_data_get_obj(value, key));
}
Array entries(obs_data_t *value, const char *key)
{
	return array(obs_data_get_array(value, key));
}
std::string text(obs_data_t *value, const char *key)
{
	return obs_data_get_string(value, key);
}
std::string label(const char *key)
{
	return obs_module_text(key);
}
bool is_kind(obs_source_t *value, const char *id)
{
	const char *actual = value ? obs_source_get_unversioned_id(value) : nullptr;
	return actual && std::strcmp(actual, id) == 0;
}
bool frontend_thread()
{
	auto *app = QCoreApplication::instance();
	return !app || QThread::currentThread() == app->thread();
}
std::string stable_id(const char *kind, std::initializer_list<std::string> parts)
{
	std::string result = kind;
	for (const auto &part : parts)
		result += ":" + std::to_string(part.size()) + ":" + part;
	return result;
}
std::string section(std::initializer_list<std::string> parts)
{
	std::string result;
	for (const auto &part : parts)
		if (!part.empty()) {
			if (!result.empty())
				result += " / ";
			result += part;
		}
	return result;
}
Data target(const char *kind)
{
	auto value = data();
	obs_data_set_string(value.get(), "kind", kind);
	return value;
}
void set_text(obs_data_t *value, const char *key, const std::string &content)
{
	obs_data_set_string(value, key, content.c_str());
}
void append(obs_data_array_t *out, const std::string &id, const std::string &name, const std::string &where,
	    bool active, bool available, const std::string &reason, bool visible, obs_data_t *action)
{
	auto value = data();
	set_text(value.get(), "id", id);
	set_text(value.get(), "name", name);
	set_text(value.get(), "section", where);
	obs_data_set_bool(value.get(), "active", active);
	obs_data_set_bool(value.get(), "available", available);
	set_text(value.get(), "reason", available ? "" : reason.empty() ? label("Controls.NotReady") : reason);
	obs_data_set_bool(value.get(), "defaultVisible", visible);
	set_text(action, "id", id);
	obs_data_set_obj(value.get(), "target", action);
	obs_data_array_push_back(out, value.get());
}

struct Item {
	std::string owner_id, owner_name, source_id, name;
	int64_t id = 0;
	bool visible = false;
};

// All names, IDs and eye states are read inside retained scene enumeration.
// No borrowed item escapes this snapshot. A later press resolves its identity
// again and pins the item inside another enumeration before changing it.
bool collect_items(obs_scene_t *scene, obs_sceneitem_t *item, void *opaque)
{
	auto &out = *static_cast<std::vector<Item> *>(opaque);
	auto *owner = obs_scene_get_source(scene);
	auto *value = obs_sceneitem_get_source(item);
	if (owner && value && !obs_obj_is_private(value) && !obs_source_removed(value))
		out.push_back({obs_source_get_uuid(owner), obs_source_get_name(owner), obs_source_get_uuid(value),
			       obs_source_get_name(value), obs_sceneitem_get_id(item), obs_sceneitem_visible(item)});
	if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect_items, opaque);
	return true;
}

struct Inventory {
	struct Filter {
		Source parent, value;
	};
	std::map<std::string, Source> sources;
	std::map<std::string, std::vector<Item>> items;
	std::vector<Filter> filters;

	Inventory()
	{
		const auto add_source = [](void *opaque, obs_source_t *value) {
			auto &out = *static_cast<std::map<std::string, Source> *>(opaque);
			if (value && !obs_obj_is_private(value) && !obs_source_removed(value))
				out.emplace(obs_source_get_uuid(value), source(obs_source_get_ref(value)));
			return true;
		};
		obs_enum_sources(add_source, &sources);
		obs_enum_scenes(add_source, &sources);
		for (const auto &[uuid, value] : sources) {
			if (auto *scene = scene_of(value.get()))
				obs_scene_enum_items(scene, collect_items, &items[uuid]);
			// Enumerate into retained handles first; never take rule snapshots
			// or invoke a filter while OBS holds its filter-list lock.
			std::vector<Source> found;
			obs_source_enum_filters(
				value.get(),
				[](obs_source_t *, obs_source_t *filter, void *opaque) {
					static_cast<std::vector<Source> *>(opaque)->push_back(
						source(obs_source_get_ref(filter)));
				},
				&found);
			for (auto &filter : found)
				if (!obs_source_removed(filter.get()) &&
				    obs_filter_get_parent(filter.get()) == value.get())
					filters.push_back({value, std::move(filter)});
		}
	}

	const Item *find(const std::string &scene, const std::string &name) const
	{
		const auto found = items.find(scene);
		if (found != items.end())
			for (const auto &item : found->second)
				if (item.name == name)
					return &item;
		return nullptr;
	}
};

struct Alias {
	Data controller, group, definition;
};
using Aliases = std::map<std::string, Alias>;

void scene_controls(obs_data_array_t *out)
{
	if (!obs_frontend_get_main_window())
		return;
	const bool studio = obs_frontend_preview_program_mode_active();
	auto current = source(studio ? obs_frontend_get_current_preview_scene() : obs_frontend_get_current_scene());
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	// Use the Scenes dock's order. The frontend list retains every source
	// until the entries, IDs and active state have been copied out.
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		obs_source_t *scene = scenes.sources.array[i];
		if (!scene || obs_obj_is_private(scene) || obs_source_removed(scene) || !obs_scene_from_source(scene) ||
		    obs_source_is_group(scene))
			continue;
		const std::string uuid = obs_source_get_uuid(scene);
		auto action = target("scene");
		set_text(action.get(), "sceneUuid", uuid);
		append(out, stable_id("scene", {uuid}), obs_source_get_name(scene), label("Controls.Scenes"),
		       scene == current.get(), true, {}, false, action.get());
	}
	obs_frontend_source_list_free(&scenes);
}

Aliases controller_controls(obs_data_array_t *out)
{
	Aliases aliases;
	auto response = data();
	scene_controller_list(response.get());
	auto controllers = entries(response.get(), "controllers");
	for (size_t i = 0; i < obs_data_array_count(controllers.get()); ++i) {
		auto controller = data(obs_data_array_item(controllers.get(), i));
		const std::string cid = text(controller.get(), "controllerId");
		const std::string scene = text(controller.get(), "scene"), name = text(controller.get(), "name");
		const bool enabled = obs_data_get_bool(controller.get(), "enabled");
		const bool ready = enabled && obs_data_get_bool(controller.get(), "available");
		const std::string controller_reason = enabled ? text(controller.get(), "reason")
							      : label("Controls.Disabled");
		auto config = object(controller.get(), "config");
		auto definitions = entries(config.get(), "groups");
		auto groups = entries(controller.get(), "groups");
		for (size_t j = 0; j < obs_data_array_count(groups.get()); ++j) {
			auto group = data(obs_data_array_item(groups.get(), j));
			const std::string gid = text(group.get(), "id");
			for (size_t k = 0; k < obs_data_array_count(definitions.get()); ++k) {
				auto definition = data(obs_data_array_item(definitions.get(), k));
				if (text(definition.get(), "id") != gid)
					continue;
				auto origin = object(definition.get(), "origin");
				if (obs_data_get_bool(origin.get(), "activate") &&
				    !text(origin.get(), "filterUuid").empty())
					aliases.emplace(text(origin.get(), "filterUuid"),
							Alias{controller, group, definition});
				break;
			}
			const bool group_enabled = obs_data_get_bool(group.get(), "enabled");
			const bool group_ready = ready && group_enabled && obs_data_get_bool(group.get(), "available");
			const std::string reason = !ready           ? controller_reason
						   : !group_enabled ? label("Controls.Disabled")
								    : text(group.get(), "reason");
			const std::string where = section({scene, name, text(group.get(), "name")});
			auto choices = entries(group.get(), "choices");
			for (size_t k = 0; k < obs_data_array_count(choices.get()); ++k) {
				auto choice = data(obs_data_array_item(choices.get(), k));
				const std::string choice_id = text(choice.get(), "id");
				auto action = target("controller");
				set_text(action.get(), "controllerId", cid);
				set_text(action.get(), "groupId", gid);
				set_text(action.get(), "choiceId", choice_id);
				set_text(action.get(), "command", "select");
				const bool available = group_ready && obs_data_get_bool(choice.get(), "available");
				append(out, stable_id("controller-choice", {cid, gid, choice_id}),
				       text(choice.get(), "name"), where, obs_data_get_bool(choice.get(), "active"),
				       available, group_ready ? text(choice.get(), "reason") : reason, true,
				       action.get());
			}
			if (text(group.get(), "kind") == "modes" && obs_data_get_bool(group.get(), "allowNone")) {
				auto action = target("controller");
				set_text(action.get(), "controllerId", cid);
				set_text(action.get(), "groupId", gid);
				set_text(action.get(), "command", "none");
				append(out, stable_id("controller-none", {cid, gid}), label("Views.None"), where,
				       text(group.get(), "current").empty(), group_ready, reason, true, action.get());
			}
		}
		auto views = entries(controller.get(), "views");
		for (size_t j = 0; j < obs_data_array_count(views.get()); ++j) {
			auto view = data(obs_data_array_item(views.get(), j));
			const std::string vid = text(view.get(), "id");
			auto action = target("controller");
			set_text(action.get(), "controllerId", cid);
			set_text(action.get(), "viewId", vid);
			set_text(action.get(), "command", "select");
			append(out, stable_id("controller-view", {cid, vid}), text(view.get(), "name"),
			       section({scene, name, label("Views.CombinedViews")}),
			       obs_data_get_bool(view.get(), "active"),
			       ready && obs_data_get_bool(view.get(), "available"),
			       ready ? text(view.get(), "reason") : controller_reason, true, action.get());
		}
	}
	return aliases;
}

Data effective_settings(const Inventory::Filter &filter, const Alias *alias)
{
	if (alias)
		return object(alias->definition.get(), "settings");
	auto saved = data(obs_source_get_settings(filter.value.get()));
	auto resolved = data(obs_data_get_defaults(saved.get()));
	obs_data_apply(resolved.get(), saved.get());
	return resolved;
}
bool alias_ready(const Alias *alias)
{
	return !alias ||
	       (obs_data_get_bool(alias->controller.get(), "enabled") &&
		obs_data_get_bool(alias->controller.get(), "available") &&
		obs_data_get_bool(alias->group.get(), "enabled") && obs_data_get_bool(alias->group.get(), "available"));
}

void legacy_modes(obs_data_array_t *out, const Inventory &inventory, const Inventory::Filter &filter,
		  const Alias *alias, const std::vector<SceneModesLook> &looks)
{
	const std::string parent_name = obs_source_get_name(filter.parent.get());
	const std::string filter_name = obs_source_get_name(filter.value.get());
	const auto found = std::find_if(looks.begin(), looks.end(), [&](const SceneModesLook &look) {
		return look.scene == parent_name && look.filter == filter_name;
	});
	if (found == looks.end())
		return;
	const auto &look = *found;
	const std::string fid = obs_source_get_uuid(filter.value.get());
	const bool ready = look.enabled && alias_ready(alias);
	const auto settings = effective_settings(filter, alias);
	std::vector<std::string> covered{obs_source_get_uuid(filter.parent.get())};
	bool scenes_exist = true;
	for (const auto &name : look.scenes) {
		auto scene = source(obs_get_source_by_name(name.c_str()));
		if (!scene || !scene_of(scene.get()))
			scenes_exist = false;
		else
			covered.emplace_back(obs_source_get_uuid(scene.get()));
	}
	for (const auto &mode : look.modes) {
		bool present = scenes_exist;
		const std::string key = "mode|" + mode.id + "|sources";
		for (const auto &name : pick_list_read(settings.get(), key.c_str(), false))
			present = present && std::any_of(covered.begin(), covered.end(), [&](const std::string &scene) {
					  return inventory.find(scene, name) != nullptr;
				  });
		auto action = target("mode");
		set_text(action.get(), "filterUuid", fid);
		set_text(action.get(), "parentUuid", obs_source_get_uuid(filter.parent.get()));
		set_text(action.get(), "modeId", mode.id);
		set_text(action.get(), "command", "on");
		append(out, stable_id("mode", {fid, mode.id}), mode.name.empty() ? mode.id : mode.name,
		       section({parent_name, filter_name}), look.current == mode.id, ready && present,
		       !ready ? label("Controls.Disabled") : label("Controls.Missing"), !alias, action.get());
	}
	if (look.allow_none) {
		auto action = target("mode");
		set_text(action.get(), "filterUuid", fid);
		set_text(action.get(), "parentUuid", obs_source_get_uuid(filter.parent.get()));
		set_text(action.get(), "command", "none");
		append(out, stable_id("mode-none", {fid}), label("Views.None"), section({parent_name, filter_name}),
		       look.current.empty(), ready && scenes_exist,
		       !ready ? label("Controls.Disabled") : label("Controls.Missing"), !alias, action.get());
	}
}

void legacy_link(obs_data_array_t *out, const Inventory &inventory, const Inventory::Filter &filter, const Alias *alias)
{
	auto settings = effective_settings(filter, alias);
	const long long mode = obs_data_get_int(settings.get(), "mode");
	if (mode != 0 && mode != 1)
		return; // Follow is an automatic rule, never a manual choice.
	const std::string fid = obs_source_get_uuid(filter.value.get());
	const std::string parent_id = obs_source_get_uuid(filter.parent.get());
	const std::string where =
		section({obs_source_get_name(filter.parent.get()), obs_source_get_name(filter.value.get())});
	const auto members = pick_list_read(settings.get(), "items", false);
	bool present = !members.empty(), all_on = true, all_off = true;
	size_t visible_count = 0;
	for (const auto &name : members) {
		const auto *item = inventory.find(parent_id, name);
		present = present && item;
		all_on = all_on && item && item->visible;
		all_off = all_off && item && !item->visible;
		if (item && item->visible)
			++visible_count;
	}
	const bool enabled = obs_source_enabled(filter.value.get()) && alias_ready(alias);
	const std::string reason = !enabled ? label("Controls.Disabled") : label("Controls.Missing");
	if (mode == 0) {
		for (const auto &name : members) {
			const auto *item = inventory.find(parent_id, name);
			auto member = source(obs_get_source_by_name(name.c_str()));
			const std::string sid = item     ? item->source_id
						: member ? obs_source_get_uuid(member.get())
							 : "";
			auto action = target("link");
			set_text(action.get(), "filterUuid", fid);
			set_text(action.get(), "parentUuid", parent_id);
			set_text(action.get(), "sourceUuid", sid);
			obs_data_set_bool(action.get(), "shown", true);
			const std::string id = sid.empty() ? stable_id("link-missing", {fid, name})
							   : stable_id("link-choice", {fid, sid});
			append(out, id, name, where, item && item->visible && visible_count == 1,
			       enabled && present && item && member && sid == obs_source_get_uuid(member.get()), reason,
			       !alias, action.get());
		}
	} else {
		for (const bool shown : {true, false}) {
			auto action = target("link");
			set_text(action.get(), "filterUuid", fid);
			set_text(action.get(), "parentUuid", parent_id);
			obs_data_set_bool(action.get(), "shown", shown);
			append(out, stable_id("link-set", {fid, shown ? "on" : "off"}),
			       label(shown ? "Views.On" : "Views.Off"), where, present && (shown ? all_on : all_off),
			       enabled && present, reason, !alias, action.get());
		}
	}
}

void ordinary_sources(obs_data_array_t *out, const Inventory &inventory)
{
	std::set<std::string> included;
	for (const auto &[root_id, items] : inventory.items) {
		(void)root_id;
		for (const auto &item : items) {
			const std::string id =
				stable_id("source", {item.owner_id, std::to_string(item.id), item.source_id});
			if (!included.insert(id).second)
				continue;
			auto action = target("source");
			set_text(action.get(), "ownerUuid", item.owner_id);
			set_text(action.get(), "sourceUuid", item.source_id);
			obs_data_set_int(action.get(), "itemId", item.id);
			append(out, id, item.name, section({item.owner_name, label("Controls.Sources")}), item.visible,
			       true, {}, false, action.get());
		}
	}
}

Array catalog()
{
	auto out = array();
	scene_controls(out.get());
	const auto aliases = controller_controls(out.get());
	const Inventory inventory;
	const auto looks = scene_modes_list(nullptr);
	for (const auto &filter : inventory.filters) {
		const auto found = aliases.find(obs_source_get_uuid(filter.value.get()));
		const Alias *alias = found == aliases.end() ? nullptr : &found->second;
		if (is_kind(filter.value.get(), kSceneModesId))
			legacy_modes(out.get(), inventory, filter, alias, looks);
		else if (is_kind(filter.value.get(), kVisibilityLinkId))
			legacy_link(out.get(), inventory, filter, alias);
		else if (is_kind(filter.value.get(), kShowInScenesId)) {
			const std::string fid = obs_source_get_uuid(filter.value.get());
			auto action = target("show-in-scenes");
			set_text(action.get(), "filterUuid", fid);
			set_text(action.get(), "parentUuid", obs_source_get_uuid(filter.parent.get()));
			append(out.get(), stable_id("show-in-scenes", {fid}), obs_source_get_name(filter.value.get()),
			       obs_source_get_name(filter.parent.get()), obs_source_enabled(filter.value.get()), true,
			       {}, true, action.get());
		}
	}
	ordinary_sources(out.get(), inventory);
	return out;
}

bool press_source(obs_data_t *action, std::string &why)
{
	auto owner = source(obs_get_source_by_uuid(obs_data_get_string(action, "ownerUuid")));
	obs_scene_t *scene = owner ? scene_of(owner.get()) : nullptr;
	if (!scene || obs_source_removed(owner.get())) {
		why = label("Controls.Missing");
		return false;
	}
	struct Selection {
		long long id;
		std::string uuid;
		obs_sceneitem_t *item = nullptr;
		bool visible = false;
		~Selection() { obs_sceneitem_release(item); }
	} selected{obs_data_get_int(action, "itemId"), text(action, "sourceUuid")};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *opaque) {
			auto &selected = *static_cast<Selection *>(opaque);
			if (obs_sceneitem_get_id(item) != selected.id ||
			    selected.uuid != obs_source_get_uuid(obs_sceneitem_get_source(item)))
				return true;
			obs_sceneitem_addref(item);
			selected.item = item;
			selected.visible = obs_sceneitem_visible(item);
			return false;
		},
		&selected);
	if (!selected.item || obs_sceneitem_get_scene(selected.item) != scene) {
		why = label("Controls.Missing");
		return false;
	}
	set_item_visible(selected.item, !selected.visible);
	return true;
}

} // namespace

void view_controls_list(obs_data_t *response)
{
	if (!response)
		return;
	const bool ready = frontend_thread() && scenes_loaded();
	auto controls = ready ? catalog() : array();
	obs_data_set_array(response, "controls", controls.get());
	obs_data_set_bool(response, "available", ready);
	set_text(response, "reason", ready ? "" : label("Controls.NotReady"));
}

bool view_control_press(obs_data_t *requested, std::string &why, bool show_scene)
{
	if (!requested || text(requested, "id").empty() || !frontend_thread() || !scenes_loaded()) {
		why = label("Controls.NotReady");
		return false;
	}
	// Saved pins carry an ID, not permission to operate arbitrary parameters.
	// Match that ID and dispatch only the newly resolved native target.
	const std::string id = text(requested, "id");
	auto controls = catalog();
	Data chosen;
	for (size_t i = 0; i < obs_data_array_count(controls.get()); ++i) {
		auto entry = data(obs_data_array_item(controls.get(), i));
		if (text(entry.get(), "id") == id) {
			chosen = std::move(entry);
			break;
		}
	}
	if (!chosen) {
		why = label("Controls.Missing");
		return false;
	}
	if (!obs_data_get_bool(chosen.get(), "available")) {
		why = text(chosen.get(), "reason");
		return false;
	}
	auto action = object(chosen.get(), "target");
	const std::string kind = text(action.get(), "kind");
	if (kind == "scene") {
		auto scene = source(obs_get_source_by_uuid(obs_data_get_string(action.get(), "sceneUuid")));
		if (!scene || obs_obj_is_private(scene.get()) || obs_source_removed(scene.get()) ||
		    !obs_scene_from_source(scene.get()) || obs_source_is_group(scene.get())) {
			why = label("Controls.Missing");
			return false;
		}
		GoTo go;
		go.kind = "Control Panel";
		go.filter = "Scene";
		go.target = obs_source_get_name(scene.get());
		go.target_uuid = obs_source_get_uuid(scene.get());
		go_to_scene(std::move(go));
		return true;
	}
	if (kind == "controller")
		return scene_controller_command(obs_data_get_string(action.get(), "controllerId"),
						obs_data_get_string(action.get(), "groupId"),
						obs_data_get_string(action.get(), "choiceId"),
						obs_data_get_string(action.get(), "viewId"),
						obs_data_get_string(action.get(), "command"), why, show_scene);
	if (kind == "source")
		return press_source(action.get(), why);
	auto filter = source(obs_get_source_by_uuid(obs_data_get_string(action.get(), "filterUuid")));
	auto parent = source(obs_get_source_by_uuid(obs_data_get_string(action.get(), "parentUuid")));
	if (!filter || !parent || obs_source_removed(filter.get()) || obs_source_removed(parent.get()) ||
	    obs_filter_get_parent(filter.get()) != parent.get()) {
		why = label("Controls.Missing");
		return false;
	}
	if (kind == "mode" && is_kind(filter.get(), kSceneModesId))
		return scene_modes_press(obs_source_get_name(parent.get()), obs_source_get_name(filter.get()),
					 obs_data_get_string(action.get(), "modeId"),
					 obs_data_get_string(action.get(), "command"), why, show_scene);
	if (kind == "link" && is_kind(filter.get(), kVisibilityLinkId)) {
		const std::string uuid = text(action.get(), "sourceUuid");
		auto member = uuid.empty() ? Source() : source(obs_get_source_by_uuid(uuid.c_str()));
		if (!uuid.empty() && (!member || obs_source_removed(member.get()))) {
			why = label("Controls.Missing");
			return false;
		}
		return visibility_link_press(filter.get(), member ? obs_source_get_name(member.get()) : "",
					     obs_data_get_bool(action.get(), "shown"), why);
	}
	if (kind == "show-in-scenes" && is_kind(filter.get(), kShowInScenesId)) {
		bool enabled = false;
		return show_in_scenes_press(parent.get(), obs_source_get_name(filter.get()), "toggle", "filter",
					    enabled, why);
	}
	why = label("Controls.Missing");
	return false;
}

} // namespace stagekit
