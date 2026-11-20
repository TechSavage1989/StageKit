/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "outline-ambient.hpp"

#include "../ambient/ambient-background.hpp"
#include "../ambient/ambient-frame.hpp"
#include "../common/loading.hpp"
#include "../common/scene-filter-ids.hpp"
#include "../common/scene-util.hpp"
#include "../common/tooltip.hpp"
#include "../shadow/shadow-backdrop.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace stagekit {

namespace {

constexpr const char *kFile = "effects/stagekit-outline-ambient.effect";
constexpr uint64_t kKeepNs = 2000000000ULL;
constexpr uint64_t kLookNs = 250000000ULL; // a choice is looked up again this often at most
constexpr size_t kMost = 16;
constexpr size_t kMostKept = 64;

// ---------------------------------------------------------------- choices

std::mutex g_mutex; // the names, the answers kept, the tests' file name
std::vector<std::string> g_names;

// A choice as it is asked for from a drawing, and its answer. The answer is
// only ever worked out in a tick (`look_tick`), never while drawing: OBS
// draws the main view holding the lock that asking for the output's source
// takes, and a scene's items are not to be gone through from inside another
// scene's drawing.
struct Kept {
	uint32_t choice = 0;
	const obs_source_t *scene = nullptr; // compared only
	obs_weak_source_t *weak = nullptr;   // the scene, held weakly (none: no scene of its own)
	uint32_t id = 0;
	uint64_t frame_rev = 0, when = 0, asked = 0;
	bool looked = false;
};
std::vector<Kept> g_kept;
constexpr uint64_t kForgetNs = 5000000000ULL; // a choice not asked for this long is let go of

bool is_ambient(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	return id && strcmp(id, kAmbientBackgroundId) == 0;
}

// The first Ambient Background in a scene, from the bottom, groups looked
// into.
uint32_t first_in(obs_source_t *scene_source)
{
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene)
		return 0;
	uint32_t found = 0;
	static const auto look = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *out = static_cast<uint32_t *>(param);
		obs_source_t *source = obs_sceneitem_get_source(item);
		if (is_ambient(source))
			*out = ambient_frame_of(source);
		else if (obs_sceneitem_is_group(item))
			obs_sceneitem_group_enum_items(
				item,
				[](obs_scene_t *, obs_sceneitem_t *inner, void *p) {
					auto *o = static_cast<uint32_t *>(p);
					obs_source_t *s = obs_sceneitem_get_source(inner);
					if (is_ambient(s))
						*o = ambient_frame_of(s);
					return *o == 0;
				},
				param);
		return *out == 0;
	};
	obs_scene_enum_items(scene, look, &found);
	return found;
}

uint32_t find_now(const std::string &name, obs_source_t *scene)
{
	if (!name.empty()) {
		obs_source_t *named = obs_get_source_by_name(name.c_str());
		const uint32_t id = named ? ambient_frame_of(named) : 0;
		obs_source_release(named);
		if (id)
			return id;
	}
	// (While OBS loads a collection only some scenes have their items.)
	if (!scenes_loaded())
		return ambient_frame_first();
	uint32_t id = first_in(scene);
	if (!id) {
		obs_source_t *on_screen = scene_on_screen();
		if (on_screen && on_screen != scene)
			id = first_in(on_screen);
		obs_source_release(on_screen);
	}
	return id ? id : ambient_frame_first();
}

// ---------------------------------------------------------------- pictures

struct Key {
	uint32_t id = 0;
	uint64_t frame = 0; // the background's own: the frame it drew that picture in
	gs_texture_t *from = nullptr;
	uint32_t w = 0, h = 0;
	float m[4] = {1, 0, 0, 1}, shift[2] = {0, 0};
	float bright = 1.0f, saturation = 1.0f;
	bool operator==(const Key &o) const
	{
		return id == o.id && frame == o.frame && from == o.from && w == o.w && h == o.h &&
		       memcmp(m, o.m, sizeof(m)) == 0 && memcmp(shift, o.shift, sizeof(shift)) == 0 &&
		       bright == o.bright && saturation == o.saturation;
	}
};

