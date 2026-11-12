/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "arrange-tools.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

std::mutex g_mutex;
ArrangeOptions g_options;
std::atomic<uint64_t> g_revision{1};

const ArrangeInfo kInfo[(int)ArrangeAction::Count] = {
	{"left", "Arrange.Left", "Hotkey.Arrange.Left", 1},
	{"center", "Arrange.Center", "Hotkey.Arrange.Center", 1},
	{"right", "Arrange.Right", "Hotkey.Arrange.Right", 1},
	{"top", "Arrange.Top", "Hotkey.Arrange.Top", 1},
	{"middle", "Arrange.Middle", "Hotkey.Arrange.Middle", 1},
	{"bottom", "Arrange.Bottom", "Hotkey.Arrange.Bottom", 1},
	{"space_across", "Arrange.SpaceAcross", "Hotkey.Arrange.SpaceAcross", 3},
	{"space_down", "Arrange.SpaceDown", "Hotkey.Arrange.SpaceDown", 3},
	{"snap_now", "Arrange.SnapNow", "Hotkey.Arrange.SnapNow", 1},
	{"same_width", "Arrange.SameWidth", "Hotkey.Arrange.SameWidth", 2},
	{"same_height", "Arrange.SameHeight", "Hotkey.Arrange.SameHeight", 2},
	{"same_size", "Arrange.SameSize", "Hotkey.Arrange.SameSize", 2},
};

struct Hotkey {
	ArrangeAction action;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	std::string name;
};
Hotkey g_hotkeys[(int)ArrangeAction::Count];

// The last step handed to OBS's undo list, kept so the tests can replay it.
std::string g_last_before, g_last_after;

void load_transforms(const char *state)
{
	if (state && *state)
		obs_scene_load_transform_states(state);
}

} // namespace

ArrangeOptions arrange_options()
{
	std::lock_guard lock(g_mutex);
	return g_options;
}

void set_arrange_options(const ArrangeOptions &options)
{
	{
		std::lock_guard lock(g_mutex);
		g_options = options;
		g_options.within = std::clamp(g_options.within, 1, 200);
	}
	g_revision++;
}

uint64_t arrange_options_revision()
{
	return g_revision.load();
}

const ArrangeInfo &arrange_info(ArrangeAction action)
{
	return kInfo[std::clamp((int)action, 0, (int)ArrangeAction::Count - 1)];
}

std::string transform_snapshot(obs_source_t *scene)
{
	obs_scene_t *s = scene_of(scene);
	if (!s)
		return {};
	obs_data_t *states = obs_scene_save_transform_states(s, true);
	const char *json = states ? obs_data_get_json(states) : nullptr;
	std::string out = json ? json : "";
	obs_data_release(states);
	return out;
}

void add_transform_undo(const char *name, const std::string &before, const std::string &after)
{
	if (before.empty() || after.empty() || before == after)
		return;
	g_last_before = before;
	g_last_after = after;
	// Without OBS's window (the tests) there is no undo list to add to.
	if (obs_frontend_get_main_window())
		obs_frontend_add_undo_redo_action(name, load_transforms, load_transforms, before.c_str(), after.c_str(),
						  false);
}

void replay_last_arrange(bool redo)
{
	load_transforms((redo ? g_last_after : g_last_before).c_str());
}

ArrangeOutcome run_arrange(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items)
{
	// One step in OBS's Edit → Undo for whatever this changes.
	const std::string before = transform_snapshot(scene);
	const ArrangeOutcome out = run_arrange_now(action, scene, items);
	if (out.changed > 0)
		add_transform_undo(obs_module_text(arrange_info(action).hotkey), before, transform_snapshot(scene));
	return out;
}

