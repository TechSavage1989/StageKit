/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "timing.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <utility>

namespace stagekit {

namespace {

constexpr size_t kMost = 48; // more kinds than StageKit has

// What one kind of filter or source was registered with, and what its tick
// and its drawing have cost since the last report. The drawing of a filter
// includes whatever it draws from (the source and the filters before it).
struct Kind {
	std::string id;
	void (*tick)(void *, float) = nullptr;
	void (*render)(void *, gs_effect_t *) = nullptr;
	std::atomic<uint64_t> tick_ns{0}, tick_max{0}, tick_calls{0};
	std::atomic<uint64_t> render_ns{0}, render_max{0}, render_calls{0};
};
std::array<Kind, kMost> g_kinds;
std::atomic<size_t> g_count{0};
std::atomic<uint64_t> g_report_at{0};

constexpr uint64_t kEvery = 30ULL * 1000000000ULL;
constexpr double kSlowCall = 3.0;  // a single call this long (ms) is worth a line
constexpr double kBusyShare = 2.0; // so is a kind that took this share (%) of the time

void add(std::atomic<uint64_t> &sum, std::atomic<uint64_t> &most, std::atomic<uint64_t> &calls, uint64_t took)
{
	sum += took;
	calls++;
	uint64_t seen = most.load();
	while (took > seen && !most.compare_exchange_weak(seen, took)) {
	}
}

void report(uint64_t now)
{
	uint64_t due = g_report_at.load();
	if (due == 0) {
		g_report_at.compare_exchange_strong(due, now + kEvery);
		return;
	}
	if (now < due || !g_report_at.compare_exchange_strong(due, now + kEvery))
		return;
	const double period_ms = kEvery / 1000000.0;
	for (size_t i = 0; i < g_count.load(); i++) {
		Kind &k = g_kinds[i];
		const uint64_t tn = k.tick_ns.exchange(0), tm = k.tick_max.exchange(0), tc = k.tick_calls.exchange(0);
		const uint64_t rn = k.render_ns.exchange(0), rm = k.render_max.exchange(0),
			       rc = k.render_calls.exchange(0);
		const double tick_ms = tn / 1000000.0, render_ms = rn / 1000000.0;
		const double tick_most = tm / 1000000.0, render_most = rm / 1000000.0;
		if (tick_most < kSlowCall && render_most < kSlowCall &&
		    (tick_ms + render_ms) / period_ms * 100.0 < kBusyShare)
			continue;
		obs_log(LOG_INFO,
			"timing, last 30 s: %s: tick %llu calls, %.2f ms each, longest %.1f ms; "
			"drawing %llu calls, %.2f ms each, longest %.1f ms; %.1f%% of the time",
			k.id.c_str(), (unsigned long long)tc, tc ? tick_ms / tc : 0.0, tick_most,
			(unsigned long long)rc, rc ? render_ms / rc : 0.0, render_most,
			(tick_ms + render_ms) / period_ms * 100.0);
	}
}

template<size_t I> void timed_tick(void *data, float seconds)
{
	Kind &k = g_kinds[I];
	const uint64_t began = os_gettime_ns();
	k.tick(data, seconds);
	const uint64_t ended = os_gettime_ns();
	add(k.tick_ns, k.tick_max, k.tick_calls, ended - began);
	report(ended);
}

template<size_t I> void timed_render(void *data, gs_effect_t *effect)
{
	Kind &k = g_kinds[I];
	const uint64_t began = os_gettime_ns();
	k.render(data, effect);
	add(k.render_ns, k.render_max, k.render_calls, os_gettime_ns() - began);
}

template<size_t... I> constexpr auto tick_table(std::index_sequence<I...>)
{
	return std::array<void (*)(void *, float), sizeof...(I)>{&timed_tick<I>...};
}
template<size_t... I> constexpr auto render_table(std::index_sequence<I...>)
{
	return std::array<void (*)(void *, gs_effect_t *), sizeof...(I)>{&timed_render<I>...};
}
const auto g_ticks = tick_table(std::make_index_sequence<kMost>{});
const auto g_renders = render_table(std::make_index_sequence<kMost>{});

} // namespace

void register_timed(obs_source_info *info)
{
	const size_t at = g_count.load();
	if (at >= kMost) {
		obs_register_source(info);
		return;
	}
	Kind &k = g_kinds[at];
	k.id = info->id ? info->id : "";
	k.tick = info->video_tick;
	k.render = info->video_render;
	g_count = at + 1;
	obs_source_info timed = *info;
	if (k.tick)
		timed.video_tick = g_ticks[at];
	if (k.render)
		timed.video_render = g_renders[at];
	obs_register_source(&timed);
}

} // namespace stagekit
