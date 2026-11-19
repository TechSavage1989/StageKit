/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "arrange-tools.hpp"

#include "../api/vendor-api.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../placement/fit-item.hpp"
#include "../placement/scene-fit.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/matrix4.h>

#include <map>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>

namespace stagekit {

namespace {

std::mutex g_mutex;
ArrangeOptions g_options;
std::atomic<uint64_t> g_revision{1};
ArrangeGuide g_guide;

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
	{"same_everywhere", "Arrange.SameEverywhere", "Hotkey.Arrange.SameEverywhere", 1},
	{"same_chosen", "Arrange.SameChosen", "Hotkey.Arrange.SameChosen", 1},
	{"fit_space", "Arrange.FitSpace", "Hotkey.Arrange.FitSpace", 1},
};

struct Hotkey {
	ArrangeAction action;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	std::string name;
};
Hotkey g_hotkeys[(int)ArrangeAction::Count];
obs_hotkey_id g_auto_fit_hotkey = OBS_INVALID_HOTKEY_ID;
const char *const kAutoFitHotkey = "stagekit.arrange.auto_fit";

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
	vendor_emit_options_changed();
}

namespace {
std::function<void(int)> g_slide; // only touched on the window's thread
ArrangeGlide g_glide;             // the same
const void *g_glide_owner = nullptr;
std::atomic<bool> g_can_slide{false};
} // namespace

void set_slide_handler(std::function<void(int)> handler)
{
	g_can_slide = (bool)handler;
	g_slide = std::move(handler);
}

bool request_slide(int direction)
{
	if (!g_can_slide || direction < 0 || direction > 8)
		return false;
	obs_queue_task(
		OBS_TASK_UI,
		[](void *param) {
			if (g_slide)
				g_slide((int)(intptr_t)param);
		},
		(void *)(intptr_t)direction, false);
	return true;
}

void set_arrange_glide(ArrangeGlide handler, const void *owner)
{
	if (!handler && owner && owner != g_glide_owner)
		return; // another dock's, set since
	g_glide = std::move(handler);
	g_glide_owner = g_glide ? owner : nullptr;
}

ArrangeGuide last_arrange_guide()
{
	std::lock_guard lock(g_mutex);
	return g_guide;
}

uint64_t arrange_options_revision()
{
	return g_revision.load();
}

const ArrangeInfo &arrange_info(ArrangeAction action)
{
	return kInfo[std::clamp((int)action, 0, (int)ArrangeAction::Count - 1)];
}

namespace {

// When each selected item was selected, from OBS's own "item_select"
// signals. Kept as bare addresses, only ever compared with live items.
std::mutex g_select_mutex;
std::map<obs_sceneitem_t *, uint64_t> g_selected_at;
uint64_t g_selections = 0;

void on_item_select(void *, calldata_t *cd)
{
	auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	std::lock_guard lock(g_select_mutex);
	g_selected_at[item] = ++g_selections;
}

void on_item_deselect(void *, calldata_t *cd)
{
	auto *item = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));
	std::lock_guard lock(g_select_mutex);
	g_selected_at.erase(item);
}

void watch_selection_in(obs_source_t *source)
{
	if (!source || (!obs_source_is_scene(source) && !obs_source_is_group(source)))
		return;
	signal_handler_t *sh = obs_source_get_signal_handler(source);
	// Never twice for the same scene.
	signal_handler_disconnect(sh, "item_select", on_item_select, nullptr);
	signal_handler_disconnect(sh, "item_deselect", on_item_deselect, nullptr);
	signal_handler_connect(sh, "item_select", on_item_select, nullptr);
	signal_handler_connect(sh, "item_deselect", on_item_deselect, nullptr);
}

} // namespace

void track_selection_order()
{
	signal_handler_connect(
		obs_get_signal_handler(), "source_create",
		[](void *, calldata_t *cd) {
			watch_selection_in(static_cast<obs_source_t *>(calldata_ptr(cd, "source")));
		},
		nullptr);
	obs_enum_scenes(
		[](void *, obs_source_t *scene) {
			watch_selection_in(scene);
			return true;
		},
		nullptr);
}