ArrangeOutcome run_arrange_now(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items)
{
	const ArrangeOptions o = arrange_options();
	ArrangeOutcome out;
	out.had = items.size();
	out.needs = arrange_info(action).needs;
	// Lining up with each other takes at least two.
	if (action <= ArrangeAction::Bottom && o.against == Against::Selection)
		out.needs = 2;
	if (!scene || out.had < out.needs)
		return out;
	switch (action) {
	case ArrangeAction::Left:
	case ArrangeAction::Center:
	case ArrangeAction::Right:
	case ArrangeAction::Top:
	case ArrangeAction::Middle:
	case ArrangeAction::Bottom:
		out.changed = align_items(scene, items, static_cast<Edge>((int)action), o.against, o.how);
		break;
	case ArrangeAction::SpaceAcross:
	case ArrangeAction::SpaceDown:
		out.changed = distribute_items(scene, items, action == ArrangeAction::SpaceAcross, o.how);
		break;
	case ArrangeAction::SnapNow:
		for (obs_sceneitem_t *item : items)
			out.changed += snap_item(scene, item, o.within, o.how) ? 1 : 0;
		break;
	case ArrangeAction::SameWidth:
		out.changed = match_size(scene, items, true, false, o.keep_shape, o.how);
		break;
	case ArrangeAction::SameHeight:
		out.changed = match_size(scene, items, false, true, o.keep_shape, o.how);
		break;
	case ArrangeAction::SameSize:
		out.changed = match_size(scene, items, true, true, o.keep_shape, o.how);
		break;
	default:
		break;
	}
	return out;
}

obs_source_t *editing_scene()
{
	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									 : obs_frontend_get_current_scene();
	// Without OBS's window (the tests), the scene being output stands in.
	return scene ? scene : scene_on_screen();
}

ArrangeOutcome run_arrange_on_selection(ArrangeAction action)
{
	obs_source_t *scene = editing_scene();
	const ArrangeOutcome out = run_arrange(action, scene, selected_items(scene));
	obs_source_release(scene);
	return out;
}

void register_arrange_hotkeys()
{
	for (int i = 0; i < (int)ArrangeAction::Count; i++) {
		Hotkey &hk = g_hotkeys[i];
		hk.action = static_cast<ArrangeAction>(i);
		hk.name = std::string("stagekit.arrange.") + kInfo[i].name;
		hk.id = obs_hotkey_register_frontend(
			hk.name.c_str(), obs_module_text(kInfo[i].hotkey),
			[](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
				if (!pressed)
					return;
				// Scene items are changed on the UI thread, as OBS itself does.
				obs_queue_task(
					OBS_TASK_UI,
					[](void *param) {
						run_arrange_on_selection(static_cast<Hotkey *>(param)->action);
					},
					data, false);
			},
			&hk);
	}
}

void arrange_save_load(obs_data_t *save_data, bool saving)
{
	if (saving) {
		const ArrangeOptions o = arrange_options();
		obs_data_set_int(save_data, "stagekit.arrange.against", (int)o.against);
		obs_data_set_bool(save_data, "stagekit.arrange.visible", o.how.visible_pixels);
		obs_data_set_bool(save_data, "stagekit.arrange.no_shadow", o.how.leave_out_shadows);
		obs_data_set_bool(save_data, "stagekit.arrange.keep_shape", o.keep_shape);
		obs_data_set_bool(save_data, "stagekit.arrange.live_snap", o.live_snap);
		obs_data_set_int(save_data, "stagekit.arrange.within", o.within);
	} else if (obs_data_has_user_value(save_data, "stagekit.arrange.within")) {
		ArrangeOptions o;
		o.against = obs_data_get_int(save_data, "stagekit.arrange.against") == 1 ? Against::Selection
											 : Against::Screen;
		o.how.visible_pixels = obs_data_get_bool(save_data, "stagekit.arrange.visible");
		o.how.leave_out_shadows = obs_data_get_bool(save_data, "stagekit.arrange.no_shadow");
		o.keep_shape = obs_data_get_bool(save_data, "stagekit.arrange.keep_shape");
		o.live_snap = obs_data_get_bool(save_data, "stagekit.arrange.live_snap");
		o.within = (int)obs_data_get_int(save_data, "stagekit.arrange.within");
		set_arrange_options(o);
	}
	for (Hotkey &hk : g_hotkeys) {
		if (hk.id == OBS_INVALID_HOTKEY_ID)
			continue;
		if (saving) {
			obs_data_array_t *bindings = obs_hotkey_save(hk.id);
			obs_data_set_array(save_data, hk.name.c_str(), bindings);
			obs_data_array_release(bindings);
		} else {
			obs_data_array_t *bindings = obs_data_get_array(save_data, hk.name.c_str());
			obs_hotkey_load(hk.id, bindings);
			obs_data_array_release(bindings);
		}
	}
}

} // namespace stagekit