struct Entry {
	Key key;
	gs_texrender_t *render = nullptr;
	uint64_t frame = 0; // the video frame it was last asked for in
	uint64_t used = 0;  // by the clock
	bool drawn = false;
};

// (All of it only in a graphics context, which one thread has at a time.)
std::vector<Entry> g_entries;
struct Shader {
	gs_effect_t *effect = nullptr;
	bool tried = false;
	gs_eparam_t *image = nullptr, *frame = nullptr, *frame_shift = nullptr;
	gs_eparam_t *bright = nullptr, *saturation = nullptr;
};
Shader g_shader;
std::string g_test_file;
bool g_reload = false;
std::atomic<long long> g_held{0}, g_draws{0};
std::atomic<uint64_t> g_last_use{0};
std::atomic<bool> g_watching{false};
signal_handler_t *g_signals = nullptr;
bool g_looking = false;

// The choices asked for are looked up afresh when a background has come or
// gone, and four times a second (the scene on screen changing sends no
// signal); those not asked for in a while are let go of.
void look_tick(void *, float)
{
	struct Ask {
		uint32_t choice;
		const obs_source_t *scene;
		obs_weak_source_t *weak;
		std::string name;
	};
	const uint64_t now = os_gettime_ns();
	const uint64_t frame_rev = ambient_frame_revision();
	std::vector<Ask> asks;
	std::vector<obs_weak_source_t *> gone;
	{
		std::lock_guard lock(g_mutex);
		for (size_t i = g_kept.size(); i-- > 0;) {
			Kept &k = g_kept[i];
			// (Asked can be later than now: another thread.)
			if (now > k.asked && now - k.asked > kForgetNs) {
				if (k.weak)
					gone.push_back(k.weak);
				g_kept.erase(g_kept.begin() + (ptrdiff_t)i);
				continue;
			}
			if (k.looked && k.frame_rev == frame_rev && (now < k.when || now - k.when < kLookNs))
				continue;
			Ask a{k.choice, k.scene, k.weak, {}};
			if (a.weak)
				obs_weak_source_addref(a.weak);
			if (k.choice && k.choice <= g_names.size())
				a.name = g_names[k.choice - 1];
			asks.push_back(std::move(a));
		}
	}
	for (obs_weak_source_t *w : gone)
		obs_weak_source_release(w);
	gone.clear();
	// (Nothing of ours is locked while OBS is asked.)
	for (Ask &a : asks) {
		obs_source_t *scene = a.weak ? obs_weak_source_get_source(a.weak) : nullptr;
		const bool scene_gone = a.weak && !scene;
		const uint32_t id = scene_gone ? 0 : find_now(a.name, scene);
		obs_source_release(scene);
		std::lock_guard lock(g_mutex);
		for (size_t i = 0; i < g_kept.size(); i++) {
			Kept &k = g_kept[i];
			if (k.choice != a.choice || k.scene != a.scene || k.weak != a.weak)
				continue;
			if (scene_gone) {
				// (Released below, outside the lock.)
				g_kept.erase(g_kept.begin() + (ptrdiff_t)i);
				gone.push_back(a.weak);
			} else {
				k.id = id;
				k.frame_rev = frame_rev;
				k.when = now;
				k.looked = true;
			}
			break;
		}
	}
	for (Ask &a : asks)
		if (a.weak)
			obs_weak_source_release(a.weak);
	for (obs_weak_source_t *w : gone)
		obs_weak_source_release(w);
}

void let_go_all()
{
	for (Entry &e : g_entries)
		gs_texrender_destroy(e.render);
	g_entries.clear();
	g_held = 0;
}

void let_go_shader()
{
	gs_effect_destroy(g_shader.effect);
	g_shader = Shader();
}

