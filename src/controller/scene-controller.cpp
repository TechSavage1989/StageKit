/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene-controller.hpp"
#include "views-dock.hpp"

#include "../api/vendor-api.hpp"
#include "../common/filter-input.hpp"
#include "../common/go-to-scene.hpp"
#include "../common/item-transition.hpp"
#include "../common/loading.hpp"
#include "../common/owner-ref.hpp"
#include "../common/pick-list.hpp"
#include "../common/scene-util.hpp"
#include "../common/source-rename.hpp"
#include "../common/timing.hpp"
#include "../placement/scene-modes.hpp"
#include "../placement/visibility-link.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {
namespace {

using Data = std::shared_ptr<obs_data_t>;
using Array = std::shared_ptr<obs_data_array_t>;
Data data(obs_data_t *value = nullptr)
{
	return Data(value ? value : obs_data_create(), obs_data_release);
}
Array array(obs_data_array_t *value = nullptr)
{
	return Array(value ? value : obs_data_array_create(), obs_data_array_release);
}
Data copy(obs_data_t *value)
{
	auto result = data();
	if (value)
		obs_data_apply(result.get(), value);
	return result;
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

struct SourceRef {
	obs_source_t *source = nullptr;
	explicit SourceRef(obs_source_t *value) : source(value) {}
	~SourceRef() { obs_source_release(source); }
	SourceRef(const SourceRef &) = delete;
	SourceRef &operator=(const SourceRef &) = delete;
};

std::string fresh_id()
{
	char *value = os_generate_uuid();
	std::string id = value ? value : "";
	bfree(value);
	return id;
}

bool is_kind(obs_source_t *source, const char *id)
{
	const char *actual = source ? obs_source_get_unversioned_id(source) : nullptr;
	return actual && strcmp(actual, id) == 0;
}

std::string mode_key(const std::string &id, const char *part)
{
	return "mode|" + id + "|" + part;
}
bool contains(const std::vector<std::string> &list, const std::string &value)
{
	return std::find(list.begin(), list.end(), value) != list.end();
}

bool set_id(obs_data_t *entry, std::set<std::string> &seen, const char *kind, std::string &why)
{
	std::string id = text(entry, "id");
	if (id.empty()) {
		id = fresh_id();
		obs_data_set_string(entry, "id", id.c_str());
	}
	if (id.empty() || !seen.insert(id).second) {
		why = std::string("Every ") + kind + " needs its own stable ID";
		return false;
	}
	if (text(entry, "name").empty()) {
		why = std::string("Every ") + kind + " needs a name";
		return false;
	}
	return true;
}

std::vector<std::string> choices_of(obs_data_t *group)
{
	const std::string kind = text(group, "kind");
	auto settings = object(group, "settings");
	if (kind == "modes")
		return pick_list_read(settings.get(), "modes", false);
	if (kind == "together")
		return {"on", "off"};
	std::vector<std::string> choices;
	if (kind == "one") {
		auto ids = object(group, "choiceIds");
		for (const auto &name : pick_list_read(settings.get(), "items", false))
			choices.push_back(text(ids.get(), name.c_str()));
	}
	return choices;
}

// Validate a complete edit before it can replace a running configuration.
// References that disappear later stay saved and are shown as unavailable.
bool validate(obs_data_t *config, std::string &why)
{
	if (!config) {
		why = "config is required";
		return false;
	}
	const long long version = obs_data_get_int(config, "schema");
	if (version != 0 && version != 1) {
		why = "This Scene Controller configuration uses an unsupported schema";
		return false;
	}
	obs_data_set_int(config, "schema", 1);
	auto groups = entries(config, "groups");
	std::set<std::string> group_ids;
	std::map<std::string, Data> by_id;
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto group = data(obs_data_array_item(groups.get(), i));
		if (!set_id(group.get(), group_ids, "group", why))
			return false;
		const std::string kind = text(group.get(), "kind");
		if (kind != "modes" && kind != "one" && kind != "together" && kind != "follow") {
			why = "A group has an unsupported rule kind";
			return false;
		}
		if (!obs_data_has_user_value(group.get(), "enabled"))
			obs_data_set_bool(group.get(), "enabled", true);
		auto origin = object(group.get(), "origin");
		if (!text(origin.get(), "filterUuid").empty() && !obs_data_get_bool(origin.get(), "activate") &&
		    obs_data_get_bool(group.get(), "enabled")) {
			why = "Activate this imported group before enabling it; its original filter still owns the rule";
			return false;
		}
		auto settings = object(group.get(), "settings");
		item_change_defaults(settings.get());
		if (kind == "modes") {
			std::set<std::string> ids;
			auto modes = entries(settings.get(), "modes");
			for (size_t j = 0; j < obs_data_array_count(modes.get()); j++) {
				auto mode = data(obs_data_array_item(modes.get(), j));
				const std::string id = text(mode.get(), "value");
				if (id.empty() || !ids.insert(id).second) {
					why = "Every mode in a group needs its own stable ID";
					return false;
				}
			}
			if (!obs_data_has_user_value(settings.get(), "mode_now") && !ids.empty()) {
				const auto ordered = pick_list_read(settings.get(), "modes", false);
				obs_data_set_string(settings.get(), "mode_now", ordered.front().c_str());
			}
		} else {
			obs_data_set_int(settings.get(), "mode", kind == "one" ? 0 : kind == "together" ? 1 : 2);
			if (kind == "follow" && (obs_data_get_int(settings.get(), "lead_rule") < 0 ||
						 obs_data_get_int(settings.get(), "lead_rule") > 1)) {
				why = "Followers show when must be Any lead or All leads";
				return false;
			}
			if (kind == "one") {
				auto ids = object(group.get(), "choiceIds");
				std::set<std::string> used;
				for (const auto &name : pick_list_read(settings.get(), "items", false)) {
					std::string id = text(ids.get(), name.c_str());
					if (id.empty()) {
						id = fresh_id();
						obs_data_set_string(ids.get(), name.c_str(), id.c_str());
					}
					if (id.empty() || !used.insert(id).second) {
						why = "Every source choice in a group needs its own stable ID";
						return false;
					}
				}
				obs_data_set_obj(group.get(), "choiceIds", ids.get());
			}
		}
		obs_data_set_obj(group.get(), "settings", settings.get());
		by_id.emplace(text(group.get(), "id"), group);
	}
	obs_data_set_array(config, "groups", groups.get());
	auto views = entries(config, "views");
	std::set<std::string> view_ids;
	for (size_t i = 0; i < obs_data_array_count(views.get()); i++) {
		auto view = data(obs_data_array_item(views.get(), i));
		if (!set_id(view.get(), view_ids, "view", why))
			return false;
		auto selections = entries(view.get(), "selections");
		if (!obs_data_array_count(selections.get()) && text(view.get(), "scene").empty()) {
			why = "A view needs a group choice or an explicit scene destination";
			return false;
		}
		std::set<std::string> selected;
		for (size_t j = 0; j < obs_data_array_count(selections.get()); j++) {
			auto selection = data(obs_data_array_item(selections.get(), j));
			const std::string group_id = text(selection.get(), "groupId");
			const std::string choice = text(selection.get(), "choiceId");
			const auto found = by_id.find(group_id);
			if (found == by_id.end() || !selected.insert(group_id).second) {
				why = "A view must reference each existing group at most once";
				return false;
			}
			auto settings = object(found->second.get(), "settings");
			const bool none = choice.empty() && text(found->second.get(), "kind") == "modes" &&
					  obs_data_get_bool(settings.get(), "allow_none");
			if (!none && !contains(choices_of(found->second.get()), choice)) {
				why = "A view refers to a choice that is not in its group";
				return false;
			}
		}
	}
	obs_data_set_array(config, "views", views.get());
	return true;
}

struct LinkBinding {
	std::string scene, uuid;
	std::shared_ptr<VisibilityLinkRuntime> engine;
};
struct Group {
	std::string id, name, kind;
	Data definition, settings;
	std::vector<std::string> scenes;
	bool enabled = true;
	std::string origin;
	bool imported = false;
	std::string error;
	std::shared_ptr<SceneModesRuntime> modes;
	std::vector<LinkBinding> links;
};
struct Runtime {
	Data config;
	std::vector<Group> groups;
	std::string error;
	uint64_t revision = 0;
};
struct Command {
	std::string group, choice, view, how;
};
struct Plan {
	std::vector<std::pair<Group *, std::string>> selections;
	std::string scene;
	bool combined = false;
};

bool scene_exists(const std::string &name)
{
	SourceRef scene(obs_get_source_by_name(name.c_str()));
	return scene.source && obs_scene_from_source(scene.source) && !obs_source_is_group(scene.source);
}

bool present(const std::string &scene_name, const std::string &source_name, bool *shown = nullptr)
{
	SourceRef scene(obs_get_source_by_name(scene_name.c_str()));
	obs_sceneitem_t *item = scene.source ? find_item(scene.source, source_name) : nullptr;
	if (shown)
		*shown = item && obs_sceneitem_visible(item);
	return item != nullptr;
}

bool group_enabled(const Group &group)
{
	if (!group.imported)
		return group.enabled;
	SourceRef legacy(obs_get_source_by_uuid(group.origin.c_str()));
	return legacy.source ? obs_source_enabled(legacy.source) : group.enabled;
}

bool group_available(const Group &group, std::string &why)
{
	if (!group.error.empty()) {
		why = group.error;
		return false;
	}
	if (group.scenes.empty()) {
		why = "Attach Scene Controller to a scene";
		return false;
	}
	for (const auto &scene : group.scenes)
		if (!scene_exists(scene)) {
			why = "Scene not found: " + scene;
			return false;
		}
	return true;
}

std::string source_choice(const Group &group, const std::string &choice)
{
	auto ids = object(group.definition.get(), "choiceIds");
	for (const auto &name : pick_list_read(group.settings.get(), "items", false))
		if (text(ids.get(), name.c_str()) == choice)
			return name;
	return {};
}

bool choice_available(const Group &group, const std::string &choice, std::string &why)
{
	if (!group_available(group, why))
		return false;
	const auto choices = choices_of(group.definition.get());
	if (group.kind == "modes" && choice.empty() && obs_data_get_bool(group.settings.get(), "allow_none"))
		return true;
	if (!contains(choices, choice)) {
		why = "The choice does not belong to this group";
		return false;
	}
	std::vector<std::string> sources;
	if (group.kind == "modes")
		sources = pick_list_read(group.settings.get(), mode_key(choice, "sources").c_str(), false);
	else if (group.kind == "one")
		sources.push_back(source_choice(group, choice));
	else
		sources = pick_list_read(group.settings.get(), "items", false);
	if (group.kind != "modes" && sources.empty()) {
		why = "Add sources to this group";
		return false;
	}
	for (const auto &name : sources) {
		bool any = false;
		for (const auto &scene : group.scenes) {
			const bool exists = present(scene, name);
			any = any || exists;
			if (!exists && group.kind != "modes") {
				why = "Source not found in " + scene + ": " + name;
				return false;
			}
		}
		if (!any) {
			why = "Source not found in the covered scenes: " + name;
			return false;
		}
	}
	return true;
}

std::string current_choice(const Group &group)
{
	if (group.modes)
		return group.modes->runtime_look().current;
	if (group.kind == "follow" || group.scenes.empty())
		return {};
	const auto names = pick_list_read(group.settings.get(), "items", false);
	if (names.empty())
		return {};
	bool all_on = true, all_off = true;
	std::string selected;
	for (const auto &scene : group.scenes) {
		std::string here;
		for (const auto &name : names) {
			bool shown = false;
			if (!present(scene, name, &shown))
				return {};
			all_on = all_on && shown;
			all_off = all_off && !shown;
			if (shown && group.kind == "one") {
				if (!here.empty())
					return {};
				here = name;
			}
		}
		if (group.kind == "one") {
			if (here.empty() || (!selected.empty() && selected != here))
				return {};
			selected = here;
		}
	}
	if (group.kind == "together")
		return all_on ? "on" : all_off ? "off" : "";
	auto ids = object(group.definition.get(), "choiceIds");
	return text(ids.get(), selected.c_str());
}

Group *find_group(Runtime &runtime, const std::string &id)
{
	for (auto &group : runtime.groups)
		if (group.id == id)
			return &group;
	return nullptr;
}

bool make_plan(Runtime &runtime, const Command &command, Plan &plan, std::string &why)
{
	if (!runtime.error.empty()) {
		why = runtime.error;
		return false;
	}
	if (command.group.empty() == command.view.empty()) {
		why = "Choose exactly one group or one saved view";
		return false;
	}
	if (!command.view.empty()) {
		if (!command.choice.empty() || (command.how != "select" && command.how != "on")) {
			why = "A saved view supports Select only";
			return false;
		}
		auto views = entries(runtime.config.get(), "views");
		Data view;
		for (size_t i = 0; i < obs_data_array_count(views.get()); i++) {
			auto candidate = data(obs_data_array_item(views.get(), i));
			if (text(candidate.get(), "id") == command.view) {
				view = candidate;
				break;
			}
		}
		if (!view) {
			why = "The saved view does not belong to this controller";
			return false;
		}
		plan.combined = true;
		plan.scene = text(view.get(), "scene");
		if (!plan.scene.empty() && !scene_exists(plan.scene)) {
			why = "The view's destination scene is unavailable";
			return false;
		}
		auto selections = entries(view.get(), "selections");
		for (size_t i = 0; i < obs_data_array_count(selections.get()); i++) {
			auto selection = data(obs_data_array_item(selections.get(), i));
			Group *group = find_group(runtime, text(selection.get(), "groupId"));
			const std::string choice = text(selection.get(), "choiceId");
			if (!group || !group_enabled(*group) || !choice_available(*group, choice, why)) {
				if (why.empty())
					why = "A group used by this view is missing or disabled";
				return false;
			}
			plan.selections.emplace_back(group, choice);
		}
		return true;
	}
	Group *group = find_group(runtime, command.group);
	if (!group) {
		why = "The group does not belong to this controller";
		return false;
	}
	if (!group_enabled(*group)) {
		why = "The group is disabled";
		return false;
	}
	std::string choice = command.choice;
	const auto choices = choices_of(group->definition.get());
	if (command.how == "next") {
		if ((group->kind != "modes" && group->kind != "one") || choices.empty()) {
			why = "Next requires an ordered choice group";
			return false;
		}
		const std::string current = current_choice(*group);
		const auto found = std::find(choices.begin(), choices.end(), current);
		choice = found == choices.end() || found + 1 == choices.end() ? choices.front() : *(found + 1);
	} else if (command.how == "none" || command.how == "toggle") {
		if (group->kind != "modes" || !group->modes) {
			why = "Toggle and None require a modes group";
			return false;
		}
		const auto state = group->modes->runtime_look();
		if (command.how == "none") {
			if (!state.allow_none) {
				why = "This group does not allow no mode";
				return false;
			}
			choice.clear();
		} else if (choice == state.current) {
			if (state.allow_none)
				choice.clear();
			else if (!state.previous.empty() && state.previous != state.current)
				choice = state.previous;
			else {
				why = "This group has no previous mode to return to";
				return false;
			}
		}
	} else if (command.how != "select" && command.how != "on") {
		why = "command must be select, on, next, toggle or none";
		return false;
	}
	if (!choice_available(*group, choice, why))
		return false;
	plan.selections.emplace_back(group, choice);
	return true;
}

class Controller;
std::mutex registry_mutex;
std::vector<Controller *> controllers;
std::mutex origins_mutex;
std::map<std::string, std::pair<Controller *, std::string>> origin_claims;
std::mutex selected_scene_mutex;
std::string selected_scene;
bool frontend_registered = false;

bool validate_origins(Controller &controller, obs_data_t *config, std::string &why);
bool claim_origins(Controller &controller, obs_data_t *config, uint64_t epoch, std::string &why);
bool origin_targets(Controller &controller, obs_data_t *config, std::map<std::string, std::string> &wanted,
		    std::string &why);

void release_origins(Controller *controller)
{
	std::lock_guard lock(origins_mutex);
	for (auto it = origin_claims.begin(); it != origin_claims.end();)
		if (it->second.first == controller)
			it = origin_claims.erase(it);
		else
			++it;
}

void retain_origins(Controller *controller, const std::shared_ptr<Runtime> &running, obs_data_t *pending)
{
	std::set<std::string> retained;
	if (running && running->error.empty())
		for (const auto &group : running->groups)
			if (group.imported)
				retained.insert(group.origin);
	if (pending) {
		auto groups = entries(pending, "groups");
		for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
			auto group = data(obs_data_array_item(groups.get(), i));
			auto origin = object(group.get(), "origin");
			if (obs_data_get_bool(origin.get(), "activate"))
				retained.insert(text(origin.get(), "filterUuid"));
		}
	}
	std::lock_guard lock(origins_mutex);
	for (auto it = origin_claims.begin(); it != origin_claims.end();)
		if (it->second.first == controller && !retained.count(it->first))
			it = origin_claims.erase(it);
		else
			++it;
}