std::vector<obs_sceneitem_t *> selection_in_order(obs_source_t *scene)
{
	std::vector<obs_sceneitem_t *> items = selected_items(scene);
	// Items selected before this was watching come first, in list order.
	std::lock_guard lock(g_select_mutex);
	auto when = [](obs_sceneitem_t *item) {
		const auto found = g_selected_at.find(item);
		return found == g_selected_at.end() ? 0 : found->second;
	};
	std::stable_sort(items.begin(), items.end(),
			 [&](obs_sceneitem_t *a, obs_sceneitem_t *b) { return when(a) < when(b); });
	return items;
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

std::string transform_snapshot_all()
{
	// One list holding every scene's entries: OBS loads such a list scene by scene.
	obs_data_array_t *merged = obs_data_array_create();
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			obs_scene_t *s = scene_of(scene);
			obs_data_t *states = s ? obs_scene_save_transform_states(s, true) : nullptr;
			obs_data_array_t *part = states ? obs_data_get_array(states, "scenes_and_groups") : nullptr;
			for (size_t i = 0; i < obs_data_array_count(part); i++) {
				obs_data_t *entry = obs_data_array_item(part, i);
				obs_data_array_push_back(static_cast<obs_data_array_t *>(param), entry);
				obs_data_release(entry);
			}
			obs_data_array_release(part);
			obs_data_release(states);
			return true;
		},
		merged);
	obs_data_t *all = obs_data_create();
	obs_data_set_array(all, "scenes_and_groups", merged);
	obs_data_array_release(merged);
	const char *json = obs_data_get_json(all);
	std::string out = json ? json : "";
	obs_data_release(all);
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
	// One step in OBS's Edit → Undo for whatever this changes, in every
	// scene when that is where it reaches.
	const bool copies = action == ArrangeAction::SameEverywhere || action == ArrangeAction::SameChosen;
	const bool everywhere = copies || arrange_options().arrange_every_scene;
	const std::string before = everywhere ? transform_snapshot_all() : transform_snapshot(scene);
	// Where each item was, for a tool whose change glides.
	const bool glides = action == ArrangeAction::FitSpace && g_glide;
	std::vector<ArrangeStart> starts;
	// (Not an item with a shadow sized in screen pixels: its filter moves the
	// item as its room changes with the size, by the size at that moment,
	// and a glide in between leaves the picture off its place.)
	const auto fixed_shadow = [](obs_sceneitem_t *item) {
		ShadowFree free;
		if (!shadow_free(obs_sceneitem_get_source(item), free))
			return false;
		obs_source_release(free.draw);
		return free.fixed_on_screen;
	};
	if (glides)
		for (obs_sceneitem_t *item : items) {
			if (fixed_shadow(item))
				continue;
			ArrangeStart start{item, {}, {}, {}, false};
			obs_sceneitem_get_pos(item, &start.pos);
			obs_sceneitem_get_scale(item, &start.scale);
			obs_sceneitem_get_bounds(item, &start.bounds);
			starts.push_back(start);
		}
	ArrangeOutcome out = run_arrange_now(action, scene, items);
	// "In every scene": what was lined up here sits the same way wherever
	// else its source is shown. Fit to space never makes another fit give
	// up room there either: a scene where the copy would is left out (not
	// shrunk there: the copies stay the same as the item here), and so is
	// one whose fits have not worked their spaces out yet (its scene not
	// shown since OBS started), where that cannot be known.
	CopyAllowed keep_fits;
	if (action == ArrangeAction::FitSpace)
		keep_fits = [&out](obs_source_t *in, obs_sceneitem_t *copy, const Box &from, const Box &to) {
			std::string who;
			const FitCheck check = copy_keeps_fits(in, copy, from, to, &who);
			if (check == FitCheck::Kept)
				return true;
			const char *name = obs_source_get_name(in);
			const std::string scene_name = name ? name : "";
			auto &list = check == FitCheck::Unknown ? out.not_known : out.not_copied;
			const std::string entry = check == FitCheck::Unknown ? scene_name : scene_name + ": " + who;
			if (std::find(list.begin(), list.end(), entry) == list.end())
				list.push_back(entry);
			return false;
		};
	if (!copies && everywhere && out.changed > 0)
		copy_to_every_scene(scene, action == ArrangeAction::FitSpace ? out.placed : items, nullptr, nullptr,
				    false, keep_fits);
	if (out.changed > 0)
		add_transform_undo(obs_module_text(arrange_info(action).hotkey), before,
				   everywhere ? transform_snapshot_all() : transform_snapshot(scene));
	// An item placed on purpose is handed to its Fit to Space where it ends
	// up: at once, or by the glide when it gets there.
	const auto placed = [&](obs_sceneitem_t *item) {
		return std::find(out.placed.begin(), out.placed.end(), item) != out.placed.end();
	};
	const auto gliding = [&](obs_sceneitem_t *item) {
		return glides && std::any_of(starts.begin(), starts.end(),
					     [item](const ArrangeStart &start) { return start.item == item; });
	};
	for (obs_sceneitem_t *item : out.placed)
		if (!gliding(item))
			fit_item_placed(scene, obs_source_get_name(obs_sceneitem_get_source(item)));
	if (glides && out.changed > 0 && !starts.empty()) {
		for (ArrangeStart &start : starts)
			start.hand_over = placed(start.item);
		g_glide(scene, starts);
	}
	return out;
}