// Pictures nobody has asked for in a while are let go of.
void sweep_tick(void *, float)
{
	if (g_held.load() == 0)
		return;
	const uint64_t last = g_last_use.load(), now = os_gettime_ns();
	if (now < last || now - last < kKeepNs)
		return;
	obs_enter_graphics();
	let_go_all();
	obs_leave_graphics();
}

const Shader *load()
{
	std::string file;
	{
		std::lock_guard lock(g_mutex);
		if (g_reload) {
			g_reload = false;
			let_go_shader();
		}
		file = g_test_file.empty() ? kFile : g_test_file;
	}
	Shader &sh = g_shader;
	if (sh.tried)
		return sh.effect ? &sh : nullptr;
	sh.tried = true;
	char *path = obs_module_file(file.c_str());
	char *errors = nullptr;
	sh.effect = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	bfree(path);
	if (sh.effect) {
		sh.image = gs_effect_get_param_by_name(sh.effect, "image");
		sh.frame = gs_effect_get_param_by_name(sh.effect, "frame");
		sh.frame_shift = gs_effect_get_param_by_name(sh.effect, "frame_shift");
		sh.bright = gs_effect_get_param_by_name(sh.effect, "bright");
		sh.saturation = gs_effect_get_param_by_name(sh.effect, "saturation");
	}
	if (!sh.effect || !sh.image || !sh.frame || !sh.frame_shift || !sh.bright || !sh.saturation) {
		obs_log(LOG_ERROR,
			"The outline's Ambient Background fill is not available (failed to load '%s': %s); such an outline is drawn in its outline colour.",
			file.c_str(),
			errors      ? errors
			: sh.effect ? "a value is missing"
				    : "no such file");
		gs_effect_destroy(sh.effect);
		sh.effect = nullptr;
	}
	bfree(errors);
	return sh.effect ? &sh : nullptr;
}

void draw(Entry &e, const Shader &sh)
{
	const Key &k = e.key;
	gs_texrender_reset(e.render);
	if (!gs_texrender_begin(e.render, k.w, k.h)) {
		e.drawn = false;
		return;
	}
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	// Every uniform of the file, at every run.
	vec4 frame;
	vec4_set(&frame, k.m[0], k.m[1], k.m[2], k.m[3]);
	vec2 shift;
	vec2_set(&shift, k.shift[0], k.shift[1]);
	gs_effect_set_texture(sh.image, k.from);
	gs_effect_set_vec4(sh.frame, &frame);
	gs_effect_set_vec2(sh.frame_shift, &shift);
	gs_effect_set_float(sh.bright, k.bright);
	gs_effect_set_float(sh.saturation, k.saturation);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)k.w, 0.0f, (float)k.h, -100.0f, 100.0f);
	while (gs_effect_loop(sh.effect, "Fill"))
		gs_draw_sprite(nullptr, 0, k.w, k.h);
	gs_blend_state_pop();
	gs_texrender_end(e.render);
	gs_enable_framebuffer_srgb(previous_srgb);
	e.drawn = true;
	g_draws++;
}

// A background was renamed: the filters that name it follow.
void renamed(void *, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	const char *now = calldata_string(cd, "new_name");
	const char *before = calldata_string(cd, "prev_name");
	if (!source || !now || !before || !*before || !ambient_frame_of(source))
		return;
	struct Names {
		const char *before, *now;
	} names{before, now};
	static const auto each = [](void *param, obs_source_t *holder) {
		obs_source_enum_filters(
			holder,
			[](obs_source_t *, obs_source_t *filter, void *p) {
				const auto *n = static_cast<const Names *>(p);
				const char *id = obs_source_get_unversioned_id(filter);
				if (!id || (strcmp(id, kShadowBackdropId) != 0 && strcmp(id, kSceneShadowId) != 0))
					return;
				obs_data_t *settings = obs_source_get_settings(filter);
				if (strcmp(obs_data_get_string(settings, "outline_ambient"), n->before) == 0) {
					obs_data_set_string(settings, "outline_ambient", n->now);
					obs_source_update(filter, settings);
				}
				obs_data_release(settings);
			},
			param);
		return true;
	};
	obs_enum_sources(each, &names);
	obs_enum_scenes(each, &names);
}

} // namespace

