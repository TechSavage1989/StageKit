/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "ambient-frame.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace stagekit {

namespace {

struct Entry {
	uint32_t id = 0;
	const obs_source_t *source = nullptr; // compared only, never used
	obs_weak_source_t *weak = nullptr;
	AmbientFrame frame;
	uint64_t tried = 0; // the frame it was last drawn here in: once a frame at most
};

// (The lock is only ever taken with nothing else of ours held, and never
// kept while OBS is asked for anything; where a graphics context is needed
// too, the context is entered first.)
std::mutex g_mutex;
std::vector<Entry> g_entries;
uint32_t g_next = 1;
std::atomic<uint64_t> g_revision{1};
std::atomic<long long> g_extra_draws{0};
// Only in a graphics context, which one thread has at a time.
gs_texrender_t *g_scratch = nullptr;
bool g_drawing = false;

Entry *find(uint32_t id)
{
	for (Entry &e : g_entries)
		if (e.id == id)
			return &e;
	return nullptr;
}

} // namespace

uint32_t ambient_frame_join(obs_source_t *source)
{
	Entry e;
	e.source = source;
	e.weak = obs_source_get_weak_source(source);
	std::lock_guard lock(g_mutex);
	e.id = g_next++;
	g_entries.push_back(e);
	g_revision++;
	return e.id;
}

void ambient_frame_leave(uint32_t id)
{
	obs_weak_source_t *weak = nullptr;
	{
		std::lock_guard lock(g_mutex);
		for (size_t i = 0; i < g_entries.size(); i++)
			if (g_entries[i].id == id) {
				weak = g_entries[i].weak;
				g_entries.erase(g_entries.begin() + (ptrdiff_t)i);
				break;
			}
		g_revision++;
	}
	obs_weak_source_release(weak);
}

void ambient_frame_publish(uint32_t id, gs_texture_t *texture, uint32_t width, uint32_t height)
{
	const uint64_t now = obs_get_video_frame_time();
	std::lock_guard lock(g_mutex);
	if (Entry *e = find(id)) {
		e->frame.texture = texture;
		e->frame.width = width;
		e->frame.height = height;
		e->frame.frame = now;
	}
}

bool ambient_frame_get(uint32_t id, AmbientFrame &out)
{
	if (!id)
		return false;
	const uint64_t now = obs_get_video_frame_time();
	const uint64_t fresh = 2 * obs_get_frame_interval_ns() + 1000000ULL;
	obs_weak_source_t *weak = nullptr;
	{
		std::lock_guard lock(g_mutex);
		Entry *e = find(id);
		if (!e)
			return false;
		out = e->frame;
		const bool stale = !out.texture || (now > out.frame && now - out.frame > fresh);
		if (stale && !g_drawing && e->weak && e->tried != now) {
			e->tried = now;
			weak = e->weak;
			obs_weak_source_addref(weak);
		}
	}
	if (weak) {
		// Nothing has drawn it lately: once, into a picture nobody looks
		// at; the background says which picture it finished as it draws.
		// (A background that is drawing right now, with this asked from
		// inside it, draws nothing: its last picture is taken.)
		obs_source_t *source = obs_weak_source_get_source(weak);
		obs_weak_source_release(weak);
		if (source) {
			if (!g_scratch)
				g_scratch = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			g_drawing = true;
			gs_texrender_reset(g_scratch);
			if (g_scratch && gs_texrender_begin(g_scratch, 8, 8)) {
				vec4 clear;
				vec4_zero(&clear);
				gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
				const uint32_t w = std::max(1u, obs_source_get_width(source));
				const uint32_t h = std::max(1u, obs_source_get_height(source));
				gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
				gs_blend_state_push();
				gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
				obs_source_video_render(source);
				gs_blend_state_pop();
				gs_texrender_end(g_scratch);
				g_extra_draws++;
			}
			g_drawing = false;
			obs_source_release(source);
		}
		std::lock_guard lock(g_mutex);
		Entry *e = find(id);
		if (!e)
			return false;
		out = e->frame;
	}
	return out.texture && out.width && out.height;
}

uint32_t ambient_frame_of(const obs_source_t *source)
{
	std::lock_guard lock(g_mutex);
	for (const Entry &e : g_entries)
		if (e.source == source)
			return e.id;
	return 0;
}

uint32_t ambient_frame_first()
{
	std::lock_guard lock(g_mutex);
	return g_entries.empty() ? 0 : g_entries.front().id;
}

uint64_t ambient_frame_revision()
{
	return g_revision.load();
}

void ambient_frame_stop()
{
	obs_enter_graphics();
	gs_texrender_destroy(g_scratch);
	g_scratch = nullptr;
	obs_leave_graphics();
}

void ambient_frame_add_test_calls()
{
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_ambient_frames(out int known, out int draws)",
		[](void *, calldata_t *cd) {
			long long known;
			{
				std::lock_guard lock(g_mutex);
				known = (long long)g_entries.size();
			}
			calldata_set_int(cd, "known", known);
			calldata_set_int(cd, "draws", g_extra_draws.load());
		},
		nullptr);
}

} // namespace stagekit
