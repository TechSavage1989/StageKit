/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "outline-pattern.hpp"

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

// The techniques by OutlineDesign::kind less OutlineDesign::Aurora, and
// which of Ambient Background's two pattern files each is in (the first ten
// patterns and the later ones: a refusal of one file leaves the other).
constexpr const char *kFiles[] = {"effects/stagekit-patterns.effect", "effects/stagekit-patterns-more.effect"};
constexpr const char *kTechniques[] = {"Aurora", "Plasma",  "Lava", "Waves", "Nebula", "Gradient", "Stars", "Bokeh",
				       "Rays",   "Circuit", "Hex",  "Rain",  "Tunnel", "Ripples",  "Embers"};
constexpr int file_of(int kind)
{
	return kind >= OutlineDesign::Circuit ? 1 : 0;
}
// As Ambient Background: a pattern is back where it began after this long at
// a speed of 100 %, which here is the Speed control's 60.
constexpr double kLoopS = 240.0;
constexpr double kFullSpeed = 60.0;
constexpr double kDetail = 0.5;
constexpr uint64_t kKeepNs = 2000000000ULL;
constexpr size_t kMost = 24;

struct Key {
	int kind = 0;
	uint32_t colour[3] = {0, 0, 0};
	uint32_t w = 0, h = 0;
	float phase = 0.0f, scale = 1.0f;
	bool operator==(const Key &o) const
	{
		return kind == o.kind && colour[0] == o.colour[0] && colour[1] == o.colour[1] &&
		       colour[2] == o.colour[2] && w == o.w && h == o.h && phase == o.phase && scale == o.scale;
	}
};

struct Entry {
	Key key;
	gs_texrender_t *render = nullptr;
	uint64_t frame = 0; // the video frame it was last asked for in
	uint64_t used = 0;  // by the clock
	bool drawn = false;
};

// (All of it only in a graphics context, which one thread has at a time; the
// mutex is for the tests' file name and the counts.)
std::vector<Entry> g_entries;
struct Shaders {
	gs_effect_t *effect = nullptr;
	bool tried = false;
	gs_eparam_t *phase = nullptr, *aspect = nullptr, *scale = nullptr, *detail = nullptr;
	gs_eparam_t *colour[3] = {nullptr, nullptr, nullptr};
};
Shaders g_shaders[2];
bool g_said = false; // that the patterns are not there: once
std::mutex g_mutex;
std::string g_test_file;
bool g_reload = false;
std::atomic<long long> g_held{0}, g_draws{0};
std::atomic<uint64_t> g_last_use{0};
std::atomic<bool> g_watching{false};

void let_go_all()
{
	for (Entry &e : g_entries)
		gs_texrender_destroy(e.render);
	g_entries.clear();
	g_held = 0;
}

void let_go_shaders()
{
	for (Shaders &sh : g_shaders) {
		// (OBS keeps one copy of an effect file for all who load it;
		// letting go of ours is all there is to do.)
		gs_effect_destroy(sh.effect);
		sh = Shaders();
	}
	g_said = false;
}

// Pictures nobody has asked for in a while are let go of, also when no line
// asks any more.
void sweep_tick(void *, float)
{
	if (g_held.load() == 0)
		return;
	const uint64_t now = os_gettime_ns();
	if (now - g_last_use.load() < kKeepNs)
		return;
	obs_enter_graphics();
	let_go_all();
	obs_leave_graphics();
}

Shaders *load(int which)
{
	std::string file;
	{
		std::lock_guard lock(g_mutex);
		if (g_reload) {
			g_reload = false;
			let_go_shaders();
		}
		file = g_test_file.empty() ? kFiles[which] : g_test_file;
	}
	Shaders &sh = g_shaders[which];
	if (sh.tried)
		return sh.effect ? &sh : nullptr;
	sh.tried = true;
	char *path = obs_module_file(file.c_str());
	char *errors = nullptr;
	sh.effect = path ? gs_effect_create_from_file(path, &errors) : nullptr;
	bfree(path);
	if (sh.effect) {
		sh.phase = gs_effect_get_param_by_name(sh.effect, "phase");
		sh.aspect = gs_effect_get_param_by_name(sh.effect, "aspect");
		sh.scale = gs_effect_get_param_by_name(sh.effect, "scale");
		sh.detail = gs_effect_get_param_by_name(sh.effect, "detail");
		sh.colour[0] = gs_effect_get_param_by_name(sh.effect, "colour1");
		sh.colour[1] = gs_effect_get_param_by_name(sh.effect, "colour2");
		sh.colour[2] = gs_effect_get_param_by_name(sh.effect, "colour3");
	} else if (!g_said) {
		g_said = true;
		obs_log(LOG_ERROR,
			"Outline patterns are not available (failed to load '%s': %s); such an outline is drawn in its outline colour.",
			file.c_str(), errors ? errors : "no such file");
	}
	bfree(errors);
	return sh.effect ? &sh : nullptr;
}