uint32_t outline_ambient_choice(const char *name)
{
	if (!name || !*name)
		return 0;
	std::lock_guard lock(g_mutex);
	for (size_t i = 0; i < g_names.size(); i++)
		if (g_names[i] == name)
			return (uint32_t)i + 1;
	g_names.emplace_back(name);
	return (uint32_t)g_names.size();
}

uint32_t outline_ambient_resolve(uint32_t choice, obs_source_t *scene)
{
	// Only the answer kept: the lookup itself is the tick's (look_tick).
	// A choice not asked for before has none until the next tick.
	const uint64_t now = os_gettime_ns();
	std::lock_guard lock(g_mutex);
	for (Kept &k : g_kept)
		if (k.choice == choice && k.scene == scene &&
		    (!scene || (k.weak && obs_weak_source_references_source(k.weak, scene)))) {
			k.asked = now;
			return k.id;
		}
	if (g_kept.size() >= kMostKept)
		return 0;
	Kept k;
	k.choice = choice;
	k.scene = scene;
	k.weak = scene ? obs_source_get_weak_source(scene) : nullptr;
	k.asked = now;
	g_kept.push_back(k);
	return 0;
}

void outline_ambient_warm(uint32_t choice, obs_source_t *scene)
{
	outline_ambient_resolve(choice, scene);
	std::string name;
	{
		std::lock_guard lock(g_mutex);
		bool waiting = false;
		for (const Kept &k : g_kept)
			if (k.choice == choice && k.scene == scene)
				waiting = !k.looked;
		if (!waiting)
			return;
		if (choice && choice <= g_names.size())
			name = g_names[choice - 1];
	}
	const uint64_t frame_rev = ambient_frame_revision();
	const uint32_t id = find_now(name, scene);
	std::lock_guard lock(g_mutex);
	for (Kept &k : g_kept)
		if (k.choice == choice && k.scene == scene && !k.looked) {
			k.id = id;
			k.frame_rev = frame_rev;
			k.when = os_gettime_ns();
			k.looked = true;
		}
}