void merge_runtime_state(obs_data_t *config, const std::shared_ptr<Runtime> &running)
{
	if (!running)
		return;
	auto groups = entries(config, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto definition = data(obs_data_array_item(groups.get(), i));
		Group *group = find_group(*running, text(definition.get(), "id"));
		if (!group || group->kind != text(definition.get(), "kind"))
			continue;
		if (group->modes) {
			auto settings = object(definition.get(), "settings");
			const auto state = group->modes->runtime_look();
			const auto ids = pick_list_read(settings.get(), "modes", false);
			if (state.current.empty() || contains(ids, state.current))
				obs_data_set_string(settings.get(), "mode_now", state.current.c_str());
			if (state.previous.empty() || contains(ids, state.previous))
				obs_data_set_string(settings.get(), "mode_before", state.previous.c_str());
			obs_data_set_obj(definition.get(), "settings", settings.get());
		}
	}
	obs_data_set_array(config, "groups", groups.get());
}

void retire_removed(const std::shared_ptr<Runtime> &old, const std::shared_ptr<Runtime> &next = {},
		    bool clear_aliases = true)
{
	if (!old)
		return;
	std::set<const SceneModesRuntime *> modes;
	std::set<const VisibilityLinkRuntime *> links;
	std::set<std::string> origins;
	if (next)
		for (const auto &group : next->groups) {
			if (group.modes)
				modes.insert(group.modes.get());
			for (const auto &link : group.links)
				links.insert(link.engine.get());
			if (group.imported && group.error.empty())
				origins.insert(group.origin);
		}
	for (const auto &group : old->groups) {
		if (group.modes && !modes.count(group.modes.get()))
			group.modes->runtime_retire();
		for (const auto &link : group.links)
			if (!links.count(link.engine.get()))
				link.engine->runtime_retire();
		const bool kept_alias =
			origins.count(group.origin) &&
			(group.modes ? modes.count(group.modes.get())
				     : !group.links.empty() && links.count(group.links.front().engine.get()));
		if (clear_aliases && group.imported && group.error.empty() && (group.modes || !group.links.empty()) &&
		    !kept_alias) {
			SourceRef legacy(obs_get_source_by_uuid(group.origin.c_str()));
			if (legacy.source) {
				if (group.kind == "modes")
					scene_modes_unforward(legacy.source, group.modes);
				else
					visibility_link_unforward(legacy.source, group.links.front().engine);
			}
		}
	}
}