ArrangeOutcome run_arrange_now(ArrangeAction action, obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items)
{
	const ArrangeOptions o = arrange_options();
	ArrangeOutcome out;
	out.had = items.size();
	out.needs = arrange_info(action).needs;
	// Lining up with each other takes at least two.
	const bool to_key = o.against == Against::First || o.against == Against::Last;
	if (action <= ArrangeAction::Bottom && (o.against == Against::Selection || to_key))
		out.needs = 2;
	if (!scene || out.had < out.needs)
		return out;
	switch (action) {
	case ArrangeAction::Left:
	case ArrangeAction::Center:
	case ArrangeAction::Right:
	case ArrangeAction::Top:
	case ArrangeAction::Middle:
	case ArrangeAction::Bottom: {
		double line = 0.0;
		out.changed = align_items(scene, items, static_cast<Edge>((int)action), o.against, o.how, &line);
		std::lock_guard lock(g_mutex);
		g_guide.serial++;
		g_guide.vertical = action <= ArrangeAction::Right;
		g_guide.at = line;
	} break;
	case ArrangeAction::SpaceAcross:
	case ArrangeAction::SpaceDown:
		out.changed = distribute_items(scene, items, action == ArrangeAction::SpaceAcross, o.how);
		break;
	case ArrangeAction::SnapNow:
		for (obs_sceneitem_t *item : items)
			out.changed += snap_item(scene, item, o.within, o.how) ? 1 : 0;
		break;
	case ArrangeAction::SameWidth:
	case ArrangeAction::SameHeight:
	case ArrangeAction::SameSize: {
		// The others take the size of one item, which goes last in the list:
		// the first or last selected when lining up to one of those, otherwise
		// the top-most in the Sources list.
		std::vector<obs_sceneitem_t *> sized = items;
		if (o.against == Against::First) {
			std::rotate(sized.begin(), sized.begin() + 1, sized.end());
		} else if (o.against != Against::Last) {
			const std::vector<obs_sceneitem_t *> stacked = selected_items(scene);
			if (stacked.size() == sized.size())
				sized = stacked;
		}
		out.changed = match_size(scene, sized, action != ArrangeAction::SameHeight,
					 action != ArrangeAction::SameWidth, o.keep_shape, o.how);
		break;
	}
	case ArrangeAction::SameEverywhere:
		out.changed = copy_to_every_scene(scene, items, nullptr, &o.copy_sources, o.copy_locked);
		break;
	case ArrangeAction::SameChosen:
		out.changed = copy_to_every_scene(scene, items, &o.copy_scenes, &o.copy_sources, o.copy_locked);
		break;
	case ArrangeAction::FitSpace:
		out.changed = fit_to_space(scene, items, o.place_margin, o.how, &out.placed, &out.fit, o.place_toward);
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
	const ArrangeOutcome out = run_arrange(action, scene, selection_in_order(scene));
	obs_source_release(scene);
	return out;
}

int bounds_type_of(const std::vector<obs_sceneitem_t *> &items)
{
	int type = -1;
	for (size_t i = 0; i < items.size(); i++) {
		const int t = (int)obs_sceneitem_get_bounds_type(items[i]);
		if (i == 0)
			type = t;
		else if (t != type)
			return -1;
	}
	return type;
}

namespace {

// Where the item's picture starts (its source's top-left corner) in its
// parent's coordinates.
vec3 picture_origin(obs_sceneitem_t *item)
{
	obs_sceneitem_force_update_transform(item);
	matrix4 draw;
	obs_sceneitem_get_draw_transform(item, &draw);
	vec3 at;
	vec3_set(&at, 0.0f, 0.0f, 0.0f);
	vec3_transform(&at, &at, &draw);
	return at;
}

// The scale OBS gives a picture of w x h in bounds of bw x bh.
vec2 scale_in_bounds(obs_bounds_type type, float w, float h, float bw, float bh)
{
	vec2 k;
	const float kx = bw / w, ky = bh / h;
	switch (type) {
	case OBS_BOUNDS_STRETCH:
		vec2_set(&k, kx, ky);
		break;
	case OBS_BOUNDS_SCALE_OUTER:
		vec2_set(&k, std::max(kx, ky), std::max(kx, ky));
		break;
	case OBS_BOUNDS_SCALE_TO_WIDTH:
		vec2_set(&k, kx, kx);
		break;
	case OBS_BOUNDS_SCALE_TO_HEIGHT:
		vec2_set(&k, ky, ky);
		break;
	case OBS_BOUNDS_MAX_ONLY: {
		const float m = (w > bw || h > bh) ? std::min(kx, ky) : 1.0f;
		vec2_set(&k, m, m);
		break;
	}
	default:
		vec2_set(&k, std::min(kx, ky), std::min(kx, ky));
		break;
	}
	return k;
}

void fit_filters_of(obs_source_t *scene, std::vector<obs_source_t *> &out)
{
	if (!scene)
		return;
	obs_source_enum_filters(
		scene,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			const char *id = obs_source_get_unversioned_id(filter);
			if (id && (std::strcmp(id, kFitItemId) == 0 || std::strcmp(id, kSceneFitId) == 0))
				static_cast<std::vector<obs_source_t *> *>(param)->push_back(filter);
		},
		&out);
}

bool is_scene_fit(obs_source_t *filter)
{
	const char *id = obs_source_get_unversioned_id(filter);
	return id && std::strcmp(id, kSceneFitId) == 0;
}

std::string fit_item_of(obs_source_t *filter)
{
	obs_data_t *settings = obs_source_get_settings(filter);
	std::string item = obs_data_get_string(settings, "item");
	obs_data_release(settings);
	return item;
}

} // namespace