gs_texture_t *outline_ambient_texture(const OutlineDesign &design, uint32_t width, uint32_t height,
				      const AmbientCover &cover)
{
	if (!design.ambient_fill() || !width || !height)
		return nullptr;
	const Shader *sh = load();
	if (!sh)
		return nullptr;
	const uint32_t id = design.ambient_id ? design.ambient_id : outline_ambient_resolve(design.ambient, nullptr);
	AmbientFrame from;
	if (!ambient_frame_get(id, from))
		return nullptr;
	if (!g_watching.exchange(true))
		obs_add_tick_callback(sweep_tick, nullptr);
	const uint64_t frame = obs_get_video_frame_time();
	const uint64_t now = os_gettime_ns();
	g_last_use = now;
	Key key;
	key.id = id;
	key.frame = from.frame;
	key.from = from.texture;
	key.w = std::clamp<uint32_t>(width, 16, 2560);
	key.h = std::clamp<uint32_t>(height, 16, 2560);
	// The line's uv to the background's: through the background's pixels,
	// so a turn keeps its shapes; sine and cosine worked out here.
	const double aw = (double)from.width, ah = (double)from.height;
	const double scale = std::clamp(design.pattern_scale, 0.05, 40.0);
	const double r = design.pose.degrees * 3.14159265358979323846 / 180.0;
	const double c = std::cos(r), s = std::sin(r);
	key.m[0] = (float)(c * cover.x / scale);
	key.m[1] = (float)(s * cover.y * ah / (aw * scale));
	key.m[2] = (float)(-s * cover.x * aw / (ah * scale));
	key.m[3] = (float)(c * cover.y / scale);
	key.shift[0] = (float)design.pose.x;
	key.shift[1] = (float)design.pose.y;
	key.bright = (float)design.ambient_bright;
	key.saturation = (float)design.ambient_sat;

	Entry *found = nullptr, *spare = nullptr;
	for (Entry &e : g_entries) {
		if (e.key == key && e.drawn)
			found = &e;
		else if (e.frame != frame && e.key.w == key.w && e.key.h == key.h)
			spare = &e; // of an earlier frame, of this size: no longer read
	}
	if (found) {
		found->frame = frame;
		found->used = now;
		return gs_texrender_get_texture(found->render);
	}
	for (size_t i = g_entries.size(); i-- > 0;) {
		Entry &e = g_entries[i];
		if (&e != spare && now - e.used > kKeepNs) {
			const bool before = spare && spare > &e;
			gs_texrender_destroy(e.render);
			g_entries.erase(g_entries.begin() + (ptrdiff_t)i);
			if (before)
				spare--;
		}
	}
	if (!spare) {
		if (g_entries.size() >= kMost) {
			spare = &*std::min_element(g_entries.begin(), g_entries.end(),
						   [](const Entry &a, const Entry &b) { return a.used < b.used; });
		} else {
			Entry e;
			e.render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			g_entries.push_back(e);
			spare = &g_entries.back();
		}
	}
	g_held = (long long)g_entries.size();
	spare->key = key;
	spare->frame = frame;
	spare->used = now;
	draw(*spare, *sh);
	return spare->drawn ? gs_texrender_get_texture(spare->render) : nullptr;
}

void add_outline_ambient_property(obs_properties_t *outline)
{
	obs_property_t *list = obs_properties_add_list(outline, "outline_ambient",
						       obs_module_text("Shadow.OutlineAmbient"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("Shadow.OutlineAmbient.Auto"), "");
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			const char *name = obs_source_get_name(source);
			if (is_ambient(source) && name && *name)
				obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
			return true;
		},
		list);
	set_tip(list, obs_module_text("Shadow.OutlineAmbient.Tip"));
}

void outline_ambient_start()
{
	g_signals = obs_get_signal_handler();
	signal_handler_connect(g_signals, "source_rename", renamed, nullptr);
	if (!g_looking) {
		g_looking = true;
		obs_add_tick_callback(look_tick, nullptr);
	}
}

void outline_ambient_stop()
{
	if (g_signals)
		signal_handler_disconnect(g_signals, "source_rename", renamed, nullptr);
	g_signals = nullptr;
	if (g_watching.exchange(false))
		obs_remove_tick_callback(sweep_tick, nullptr);
	obs_enter_graphics();
	let_go_all();
	let_go_shader();
	obs_leave_graphics();
	if (g_looking) {
		g_looking = false;
		obs_remove_tick_callback(look_tick, nullptr);
	}
	std::vector<obs_weak_source_t *> weak;
	{
		std::lock_guard lock(g_mutex);
		for (Kept &k : g_kept)
			if (k.weak)
				weak.push_back(k.weak);
		g_kept.clear();
	}
	for (obs_weak_source_t *w : weak)
		obs_weak_source_release(w);
}

void outline_ambient_add_test_calls()
{
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_test_outline_ambient(in string file, out int held, out int draws)",
		[](void *, calldata_t *cd) {
			const char *file = nullptr;
			if (calldata_get_string(cd, "file", &file) && file && *file) {
				std::lock_guard lock(g_mutex);
				const std::string name = strcmp(file, "-") == 0 ? "" : file;
				if (name != g_test_file) {
					g_test_file = name;
					g_reload = true;
				}
			}
			calldata_set_int(cd, "held", g_held.load());
			calldata_set_int(cd, "draws", g_draws.load());
		},
		nullptr);
}

} // namespace stagekit