class Controller {
public:
	Controller(obs_data_t *settings, obs_source_t *source) : source_(source), owner_(source)
	{
		runtime_ = std::make_shared<Runtime>();
		runtime_->config = data();
		update(settings);
		{
			std::lock_guard lock(registry_mutex);
			controllers.push_back(this);
		}
		for (const char *signal :
		     {"source_create", "source_remove", "source_filter_add", "source_filter_remove"})
			signal_handler_connect(obs_get_signal_handler(), signal, source_changed, this);
		signal_handler_connect(obs_get_signal_handler(), "source_rename", renamed, this);
		signal_handler_connect(obs_source_get_signal_handler(source_), "enable", changed, this);
	}
	~Controller()
	{
		{
			std::lock_guard lock(registry_mutex);
			controllers.erase(std::remove(controllers.begin(), controllers.end(), this), controllers.end());
		}
		for (const char *signal :
		     {"source_create", "source_remove", "source_filter_add", "source_filter_remove"})
			signal_handler_disconnect(obs_get_signal_handler(), signal, source_changed, this);
		signal_handler_disconnect(obs_get_signal_handler(), "source_rename", renamed, this);
		signal_handler_disconnect(obs_source_get_signal_handler(source_), "enable", changed, this);
		detach();
		unwatch();
		retired_.clear();
	}
	obs_source_t *source() const { return source_; }
	obs_source_t *owner_ref() const { return owner_.get(); }
	obs_source_t *parent_ref()
	{
		std::lock_guard lock(mutex_);
		return attached_ && parent_weak_ ? obs_weak_source_get_source(parent_weak_) : nullptr;
	}
	bool attached(uint64_t epoch) const { return attached_.load() && epoch_.load() == epoch; }
	bool reserve_origins(const std::map<std::string, std::string> &wanted, uint64_t epoch, std::string &why)
	{
		std::lock_guard lock(mutex_);
		return reserve_locked(wanted, epoch, why);
	}
	std::shared_ptr<Runtime> runtime()
	{
		std::lock_guard lock(mutex_);
		return runtime_;
	}
	void attach(obs_source_t *parent)
	{
		obs_weak_source_t *previous;
		{
			std::lock_guard lock(mutex_);
			previous = parent_weak_;
			parent_weak_ = obs_source_get_weak_source(parent);
			++epoch_;
			attached_ = true;
		}
		obs_weak_source_release(previous);
		rebuild_ = true;
		watch_dirty_ = true;
		dirty_ = true;
	}
	void detach()
	{
		attached_ = false;
		const uint64_t epoch = ++epoch_;
		std::shared_ptr<Runtime> old;
		Data resume;
		uint64_t revision;
		bool pending;
		obs_weak_source_t *parent = nullptr;
		{
			std::lock_guard lock(mutex_);
			pending = bool(pending_);
			resume = copy(pending_ ? pending_.get() : runtime_ ? runtime_->config.get() : nullptr);
			revision = revision_;
			old.swap(runtime_);
			parent = parent_weak_;
			parent_weak_ = nullptr;
			commands_.clear();
		}
		// No execution lock or signal disconnection in filter_remove: OBS
		// can call this while a visibility callback still holds its signal.
		// Engine retirement only disables and unregisters the native object.
		retire_removed(old);
		merge_runtime_state(resume.get(), old);
		if (!pending && old) {
			auto groups = entries(resume.get(), "groups");
			for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
				auto definition = data(obs_data_array_item(groups.get(), i));
				Group *group = find_group(*old, text(definition.get(), "id"));
				if (group && group->imported)
					obs_data_set_bool(definition.get(), "enabled", group_enabled(*group));
			}
		}
		{
			std::lock_guard lock(mutex_);
			if (epoch_ == epoch && revision_ == revision) {
				pending_ = resume;
				++revision_;
			}
		}
		release_origins(this);
		obs_weak_source_release(parent);
		{
			std::lock_guard lock(mutex_);
			if (old) {
				retired_.push_back(old);
				// Transfer the last cleanup responsibility while holding the
				// model lock, before the video thread can drain this list.
				old.reset();
			}
		}
		dirty_ = true;
	}
	void update(obs_data_t *settings)
	{
		std::lock_guard lock(mutex_);
		auto config = copy(object(settings, "controller").get());
		std::string why;
		if (!validate(config.get(), why)) {
			last_error_ = why;
			dirty_ = true;
			return;
		}
		pending_ = config;
		++revision_;
		rebuild_ = true;
		dirty_ = true;
	}
	Data configuration(bool include_pending = false)
	{
		std::shared_ptr<Runtime> running;
		Data config;
		bool pending = false;
		{
			std::lock_guard lock(mutex_);
			running = runtime_;
			pending = include_pending && bool(pending_);
			config = include_pending && pending_ ? copy(pending_.get())
				 : running                   ? copy(running->config.get())
				 : pending_                  ? copy(pending_.get())
							     : data();
		}
		merge_runtime_state(config.get(), running);
		auto groups = entries(config.get(), "groups");
		for (size_t i = 0; running && i < obs_data_array_count(groups.get()); i++) {
			auto definition = data(obs_data_array_item(groups.get(), i));
			Group *group = find_group(*running, text(definition.get(), "id"));
			if (!group)
				continue;
			if (!pending && group->imported)
				obs_data_set_bool(definition.get(), "enabled", group_enabled(*group));
		}
		obs_data_set_array(config.get(), "groups", groups.get());
		return config;
	}
	bool configure(obs_data_t *requested, std::string &why)
	{
		const uint64_t epoch = epoch_;
		SourceRef parent(parent_ref());
		if (!attached(epoch) || !parent.source || !obs_scene_from_source(parent.source) ||
		    obs_source_is_group(parent.source)) {
			why = "Attach Scene Controller to a scene";
			return false;
		}
		if (!requested) {
			why = "config is required";
			return false;
		}
		auto config = copy(requested);
		auto running = runtime();
		auto groups = entries(config.get(), "groups");
		for (size_t i = 0; running && i < obs_data_array_count(groups.get()); i++) {
			auto group = data(obs_data_array_item(groups.get(), i));
			Group *previous = find_group(*running, text(group.get(), "id"));
			if (!previous || previous->kind != text(group.get(), "kind"))
				continue;
			if (previous->modes) {
				auto settings = object(group.get(), "settings");
				const auto state = previous->modes->runtime_look();
				const auto ids = pick_list_read(settings.get(), "modes", false);
				if (state.current.empty() || contains(ids, state.current))
					obs_data_set_string(settings.get(), "mode_now", state.current.c_str());
				if (state.previous.empty() || contains(ids, state.previous))
					obs_data_set_string(settings.get(), "mode_before", state.previous.c_str());
				obs_data_set_obj(group.get(), "settings", settings.get());
			}
			if (previous->kind == "one") {
				auto ids = object(group.get(), "choiceIds");
				auto old_ids = object(previous->definition.get(), "choiceIds");
				auto settings = object(group.get(), "settings");
				for (const auto &name : pick_list_read(settings.get(), "items", false)) {
					const std::string id = text(old_ids.get(), name.c_str());
					if (!id.empty())
						obs_data_set_string(ids.get(), name.c_str(), id.c_str());
				}
				obs_data_set_obj(group.get(), "choiceIds", ids.get());
			}
		}
		obs_data_set_array(config.get(), "groups", groups.get());
		std::map<std::string, std::string> wanted;
		if (!validate(config.get(), why) || !validate_origins(*this, config.get(), why) ||
		    !origin_targets(*this, config.get(), wanted, why))
			return false;
		auto settings = data(obs_source_get_settings(source_));
		{
			std::lock_guard lock(mutex_);
			if (!reserve_locked(wanted, epoch, why))
				return false;
			pending_ = config;
			++revision_;
			last_error_.clear();
			// Our UI and video-thread publications share this lock. A tick
			// cannot replace a newly accepted definition with an older copy.
			obs_data_set_obj(settings.get(), "controller", config.get());
			rebuild_ = true;
			dirty_ = true;
		}
		return true;
	}
	bool command(Command command, std::string &why)
	{
		const uint64_t epoch = epoch_;
		SourceRef parent(parent_ref());
		if (!attached(epoch) || !parent.source || !obs_scene_from_source(parent.source) ||
		    obs_source_is_group(parent.source)) {
			why = "Attach Scene Controller to a scene";
			return false;
		}
		if (!obs_source_enabled(source_)) {
			why = "Scene Controller is disabled";
			return false;
		}
		if (!scenes_loaded()) {
			why = "OBS is still loading its scene collection";
			return false;
		}
		auto running = runtime();
		if (!running) {
			why = "Scene Controller is not ready";
			return false;
		}
		Plan plan;
		if (!make_plan(*running, command, plan, why))
			return false;
		std::lock_guard lock(mutex_);
		if (!attached(epoch) || runtime_ != running) {
			why = "Scene Controller changed before the command could be queued";
			return false;
		}
		commands_.push_back(std::move(command));
		return true;
	}
	void snapshot(obs_data_t *out)
	{
		auto running = runtime();
		SourceRef parent_hold(parent_ref());
		obs_source_t *parent = parent_hold.source;
		obs_data_set_string(out, "controllerId", obs_source_get_uuid(source_));
		obs_data_set_string(out, "name", obs_source_get_name(source_));
		obs_data_set_string(out, "scene", parent ? obs_source_get_name(parent) : "");
		obs_data_set_string(out, "sceneUuid", parent ? obs_source_get_uuid(parent) : "");
		obs_data_set_bool(out, "enabled", obs_source_enabled(source_));
		const bool on_scene = attached_ && parent && obs_scene_from_source(parent) &&
				      !obs_source_is_group(parent);
		const bool available = on_scene && running && running->error.empty();
		obs_data_set_bool(out, "available", available);
		obs_data_set_string(out, "reason",
				    !on_scene  ? "Attach Scene Controller to a scene"
				    : !running ? "Scene Controller is not ready"
					       : running->error.c_str());
		auto groups = array();
		std::map<std::string, std::string> current;
		for (const auto &group : running ? running->groups : std::vector<Group>{}) {
			auto entry = data();
			const std::string selected = current_choice(group);
			current[group.id] = selected;
			obs_data_set_string(entry.get(), "id", group.id.c_str());
			obs_data_set_string(entry.get(), "name", group.name.c_str());
			obs_data_set_string(entry.get(), "kind", group.kind.c_str());
			obs_data_set_bool(entry.get(), "enabled", group_enabled(group));
			std::string reason;
			obs_data_set_bool(entry.get(), "available", group_available(group, reason));
			obs_data_set_string(entry.get(), "reason", reason.c_str());
			obs_data_set_string(entry.get(), "current", selected.c_str());
			obs_data_set_bool(entry.get(), "allowNone",
					  group.kind == "modes" &&
						  obs_data_get_bool(group.settings.get(), "allow_none"));
			if (group.modes)
				obs_data_set_string(entry.get(), "previous",
						    group.modes->runtime_look().previous.c_str());
			auto choices = array();
			for (const auto &id : choices_of(group.definition.get())) {
				auto choice = data();
				std::string name, type;
				if (group.kind == "modes") {
					name = text(group.settings.get(), mode_key(id, "name").c_str());
					if (name.empty())
						name = id;
					type = text(group.settings.get(), mode_key(id, "type").c_str());
				} else if (group.kind == "one") {
					name = source_choice(group, id);
				} else {
					name = obs_module_text(id == "on" ? "Views.On" : "Views.Off");
				}
				obs_data_set_string(choice.get(), "id", id.c_str());
				obs_data_set_string(choice.get(), "name", name.c_str());
				obs_data_set_string(choice.get(), "type", type.c_str());
				obs_data_set_bool(choice.get(), "active", selected == id);
				reason.clear();
				obs_data_set_bool(choice.get(), "available", choice_available(group, id, reason));
				obs_data_set_string(choice.get(), "reason", reason.c_str());
				obs_data_array_push_back(choices.get(), choice.get());
			}
			obs_data_set_array(entry.get(), "choices", choices.get());
			obs_data_array_push_back(groups.get(), entry.get());
		}
		obs_data_set_array(out, "groups", groups.get());
		auto views = array();
		auto configured = running ? entries(running->config.get(), "views") : array();
		for (size_t i = 0; i < obs_data_array_count(configured.get()); i++) {
			auto definition = data(obs_data_array_item(configured.get(), i));
			auto view = data();
			for (const char *key : {"id", "name", "type"})
				obs_data_set_string(view.get(), key, obs_data_get_string(definition.get(), key));
			Plan plan;
			std::string reason;
			const bool ready =
				make_plan(*running, {"", "", text(definition.get(), "id"), "select"}, plan, reason);
			bool active = ready;
			for (const auto &[group, choice] : plan.selections)
				active = active && current[group->id] == choice;
			if (!plan.scene.empty()) {
				std::lock_guard lock(selected_scene_mutex);
				active = active && plan.scene == selected_scene;
			}
			obs_data_set_bool(view.get(), "active", active);
			obs_data_set_bool(view.get(), "available", ready);
			obs_data_set_string(view.get(), "reason", reason.c_str());
			obs_data_array_push_back(views.get(), view.get());
		}
		obs_data_set_array(out, "views", views.get());
		auto config = configuration();
		obs_data_set_obj(out, "config", config.get());
		{
			std::lock_guard lock(mutex_);
			obs_data_set_string(out, "error", last_error_.c_str());
		}
	}
	void tick(float seconds);