std::string fitted_by(obs_source_t *scene, const std::string &source)
{
	std::vector<obs_source_t *> filters;
	fit_filters_of(scene, filters);
	for (obs_source_t *filter : filters) {
		if (!obs_source_enabled(filter))
			continue;
		if (is_scene_fit(filter) ? scene_fit_covers(filter, source) && scene_fit_entry_on(filter, source)
					 : fit_item_of(filter) == source)
			return obs_source_get_name(filter);
	}
	return {};
}

BoundsOutcome set_bounds_type(obs_source_t *scene, const std::vector<obs_sceneitem_t *> &items, obs_bounds_type type)
{
	BoundsOutcome out;
	const std::string before = transform_snapshot(scene);
	for (obs_sceneitem_t *item : items) {
		const obs_bounds_type was = obs_sceneitem_get_bounds_type(item);
		if (was == type)
			continue;
		refresh_item(item);
		obs_source_t *source = obs_sceneitem_get_source(item);
		obs_sceneitem_crop crop;
		obs_sceneitem_get_crop(item, &crop);
		const float w = std::max(1.0f, (float)obs_source_get_width(source) - crop.left - crop.right);
		const float h = std::max(1.0f, (float)obs_source_get_height(source) - crop.top - crop.bottom);
		vec2 scale, bounds;
		obs_sceneitem_get_scale(item, &scale);
		obs_sceneitem_get_bounds(item, &bounds);
		const vec3 from = picture_origin(item);
		obs_sceneitem_defer_update_begin(item);
		if (type == OBS_BOUNDS_NONE) {
			// The scale OBS used inside the bounds, so the size on screen stays.
			const vec2 k = scale_in_bounds(was, w, h, std::fabs(bounds.x), std::fabs(bounds.y));
			vec2 now;
			vec2_set(&now, std::copysign(k.x, scale.x), std::copysign(k.y, scale.y));
			obs_sceneitem_set_scale(item, &now);
		} else if (was == OBS_BOUNDS_NONE) {
			vec2 size;
			vec2_set(&size, w * std::fabs(scale.x), h * std::fabs(scale.y));
			obs_sceneitem_set_bounds(item, &size);
		}
		obs_sceneitem_set_bounds_type(item, type);
		obs_sceneitem_defer_update_end(item);
		// The picture where it was: the position moves by however far its
		// corner did (a picture smaller than its bounds sits inside them).
		const vec3 to = picture_origin(item);
		if (std::fabs(to.x - from.x) > 0.01f || std::fabs(to.y - from.y) > 0.01f) {
			vec2 pos;
			obs_sceneitem_get_pos(item, &pos);
			pos.x += from.x - to.x;
			pos.y += from.y - to.y;
			obs_sceneitem_set_pos(item, &pos);
		}
		out.changed++;
		const char *name = obs_source_get_name(source);
		const std::string by = name ? fitted_by(scene, name) : std::string();
		if (!by.empty())
			out.fitted.push_back(std::string(name) + "\t" + by);
	}
	if (out.changed)
		add_transform_undo(obs_module_text("Dock.Bounds"), before, transform_snapshot(scene));
	return out;
}

