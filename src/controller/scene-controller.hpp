/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <obs.h>
#include "../common/scene-filter-ids.hpp"

#include <string>

namespace stagekit {

void register_scene_controller();
void unregister_scene_controller();

// Snapshots and configuration are copied into caller-owned OBS data. Identity
// is the real filter's UUID, never its editable display name. A group or view
// ID belongs to that controller; a choice ID belongs to its group.
//
// Snapshot: controllerId, name, scene, sceneUuid, enabled, available, reason,
// groups [{id,name,kind,enabled,available,reason,current,previous,allowNone,
//          choices:[{id,name,type,active,available}]}],
// views [{id,name,type,active,available,reason}], config.
// List writes the same snapshots under "controllers".
void scene_controller_list(obs_data_t *response);
bool scene_controller_get(const char *controller_id, obs_data_t *response, std::string &why);
// The newest accepted definition (the config above), including one queued by
// scene_controller_configure that the next tick has not built yet. Editors
// start from and compare against this, so a save is never mistaken for a
// foreign change while it waits for that tick.
bool scene_controller_definition(const char *controller_id, obs_data_t *config, std::string &why);

// Config schema 1:
// groups [{id,name,kind:"modes"|"one"|"together"|"follow",enabled,
//          scope:[{value:scene_name}],settings:{complete native rule settings},
//          choiceIds:{source_name:stable_choice_id},origin:optional_import_record}]
// views [{id,name,type,selections:[{groupId,choiceId}],scene:optional_scene_name}]
// Native rule settings retain every transition, per-source override, placement
// and legacy rule. Lists use OBS's existing [{value:name}] representation.
// For modes: modes:[{value:id}], mode|id|name/type/sources/... .
// For other groups: items, leads, elsewhere, lead_rule (0 any, 1 all), plus
// the existing transition and lock_hidden settings. kind selects mode 0/1/2.
// Missing IDs are allocated once by configure; existing IDs remain unchanged.
// Configuration validates as a whole before it is queued; no partial edits.
// Edits run on the frontend thread. get/list/command accept other callers.
// The owner's scene is implicit in scope; listed scenes are additional.
bool scene_controller_configure(const char *controller_id, obs_data_t *config, std::string &why);

// Existing native panel pins retain these identities while an imported group
// supplies their operation. Revalidate them at acceptance and queued execution;
// a detached original or a replacement member cannot inherit a pending press.
// Ordinary controller commands leave this empty.
struct SceneControllerAliasTarget {
	std::string filter_uuid, parent_uuid, source_uuid;
};

// Exactly one target: view_id, or group_id (with choice_id for select/on).
// "select"/"on" selects a choice or view. "next" selects the next choice of
// modes/one. "toggle"/"none" uses a modes group's existing previous/allow-none
// semantics. Together exposes the stable choices "on" and "off". Follow is
// a visibility rule, not a directly selectable choice list.
// Success means accepted. The real state changes on the controller tick and
// emits SceneControllerChanged only after all involved rule locks are released.
// show_scene is a panel-only opt-in: modes and combined views also display their
// explicit destination, or the owning scene when none is configured. Vendor and
// hotkey defaults remain false. Choose one, Show together and clearing a modes
// group do not add navigation.
bool scene_controller_command(const char *controller_id, const char *group_id, const char *choice_id,
			      const char *view_id, const char *command, std::string &why, bool show_scene = false,
			      const SceneControllerAliasTarget &alias = {});

// Frontend editing helpers. create adds one real scene filter and returns its
// snapshot. import copies the full legacy configuration; activate transfers
// execution while preserving the old filter as a compatibility alias. With
// activate false, the imported group stays disabled and originals keep running.
// A review copy can later set origin.activate true before enabling the group.
// Remove that group or controller to return ownership to the original filter.
// Import rejects unsupported kinds/settings before changing either side.
bool scene_controller_create(const char *scene_uuid, const char *name, obs_data_t *response, std::string &why);
bool scene_controller_import(const char *controller_id, const char *legacy_filter_uuid, bool activate,
			     obs_data_t *response, std::string &why);
// Import all compatible filters on the controller's own scene in one edit.
// Already imported filters, including active owners in another controller, are
// skipped. Any invalid prospective group rejects the entire batch. The response
// contains importedCount, skippedCount and groupIds [{value:stable_group_id}].
bool scene_controller_import_all(const char *controller_id, bool activate, obs_data_t *response, std::string &why);

} // namespace stagekit