private:
	bool reserve_locked(const std::map<std::string, std::string> &wanted, uint64_t epoch, std::string &why)
	{
		if (!attached(epoch)) {
			why = "Scene Controller was removed or moved before this change could be applied";
			return false;
		}
		std::lock_guard lock(origins_mutex);
		for (const auto &[uuid, group] : wanted) {
			const auto found = origin_claims.find(uuid);
			if (found != origin_claims.end() && found->second.first != this) {
				why = "That existing filter is already active in another Scene Controller";
				return false;
			}
		}
		for (const auto &[uuid, group] : wanted)
			origin_claims[uuid] = {this, group};
		return true;
	}
	static void changed(void *opaque, calldata_t *) { static_cast<Controller *>(opaque)->dirty_ = true; }
	static void structure(void *opaque, calldata_t *cd)
	{
		auto *self = static_cast<Controller *>(opaque);
		auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
		if (item && !selectable_item_name(item))
			return;
		self->dirty_ = true;
		self->watch_dirty_ = true;
		if (item && obs_sceneitem_is_group(item))
			self->rebuild_ = true;
	}
	static void source_changed(void *opaque, calldata_t *cd)
	{
		auto *self = static_cast<Controller *>(opaque);
		auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
		if (source && (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE || is_kind(source, kSceneModesId) ||
			       is_kind(source, kVisibilityLinkId)))
			self->rebuild_ = true;
		self->watch_dirty_ = true;
		self->dirty_ = true;
	}
	static void renamed(void *opaque, calldata_t *cd)
	{
		auto *self = static_cast<Controller *>(opaque);
		const char *was = calldata_string(cd, "prev_name"), *now = calldata_string(cd, "new_name");
		if (!was || !now || !*was || !*now || strcmp(was, now) == 0)
			return;
		{
			std::lock_guard lock(self->mutex_);
			self->renames_.emplace_back(was, now);
		}
		self->rebuild_ = true;
		self->dirty_ = true;
	}
	void unwatch()
	{
		std::vector<obs_weak_source_t *> watched;
		watched.swap(watched_);
		for (auto *weak : watched) {
			SourceRef source(obs_weak_source_get_source(weak));
			if (source.source) {
				auto *signals = obs_source_get_signal_handler(source.source);
				for (const char *signal : {"item_add", "item_remove", "reorder", "refresh"})
					signal_handler_disconnect(signals, signal, structure, this);
				for (const char *signal : {"item_visible", "enable"})
					signal_handler_disconnect(signals, signal, changed, this);
			}
			obs_weak_source_release(weak);
		}
	}
	void watch(const std::shared_ptr<Runtime> &running)
	{
		unwatch();
		std::vector<obs_source_t *> sources;
		obs_enum_scenes(
			[](void *opaque, obs_source_t *source) {
				if (auto *held = obs_source_get_ref(source))
					static_cast<std::vector<obs_source_t *> *>(opaque)->push_back(held);
				return true;
			},
			&sources);
		for (const auto &group : running->groups)
			if (group.imported)
				if (auto *legacy = obs_get_source_by_uuid(group.origin.c_str()))
					sources.push_back(legacy);
		for (auto *source : sources) {
			if (!attached_) {
				obs_source_release(source);
				continue;
			}
			auto *signals = obs_source_get_signal_handler(source);
			if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
				for (const char *signal : {"item_add", "item_remove", "reorder", "refresh"})
					signal_handler_connect(signals, signal, structure, this);
				signal_handler_connect(signals, "item_visible", changed, this);
			} else {
				signal_handler_connect(signals, "enable", changed, this);
			}
			watched_.push_back(obs_source_get_weak_source(source));
			obs_source_release(source);
		}
		if (!attached_)
			unwatch();
	}
	std::shared_ptr<Runtime> build(Data config, const std::shared_ptr<Runtime> &previous, uint64_t revision,
				       bool apply_switches, uint64_t epoch, obs_source_t *parent);
	void bind_imports(const std::shared_ptr<Runtime> &running, bool apply_switches, uint64_t epoch,
			  obs_source_t *parent);
	void persist(const std::shared_ptr<Runtime> &running);
	obs_source_t *source_;
	OwnerRef owner_;
	std::mutex mutex_;
	obs_weak_source_t *parent_weak_ = nullptr;
	Data pending_;
	uint64_t revision_ = 0;
	std::shared_ptr<Runtime> runtime_;
	std::vector<std::shared_ptr<Runtime>> retired_;
	std::vector<Command> commands_;
	std::vector<std::pair<std::string, std::string>> renames_;
	std::string last_error_;
	std::vector<obs_weak_source_t *> watched_;
	std::atomic<bool> dirty_{true}, rebuild_{true}, watch_dirty_{true};
	std::atomic<bool> attached_{false};
	std::atomic<uint64_t> epoch_{0};
};