std::vector<std::string> sources_of(const std::vector<obs_sceneitem_t *> &items)
{
	std::vector<std::string> out;
	for (obs_sceneitem_t *item : items) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
		if (name && *name && std::find(out.begin(), out.end(), name) == out.end())
			out.push_back(name);
	}
	return out;
}

namespace {

// Calls `each(filter, source)` for every switch the Auto-fit control has:
// a whole filter (source empty), or a source's Scene Fit entry.
template<typename F> void each_auto_fit(obs_source_t *scene, const std::vector<std::string> *sources, F each)
{
	std::vector<obs_source_t *> filters;
	fit_filters_of(scene, filters);
	for (obs_source_t *filter : filters) {
		if (!sources) {
			each(filter, std::string());
			continue;
		}
		for (const std::string &source : *sources) {
			if (is_scene_fit(filter)) {
				if (scene_fit_covers(filter, source))
					each(filter, source);
			} else if (fit_item_of(filter) == source) {
				each(filter, std::string());
			}
		}
	}
}

} // namespace

int auto_fit_state(obs_source_t *scene, const std::vector<std::string> *sources)
{
	int state = -1;
	each_auto_fit(scene, sources, [&](obs_source_t *filter, const std::string &source) {
		const bool on = source.empty() ? obs_source_enabled(filter) : scene_fit_entry_on(filter, source);
		state = std::max(state, on ? 1 : 0);
	});
	return state;
}

int set_auto_fit(obs_source_t *scene, bool on, const std::vector<std::string> *sources)
{
	int changed = 0;
	each_auto_fit(scene, sources, [&](obs_source_t *filter, const std::string &source) {
		if (source.empty()) {
			if (obs_source_enabled(filter) != on) {
				obs_source_set_enabled(filter, on);
				changed++;
			}
		} else if (scene_fit_entry_on(filter, source) != on) {
			scene_fit_set_entry_on(filter, source, on);
			changed++;
		}
	});
	return changed;
}