void draw(Entry &e, const Shaders &sh)
{
	const Key &k = e.key;
	gs_texrender_reset(e.render);
	if (!gs_texrender_begin(e.render, k.w, k.h)) {
		e.drawn = false;
		return;
	}
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	// Every uniform of the file, at every run (Ambient Background uses the
	// same copy of it).
	vec2 aspect;
	vec2_set(&aspect, (float)k.w / (float)k.h, 1.0f);
	gs_effect_set_float(sh.phase, k.phase);
	gs_effect_set_vec2(sh.aspect, &aspect);
	gs_effect_set_float(sh.scale, k.scale);
	gs_effect_set_float(sh.detail, (float)kDetail);
	for (int i = 0; i < 3; i++) {
		vec4 colour;
		vec4_from_rgba(&colour, k.colour[i] | 0xFF000000u);
		gs_effect_set_vec4(sh.colour[i], &colour);
	}
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_ortho(0.0f, (float)k.w, 0.0f, (float)k.h, -100.0f, 100.0f);
	while (gs_effect_loop(sh.effect, kTechniques[k.kind - OutlineDesign::Aurora]))
		gs_draw_sprite(nullptr, 0, k.w, k.h);
	gs_blend_state_pop();
	gs_texrender_end(e.render);
	gs_enable_framebuffer_srgb(previous_srgb);
	e.drawn = true;
	g_draws++;
}

} // namespace

gs_texture_t *outline_pattern_texture(const OutlineDesign &design, uint32_t outline_color, uint32_t width,
				      uint32_t height, double size)
{
	if (!design.pattern() || !width || !height)
		return nullptr;
	const Shaders *sh = load(file_of(design.kind));
	if (!sh)
		return nullptr;
	if (!g_watching.exchange(true))
		obs_add_tick_callback(sweep_tick, nullptr);
	const uint64_t frame = obs_get_video_frame_time();
	const uint64_t now = os_gettime_ns();
	g_last_use = now;
	Key key;
	key.kind = design.kind;
	key.colour[0] = outline_color & 0xFFFFFFu;
	key.colour[1] = design.color2 & 0xFFFFFFu;
	key.colour[2] = design.color3 & 0xFFFFFFu;
	// Half the size it is shown at, as Ambient Background draws them; the
	// patterns are soft.
	key.w = std::clamp<uint32_t>((width + 1) / 2, 16, 1280);
	key.h = std::clamp<uint32_t>((height + 1) / 2, 16, 1280);
	// (The clock in double precision, wrapped to one loop here: the shader
	// never sees a large number, and there is no seam but the loop's own.)
	double phase = design.speed == 0.0 ? 0.0 : (double)frame * 1e-9 * (design.speed / kFullSpeed) / kLoopS;
	phase -= std::floor(phase);
	key.phase = (float)phase;
	key.scale = (float)std::clamp(design.pattern_scale * size, 0.05, 40.0);

	Entry *found = nullptr, *spare = nullptr;
	for (Entry &e : g_entries) {
		if (e.key == key && e.drawn)
			found = &e;
		// One of an earlier frame, of this size: its picture is no longer
		// read. (The one this very line had, as a rule.)
		else if (e.frame != frame && e.key.w == key.w && e.key.h == key.h &&
			 (!spare || (e.key.kind == key.kind && spare->key.kind != key.kind)))
			spare = &e;
	}
	if (found) {
		found->frame = frame;
		found->used = now;
		return gs_texrender_get_texture(found->render);
	}
	// The ones not asked for in a while go, whatever their size.
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
			// (More lines at once than this never share a frame in
			// practice; the oldest picture is taken.)
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

void outline_pattern_stop()
{
	if (g_watching.exchange(false))
		obs_remove_tick_callback(sweep_tick, nullptr);
	obs_enter_graphics();
	let_go_all();
	let_go_shaders();
	obs_leave_graphics();
}

void outline_pattern_test_file(const char *file)
{
	std::lock_guard lock(g_mutex);
	const std::string name = file ? file : "";
	if (name == g_test_file)
		return;
	g_test_file = name;
	g_reload = true;
}

void outline_pattern_add_test_calls()
{
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(
		ph, "void stagekit_test_outline_patterns(in string file, out int held, out int draws)",
		[](void *, calldata_t *cd) {
			const char *file = nullptr;
			if (calldata_get_string(cd, "file", &file) && file && *file)
				outline_pattern_test_file(strcmp(file, "-") == 0 ? "" : file);
			calldata_set_int(cd, "held", g_held.load());
			calldata_set_int(cd, "draws", g_draws.load());
		},
		nullptr);
}

} // namespace stagekit