void rename_list(obs_data_t *settings, const char *key, const std::string &was, const std::string &now)
{
	auto rows = entries(settings, key);
	for (size_t i = 0; i < obs_data_array_count(rows.get()); i++) {
		auto row = data(obs_data_array_item(rows.get(), i));
		if (text(row.get(), "value") == was)
			obs_data_set_string(row.get(), "value", now.c_str());
	}
}

void rename_config(obs_data_t *config, const std::string &was, const std::string &now)
{
	auto groups = entries(config, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto group = data(obs_data_array_item(groups.get(), i));
		rename_list(group.get(), "scope", was, now);
		const bool modes = text(group.get(), "kind") == "modes";
		auto settings = object(group.get(), "settings");
		source_settings_renamed(modes ? kSceneModesId : kVisibilityLinkId, settings.get(), was.c_str(),
					now.c_str(), true);
		if (modes)
			scene_modes_rename_settings(settings.get(), was.c_str(), now.c_str());
		auto ids = object(group.get(), "choiceIds");
		const std::string id = text(ids.get(), was.c_str());
		if (!id.empty()) {
			obs_data_set_string(ids.get(), now.c_str(), id.c_str());
			obs_data_erase(ids.get(), was.c_str());
		}
		auto origin = object(group.get(), "origin");
		auto original = object(origin.get(), "settings");
		source_settings_renamed(modes ? kSceneModesId : kVisibilityLinkId, original.get(), was.c_str(),
					now.c_str(), true);
		if (modes)
			scene_modes_rename_settings(original.get(), was.c_str(), now.c_str());
	}
	auto views = entries(config, "views");
	for (size_t i = 0; i < obs_data_array_count(views.get()); i++) {
		auto view = data(obs_data_array_item(views.get(), i));
		if (text(view.get(), "scene") == was)
			obs_data_set_string(view.get(), "scene", now.c_str());
	}
	std::lock_guard lock(selected_scene_mutex);
	if (selected_scene == was)
		selected_scene = now;
}