void toggle_auto_fit_on_selection()
{
	obs_source_t *scene = editing_scene();
	if (!scene)
		return;
	const std::vector<std::string> picked = sources_of(selection_in_order(scene));
	const std::vector<std::string> *sources = arrange_options().auto_fit_sources ? &picked : nullptr;
	const int state = auto_fit_state(scene, sources);
	if (state >= 0)
		set_auto_fit(scene, state == 0, sources);
	obs_source_release(scene);
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
	g_auto_fit_hotkey = obs_hotkey_register_frontend(
		kAutoFitHotkey, obs_module_text("Hotkey.Arrange.AutoFit"),
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
			if (pressed)
				obs_queue_task(
					OBS_TASK_UI, [](void *) { toggle_auto_fit_on_selection(); }, nullptr, false);
		},
		nullptr);
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
		obs_data_set_bool(save_data, "stagekit.arrange.guides", o.guides);
		obs_data_set_bool(save_data, "stagekit.send.every_scene", o.send_every_scene);
		obs_data_set_bool(save_data, "stagekit.arrange.every_scene", o.arrange_every_scene);
		obs_data_set_bool(save_data, "stagekit.copy.locked", o.copy_locked);
		obs_data_set_bool(save_data, "stagekit.send.slide", o.send_slide);
		obs_data_set_int(save_data, "stagekit.send.curve", o.send_curve);
		obs_data_set_int(save_data, "stagekit.send.mode", o.send_mode);
		obs_data_set_bool(save_data, "stagekit.arrange.limit_resize", o.limit_resize);
		obs_data_set_int(save_data, "stagekit.arrange.limit_px", o.limit_px);
		obs_data_array_t *copy_scenes = obs_data_array_create();
		for (const std::string &name : o.copy_scenes) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "name", name.c_str());
			obs_data_array_push_back(copy_scenes, entry);
			obs_data_release(entry);
		}
		obs_data_set_array(save_data, "stagekit.copy.scenes", copy_scenes);
		obs_data_array_release(copy_scenes);
		obs_data_array_t *copy_sources = obs_data_array_create();
		for (const std::string &name : o.copy_sources) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "name", name.c_str());
			obs_data_array_push_back(copy_sources, entry);
			obs_data_release(entry);
		}
		obs_data_set_array(save_data, "stagekit.copy.sources", copy_sources);
		obs_data_array_release(copy_sources);
		obs_data_set_int(save_data, "stagekit.place.margin", o.place_margin);
		obs_data_set_int(save_data, "stagekit.place.toward", o.place_toward);
		obs_data_set_int(save_data, "stagekit.arrange.within", o.within);
		obs_data_set_bool(save_data, "stagekit.arrange.auto_fit_sources", o.auto_fit_sources);
	} else if (obs_data_has_user_value(save_data, "stagekit.arrange.within")) {
		ArrangeOptions o;
		o.against = static_cast<Against>(
			std::clamp((int)obs_data_get_int(save_data, "stagekit.arrange.against"), 0, 3));
		o.how.visible_pixels = obs_data_get_bool(save_data, "stagekit.arrange.visible");
		o.how.leave_out_shadows = obs_data_get_bool(save_data, "stagekit.arrange.no_shadow");
		o.keep_shape = obs_data_get_bool(save_data, "stagekit.arrange.keep_shape");
		o.live_snap = obs_data_get_bool(save_data, "stagekit.arrange.live_snap");
		o.guides = obs_data_get_bool(save_data, "stagekit.arrange.guides");
		o.within = (int)obs_data_get_int(save_data, "stagekit.arrange.within");
		// Collections saved before this option existed take its default.
		o.arrange_every_scene = obs_data_get_bool(save_data, "stagekit.arrange.every_scene");
		o.copy_locked = obs_data_get_bool(save_data, "stagekit.copy.locked");
		o.auto_fit_sources = obs_data_get_bool(save_data, "stagekit.arrange.auto_fit_sources");
		if (obs_data_has_user_value(save_data, "stagekit.send.every_scene"))
			o.send_every_scene = obs_data_get_bool(save_data, "stagekit.send.every_scene");
		o.send_slide = obs_data_get_bool(save_data, "stagekit.send.slide");
		if (obs_data_has_user_value(save_data, "stagekit.send.curve"))
			o.send_curve = std::clamp((int)obs_data_get_int(save_data, "stagekit.send.curve"), 0, 10);
		o.send_mode = std::clamp((int)obs_data_get_int(save_data, "stagekit.send.mode"), 0, 2);
		o.limit_resize = obs_data_get_bool(save_data, "stagekit.arrange.limit_resize");
		if (obs_data_has_user_value(save_data, "stagekit.place.margin"))
			o.place_margin = std::clamp((int)obs_data_get_int(save_data, "stagekit.place.margin"), 0, 400);
		o.place_toward = std::clamp((int)obs_data_get_int(save_data, "stagekit.place.toward"), 0, 8);
		if (obs_data_has_user_value(save_data, "stagekit.arrange.limit_px"))
			o.limit_px = (int)obs_data_get_int(save_data, "stagekit.arrange.limit_px");
		if (obs_data_array_t *copy_scenes = obs_data_get_array(save_data, "stagekit.copy.scenes")) {
			for (size_t i = 0; i < obs_data_array_count(copy_scenes); i++) {
				obs_data_t *entry = obs_data_array_item(copy_scenes, i);
				const char *name = obs_data_get_string(entry, "name");
				if (name && *name)
					o.copy_scenes.push_back(name);
				obs_data_release(entry);
			}
			obs_data_array_release(copy_scenes);
		}
		if (obs_data_array_t *copy_sources = obs_data_get_array(save_data, "stagekit.copy.sources")) {
			for (size_t i = 0; i < obs_data_array_count(copy_sources); i++) {
				obs_data_t *entry = obs_data_array_item(copy_sources, i);
				const char *name = obs_data_get_string(entry, "name");
				if (name && *name)
					o.copy_sources.push_back(name);
				obs_data_release(entry);
			}
			obs_data_array_release(copy_sources);
		}
		set_arrange_options(o);
	}
	if (g_auto_fit_hotkey != OBS_INVALID_HOTKEY_ID) {
		if (saving) {
			obs_data_array_t *bindings = obs_hotkey_save(g_auto_fit_hotkey);
			obs_data_set_array(save_data, kAutoFitHotkey, bindings);
			obs_data_array_release(bindings);
		} else {
			obs_data_array_t *bindings = obs_data_get_array(save_data, kAutoFitHotkey);
			obs_hotkey_load(g_auto_fit_hotkey, bindings);
			obs_data_array_release(bindings);
		}
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