std::shared_ptr<Runtime> Controller::build(Data config, const std::shared_ptr<Runtime> &previous, uint64_t revision,
					   bool apply_switches, uint64_t epoch, obs_source_t *parent)
{
	auto next = std::make_shared<Runtime>();
	next->config = config;
	next->revision = revision;
	const bool on_scene = parent && obs_scene_from_source(parent) && !obs_source_is_group(parent);
	if (!on_scene)
		next->error = "Attach Scene Controller to a scene";
	else
		claim_origins(*this, config.get(), epoch, next->error);
	auto definitions = entries(config.get(), "groups");
	for (size_t i = 0; i < obs_data_array_count(definitions.get()); i++) {
		Group group;
		group.definition = data(obs_data_array_item(definitions.get(), i));
		group.id = text(group.definition.get(), "id");
		group.name = text(group.definition.get(), "name");
		group.kind = text(group.definition.get(), "kind");
		group.error = next->error;
		group.enabled = obs_data_get_bool(group.definition.get(), "enabled");
		group.settings = object(group.definition.get(), "settings");
		auto origin = object(group.definition.get(), "origin");
		group.origin = text(origin.get(), "filterUuid");
		group.imported = !group.origin.empty() && obs_data_get_bool(origin.get(), "activate");
		if (on_scene)
			group.scenes.push_back(obs_source_get_name(parent));
		for (const auto &name : pick_list_read(group.definition.get(), "scope", false))
			if (!contains(group.scenes, name))
				group.scenes.push_back(name);
		if (group.kind == "modes")
			for (const auto &name : pick_list_read(group.settings.get(), "share_scenes", false))
				if (!contains(group.scenes, name))
					group.scenes.push_back(name);
		const bool enabled = apply_switches ? group.enabled : group_enabled(group);
		group.enabled = enabled;
		obs_data_set_bool(group.definition.get(), "enabled", enabled);
		Group *old = previous ? find_group(*previous, group.id) : nullptr;
		if (old && old->kind != group.kind)
			old = nullptr;
		auto settings = copy(group.settings.get());
		if (next->error.empty() && group.kind == "modes") {
			std::vector<std::string> shared(group.scenes.begin() + 1, group.scenes.end());
			pick_list_write(settings.get(), "share_scenes", false, shared);
			if (old && old->modes) {
				group.modes = old->modes;
				group.modes->runtime_update(settings.get(), enabled);
			} else {
				group.modes = scene_modes_runtime(settings.get(), source_, parent, false);
			}
		} else if (next->error.empty()) {
			for (const auto &name : group.scenes) {
				SourceRef scene(obs_get_source_by_name(name.c_str()));
				if (!scene.source || !obs_scene_from_source(scene.source) ||
				    obs_source_is_group(scene.source))
					continue;
				const std::string uuid = obs_source_get_uuid(scene.source);
				std::shared_ptr<VisibilityLinkRuntime> engine;
				if (old)
					for (const auto &binding : old->links)
						if (binding.uuid == uuid) {
							engine = binding.engine;
							break;
						}
				if (engine)
					engine->runtime_update(settings.get(), enabled);
				else
					engine = visibility_link_runtime(settings.get(), source_, scene.source, false);
				group.links.push_back({name, uuid, std::move(engine)});
			}
		}
		next->groups.push_back(std::move(group));
	}
	return next;
}

void Controller::bind_imports(const std::shared_ptr<Runtime> &running, bool apply_switches, uint64_t epoch,
			      obs_source_t *parent)
{
	if (!running->error.empty())
		return;
	for (const auto &group : running->groups) {
		if (!attached(epoch))
			return;
		if (group.imported) {
			SourceRef legacy(obs_get_source_by_uuid(group.origin.c_str()));
			if (legacy.source && obs_filter_get_parent(legacy.source) == parent) {
				if (group.modes)
					scene_modes_forward(legacy.source, group.modes);
				else if (!group.links.empty())
					visibility_link_forward(legacy.source, group.links.front().engine);
				// An enabled-state edit cannot briefly run the old rule while
				// its new owner is still waiting for its first video tick.
				if (apply_switches && attached(epoch))
					obs_source_set_enabled(legacy.source, group.enabled);
			}
		}
	}
}

void Controller::persist(const std::shared_ptr<Runtime> &running)
{
	auto config = copy(running->config.get());
	merge_runtime_state(config.get(), running);
	auto groups = entries(config.get(), "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto definition = data(obs_data_array_item(groups.get(), i));
		Group *group = find_group(*running, text(definition.get(), "id"));
		if (group && group->imported)
			obs_data_set_bool(definition.get(), "enabled", group_enabled(*group));
	}
	auto settings = data(obs_source_get_settings(source_));
	std::lock_guard lock(mutex_);
	// update() or configure() may queue a newer replacement during this tick.
	// Its newer definition already lives in OBS settings, and must win.
	if (runtime_ == running && revision_ == running->revision && !pending_)
		obs_data_set_obj(settings.get(), "controller", config.get());
}

void Controller::tick(float seconds)
{
	// Keep these references until every local runtime snapshot is released.
	// A final release can synchronously call filter_remove in libobs.
	SourceRef owner(owner_ref());
	const uint64_t epoch = epoch_;
	SourceRef parent(parent_ref());
	std::vector<std::shared_ptr<Runtime>> retired;
	{
		std::lock_guard lock(mutex_);
		retired.swap(retired_);
	}
	retired.clear();
	if (!owner.source || !attached(epoch) || !parent.source) {
		unwatch();
		return;
	}
	bool publish = dirty_.exchange(false);
	if (rebuild_.exchange(false)) {
		Data config;
		std::shared_ptr<Runtime> previous;
		uint64_t revision = 0;
		bool apply_switches = false;
		std::vector<std::pair<std::string, std::string>> renames;
		{
			std::lock_guard lock(mutex_);
			previous = runtime_;
			apply_switches = bool(pending_);
			config = pending_ ? std::move(pending_) : previous ? previous->config : data();
			revision = revision_;
			renames.swap(renames_);
		}
		config = copy(config.get());
		merge_runtime_state(config.get(), previous);
		for (const auto &[was, now] : renames)
			rename_config(config.get(), was, now);
		auto next = build(config, previous, revision, apply_switches, epoch, parent.source);
		std::shared_ptr<Runtime> old;
		bool accepted = false;
		{
			std::lock_guard lock(mutex_);
			if (attached(epoch)) {
				old.swap(runtime_);
				runtime_ = next;
				last_error_ = next->error;
				accepted = true;
			}
		}
		if (!accepted) {
			retire_removed(next, {}, false);
			unwatch();
			return;
		}
		retire_removed(old, next);
		{
			std::lock_guard lock(mutex_);
			if (attached(epoch))
				retain_origins(this, next, pending_.get());
		}
		old.reset();
		bind_imports(next, apply_switches, epoch, parent.source);
		publish = true;
		watch_dirty_ = true;
	}
	auto running = runtime();
	if (!running)
		return;
	if (watch_dirty_.exchange(false))
		watch(running);
	if (!attached(epoch))
		return;
	if (publish)
		for (auto &group : running->groups) {
			const bool enabled = group_enabled(group);
			if (group.modes)
				group.modes->runtime_enabled(enabled);
			for (auto &link : group.links)
				link.engine->runtime_enabled(enabled);
		}
	if (!scenes_loaded()) {
		dirty_ = true;
		return;
	}
	// The inherited engines do their established, change-driven work. No
	// controller snapshot, JSON copy, source inventory or pixel work occurs
	// on an idle tick. New engines finish their load reconciliation first.
	for (auto &group : running->groups)
		if (group.modes)
			group.modes->tick(seconds);
	for (auto &group : running->groups)
		for (auto &link : group.links)
			link.engine->tick(seconds);
	if (!attached(epoch))
		return;
	std::vector<Command> commands;
	{
		std::lock_guard lock(mutex_);
		if (scenes_loaded())
			commands.swap(commands_);
	}
	for (const auto &command : commands) {
		if (!attached(epoch))
			return;
		std::string why;
		Plan plan;
		if (!obs_source_enabled(source_) || !make_plan(*running, command, plan, why)) {
			std::lock_guard lock(mutex_);
			last_error_ = why.empty() ? "Scene Controller is disabled" : why;
			publish = true;
			continue;
		}
		// make_plan validates every member before any engine receives a press.
		// Reserve all target modes before applying the first one, so another
		// independent group's previous mode cannot veto a shared source.
		bool staged = true;
		for (const auto &[group, choice] : plan.selections)
			if (group->modes)
				staged = group->modes->runtime_stage(choice.c_str(), !plan.combined, why) && staged;
		if (staged) {
			for (auto &group : running->groups)
				if (group.modes)
					group.modes->tick(0.0f);
			for (const auto &[group, choice] : plan.selections) {
				if (group->modes)
					continue;
				const std::string source = group->kind == "one" ? source_choice(*group, choice) : "";
				for (auto &link : group->links)
					link.engine->runtime_press(source.c_str(), choice != "off", why);
			}
			for (auto &group : running->groups)
				for (auto &link : group.links)
					link.engine->tick(0.0f);
			if (!plan.scene.empty()) {
				GoTo go;
				go.kind = "Scene Controller";
				go.filter = obs_source_get_name(source_);
				go.target = plan.scene;
				go_to_scene(std::move(go));
			}
		}
		for (auto &group : running->groups)
			if (group.modes)
				group.modes->runtime_unstage();
		{
			std::lock_guard lock(mutex_);
			last_error_ = why;
		}
		publish = true;
	}
	std::vector<Group *> mode_changes;
	for (auto &group : running->groups)
		if (group.modes && group.modes->runtime_changed()) {
			mode_changes.push_back(&group);
			publish = true;
		}
	publish = dirty_.exchange(false) || publish;
	if (!publish)
		return;
	persist(running);
	// Notifications are outside every engine/model lock. Legacy key bindings
	// receive their original event as well as the new controller event.
	for (const auto *group : mode_changes)
		if (group->imported) {
			SourceRef legacy(obs_get_source_by_uuid(group->origin.c_str()));
			if (legacy.source) {
				const auto state = group->modes->runtime_look();
				vendor_emit_scene_mode_changed(legacy.source, state.current, state.previous);
			}
		}
	vendor_emit_scene_controller_changed(obs_source_get_uuid(source_));
}

Controller *find_controller(const char *id, SourceRef &held, std::string &why)
{
	if (!id || !*id) {
		why = "controllerId is required";
		return nullptr;
	}
	held.source = obs_get_source_by_uuid(id);
	if (!is_kind(held.source, kSceneControllerId)) {
		why = "Scene Controller not found";
		return nullptr;
	}
	auto *self = static_cast<Controller *>(obs_obj_get_data(held.source));
	if (!self)
		why = "Scene Controller is unavailable";
	return self;
}

void frontend_event(obs_frontend_event event, void *)
{
	if (event != OBS_FRONTEND_EVENT_SCENE_CHANGED && event != OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED &&
	    event != OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED && event != OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED &&
	    event != OBS_FRONTEND_EVENT_FINISHED_LOADING && event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		return;
	SourceRef scene(obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
								   : obs_frontend_get_current_scene());
	{
		std::lock_guard lock(selected_scene_mutex);
		selected_scene = scene.source ? obs_source_get_name(scene.source) : "";
	}
	std::vector<obs_source_t *> held;
	{
		std::lock_guard lock(registry_mutex);
		for (auto *controller : controllers)
			if (auto *source = controller->owner_ref())
				held.push_back(source);
	}
	for (auto *source : held) {
		vendor_emit_scene_controller_changed(obs_source_get_uuid(source));
		obs_source_release(source);
	}
}

std::vector<obs_source_t *> controller_sources()
{
	std::vector<obs_source_t *> sources;
	std::lock_guard lock(registry_mutex);
	for (auto *controller : controllers)
		if (auto *source = controller->owner_ref())
			sources.push_back(source);
	return sources;
}

bool origin_targets(Controller &controller, obs_data_t *config, std::map<std::string, std::string> &wanted,
		    std::string &why)
{
	SourceRef parent(controller.parent_ref());
	auto groups = entries(config, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto group = data(obs_data_array_item(groups.get(), i));
		auto origin = object(group.get(), "origin");
		if (!obs_data_get_bool(origin.get(), "activate"))
			continue;
		const std::string uuid = text(origin.get(), "filterUuid");
		if (uuid.empty() || !wanted.emplace(uuid, text(group.get(), "id")).second) {
			why = "An existing filter can be active in only one controller group";
			return false;
		}
		const bool modes = text(group.get(), "kind") == "modes";
		const char *expected = modes ? kSceneModesId : kVisibilityLinkId;
		if (text(origin.get(), "type") != expected) {
			why = "The imported filter's type must match its controller group";
			return false;
		}
		SourceRef legacy(obs_get_source_by_uuid(uuid.c_str()));
		if (legacy.source) {
			if (!is_kind(legacy.source, expected) ||
			    obs_filter_get_parent(legacy.source) != parent.source) {
				why = "The imported filter's type and owning scene must match this group";
				return false;
			}
		}
	}
	return true;
}

bool claim_origins(Controller &controller, obs_data_t *config, uint64_t epoch, std::string &why)
{
	std::map<std::string, std::string> wanted;
	if (!origin_targets(controller, config, wanted, why))
		return false;
	// Removed claims remain reserved until their old engines are retired.
	// A pending edit cannot let another controller run the same rule early.
	return controller.reserve_origins(wanted, epoch, why);
}

bool validate_origins(Controller &controller, obs_data_t *config, std::string &why)
{
	std::map<std::string, std::string> wanted;
	if (!origin_targets(controller, config, wanted, why))
		return false;
	auto previous = controller.configuration(true);
	auto old_groups = entries(previous.get(), "groups");
	std::map<std::string, Data> old_origins;
	for (size_t i = 0; i < obs_data_array_count(old_groups.get()); i++) {
		auto old = data(obs_data_array_item(old_groups.get(), i));
		old_origins.emplace(text(old.get(), "id"), object(old.get(), "origin"));
	}
	auto groups = entries(config, "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto group = data(obs_data_array_item(groups.get(), i));
		auto origin = object(group.get(), "origin");
		const auto found = old_origins.find(text(group.get(), "id"));
		const std::string old_uuid = found == old_origins.end() ? "" : text(found->second.get(), "filterUuid");
		const bool was_active = found != old_origins.end() &&
					obs_data_get_bool(found->second.get(), "activate");
		const std::string uuid = text(origin.get(), "filterUuid");
		const bool active = obs_data_get_bool(origin.get(), "activate");
		if (!old_uuid.empty() && (old_uuid != uuid || (was_active && !active))) {
			why = "Remove the imported group to restore its original filter; ownership cannot be cleared in place";
			return false;
		}
		if (active) {
			SourceRef legacy(obs_get_source_by_uuid(uuid.c_str()));
			if (!legacy.source && !(was_active && old_uuid == uuid)) {
				why = "The filter selected for import is unavailable";
				return false;
			}
		}
	}
	return true;
}

} // namespace

void scene_controller_list(obs_data_t *response)
{
	if (!response)
		return;
	auto list = array();
	for (auto *source : controller_sources()) {
		if (auto *self = static_cast<Controller *>(obs_obj_get_data(source))) {
			auto entry = data();
			self->snapshot(entry.get());
			obs_data_array_push_back(list.get(), entry.get());
		}
		obs_source_release(source);
	}
	obs_data_set_array(response, "controllers", list.get());
}

bool scene_controller_get(const char *controller_id, obs_data_t *response, std::string &why)
{
	if (!response) {
		why = "A response object is required";
		return false;
	}
	SourceRef held(nullptr);
	Controller *self = find_controller(controller_id, held, why);
	if (!self)
		return false;
	self->snapshot(response);
	return true;
}

bool scene_controller_configure(const char *controller_id, obs_data_t *config, std::string &why)
{
	SourceRef held(nullptr);
	Controller *self = find_controller(controller_id, held, why);
	if (!self || !config) {
		if (why.empty())
			why = "config is required";
		return false;
	}
	return self->configure(config, why);
}

bool scene_controller_command(const char *controller_id, const char *group_id, const char *choice_id,
			      const char *view_id, const char *command, std::string &why)
{
	SourceRef held(nullptr);
	Controller *self = find_controller(controller_id, held, why);
	if (!self)
		return false;
	if (!command || !*command) {
		why = "command is required";
		return false;
	}
	return self->command({group_id ? group_id : "", choice_id ? choice_id : "", view_id ? view_id : "", command},
			     why);
}

bool scene_controller_create(const char *scene_uuid, const char *name, obs_data_t *response, std::string &why)
{
	if (!scene_uuid || !*scene_uuid || !name || !*name || !response) {
		why = "sceneUuid, name and response are required";
		return false;
	}
	SourceRef scene(obs_get_source_by_uuid(scene_uuid));
	if (!scene.source || !obs_scene_from_source(scene.source) || obs_source_is_group(scene.source)) {
		why = "Choose an available OBS scene";
		return false;
	}
	SourceRef filter(obs_source_create(kSceneControllerId, name, nullptr, nullptr));
	if (!filter.source) {
		why = "Scene Controller could not be created";
		return false;
	}
	obs_source_filter_add(scene.source, filter.source);
	return scene_controller_get(obs_source_get_uuid(filter.source), response, why);
}

bool scene_controller_import(const char *controller_id, const char *legacy_filter_uuid, bool activate,
			     obs_data_t *response, std::string &why)
{
	SourceRef held(nullptr);
	Controller *self = find_controller(controller_id, held, why);
	if (!self)
		return false;
	if (!legacy_filter_uuid || !*legacy_filter_uuid || !response) {
		why = "legacyFilterUuid and response are required";
		return false;
	}
	SourceRef legacy(obs_get_source_by_uuid(legacy_filter_uuid));
	const bool modes = is_kind(legacy.source, kSceneModesId);
	if (!modes && !is_kind(legacy.source, kVisibilityLinkId)) {
		why = "Import supports existing Scene Modes and Link Visibility filters";
		return false;
	}
	SourceRef parent(self->parent_ref());
	if (!parent.source || obs_source_is_group(parent.source) ||
	    obs_filter_get_parent(legacy.source) != parent.source) {
		why = "Import a filter from the same scene as this Scene Controller";
		return false;
	}
	auto config = self->configuration(true);
	auto groups = entries(config.get(), "groups");
	for (size_t i = 0; i < obs_data_array_count(groups.get()); i++) {
		auto existing = data(obs_data_array_item(groups.get(), i));
		auto origin = object(existing.get(), "origin");
		if (text(origin.get(), "filterUuid") == legacy_filter_uuid) {
			why = "This filter is already imported; configure its existing group";
			return false;
		}
	}
	auto saved = data(obs_source_get_settings(legacy.source));
	auto resolved = data(obs_data_get_defaults(saved.get()));
	obs_data_apply(resolved.get(), saved.get());
	auto settings = copy(resolved.get());
	const long long mode = obs_data_get_int(settings.get(), "mode");
	if (!modes && (mode < 0 || mode > 2)) {
		why = "This Link Visibility mode is unsupported; its original settings were left intact";
		return false;
	}
	auto group = data();
	const std::string id = fresh_id();
	obs_data_set_string(group.get(), "id", id.c_str());
	obs_data_set_string(group.get(), "name", obs_source_get_name(legacy.source));
	obs_data_set_string(group.get(), "kind",
			    modes       ? "modes"
			    : mode == 0 ? "one"
			    : mode == 1 ? "together"
					: "follow");
	obs_data_set_bool(group.get(), "enabled", activate && obs_source_enabled(legacy.source));
	obs_data_set_obj(group.get(), "settings", settings.get());
	auto origin = data();
	obs_data_set_string(origin.get(), "filterUuid", legacy_filter_uuid);
	obs_data_set_string(origin.get(), "type", obs_source_get_unversioned_id(legacy.source));
	obs_data_set_string(origin.get(), "name", obs_source_get_name(legacy.source));
	obs_data_set_bool(origin.get(), "activate", activate);
	obs_data_set_bool(origin.get(), "enabled", obs_source_enabled(legacy.source));
	obs_data_set_obj(origin.get(), "settings", resolved.get());
	obs_data_set_obj(group.get(), "origin", origin.get());
	obs_data_array_push_back(groups.get(), group.get());
	obs_data_set_array(config.get(), "groups", groups.get());
	if (!scene_controller_configure(controller_id, config.get(), why))
		return false;
	obs_data_set_string(response, "groupId", id.c_str());
	obs_data_set_bool(response, "activated", activate);
	return true;
}

void register_scene_controller()
{
	obs_source_info info{};
	info.id = kSceneControllerId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = [](void *) {
		return obs_module_text("SceneController.Name");
	};
	info.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
		return new Controller(settings, source);
	};
	info.destroy = [](void *opaque) {
		delete static_cast<Controller *>(opaque);
	};
	info.update = [](void *opaque, obs_data_t *settings) {
		static_cast<Controller *>(opaque)->update(settings);
	};
	info.get_properties = [](void *opaque) {
		auto *properties = obs_properties_create();
		obs_properties_add_button2(
			properties, "configure", obs_module_text("Views.Configure"),
			[](obs_properties_t *, obs_property_t *, void *opaque) {
				if (auto *self = static_cast<Controller *>(opaque))
					show_views_controller(obs_source_get_uuid(self->source()));
				return false;
			},
			opaque);
		auto *help =
			obs_properties_add_text(properties, "help", obs_module_text("Views.SelectHelp"), OBS_TEXT_INFO);
		obs_property_text_set_info_word_wrap(help, true);
		return properties;
	};
	info.video_tick = [](void *opaque, float seconds) {
		static_cast<Controller *>(opaque)->tick(seconds);
	};
	info.video_render = [](void *opaque, gs_effect_t *) {
		skip_filter_input(static_cast<Controller *>(opaque)->source());
	};
	info.filter_add = [](void *opaque, obs_source_t *parent) {
		static_cast<Controller *>(opaque)->attach(parent);
	};
	info.filter_remove = [](void *opaque, obs_source_t *) {
		static_cast<Controller *>(opaque)->detach();
	};
	register_timed(&info);
	if (obs_frontend_get_main_window() && !frontend_registered) {
		frontend_registered = true;
		obs_frontend_add_event_callback(frontend_event, nullptr);
		frontend_event(OBS_FRONTEND_EVENT_SCENE_CHANGED, nullptr);
	}
}

void unregister_scene_controller()
{
	if (frontend_registered) {
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		frontend_registered = false;
	}
}

} // namespace stagekit
