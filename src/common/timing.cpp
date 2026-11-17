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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

constexpr size_t kMost = 64; // more kinds than StageKit has

// What one kind of filter or source was registered with, and what its tick
// and its drawing have cost. The sums only grow; the report and the take
// each keep what they had seen last and work with the difference. The
// drawing of a filter includes whatever it draws from (the source and the
// filters before it).
struct Kind {
	std::string id;
	void (*tick)(void *, float) = nullptr;
	void (*render)(void *, gs_effect_t *) = nullptr;
	std::atomic<uint64_t> tick_ns{0}, tick_calls{0}, render_ns{0}, render_calls{0};
	// Longest single call since the last report, and since the last take.
	std::atomic<uint64_t> tick_max{0}, render_max{0}, take_tick_max{0}, take_render_max{0};
	// Ticks of a millisecond or more since the last take: headless, where a
	// read-back in a tick waits for llvmpipe's drawing, these are mostly
	// that wait; the take gives them apart.
	std::atomic<uint64_t> take_tick_long_ns{0}, take_tick_long_calls{0};
	// What the last report and the last take saw (under their own locks).
	uint64_t rep_tick_ns = 0, rep_tick_calls = 0, rep_render_ns = 0, rep_render_calls = 0;
	uint64_t took_tick_ns = 0, took_tick_calls = 0, took_render_ns = 0, took_render_calls = 0;
};

// How many calls took how long, since the last take, for its percentiles:
// eight steps to each doubling (about 9 % apart), from 32 ns.
constexpr int kSteps = 8;
constexpr int kBuckets = kSteps * 30;
struct Spread {
	std::array<std::atomic<uint32_t>, kBuckets> n{};

	static int bucket(uint64_t ns)
	{
		if (ns < 32)
			return 0;
		int top = 5; // ns in [2^top, 2^(top+1))
		while (top < 63 && (ns >> (top + 1)))
			top++;
		const int part = (int)((ns >> (top - 3)) & (kSteps - 1));
		const int at = (top - 5) * kSteps + part;
		return std::min(at, kBuckets - 1);
	}
	// The middle of a bucket, in ns.
	static double middle(int at)
	{
		const int top = at / kSteps + 5, part = at % kSteps;
		const double low = (double)(1ULL << top) * (1.0 + part / (double)kSteps);
		return low * (1.0 + 0.5 / (kSteps + part));
	}
	void add(uint64_t ns) { n[bucket(ns)].fetch_add(1, std::memory_order_relaxed); }
	// Takes the counts and starts afresh; the times at these shares, in µs.
	void take(std::initializer_list<double> shares, std::vector<double> &out)
	{
		std::array<uint32_t, kBuckets> got{};
		uint64_t all = 0;
		for (int i = 0; i < kBuckets; i++)
			all += got[i] = n[i].exchange(0, std::memory_order_relaxed);
		for (double share : shares) {
			double at_us = 0.0;
			if (all) {
				const uint64_t want = std::max<uint64_t>(1, (uint64_t)(share * (double)all + 0.999999));
				uint64_t seen = 0;
				for (int i = 0; i < kBuckets; i++) {
					seen += got[i];
					if (seen >= want) {
						at_us = middle(i) / 1000.0;
						break;
					}
				}
			}
			out.push_back(at_us);
		}
	}
};
std::array<Spread, kMost> g_tick_spread, g_render_spread;

// All of StageKit's ticks in one frame (OBS ticks every source once a frame,
// on its graphics thread), for the per-frame percentiles of the take.
std::atomic<uint64_t> g_frame_at{0}, g_frame_ns{0};
std::atomic<uint64_t> g_frame_max{0};
Spread g_frame_spread;

void frame_add(uint64_t took)
{
	const uint64_t frame = obs_get_video_frame_time();
	if (g_frame_at.load(std::memory_order_relaxed) != frame) {
		const uint64_t was = g_frame_at.exchange(frame, std::memory_order_relaxed);
		if (was != frame) {
			const uint64_t sum = g_frame_ns.exchange(0, std::memory_order_relaxed);
			if (was && sum) {
				g_frame_spread.add(sum);
				uint64_t seen = g_frame_max.load(std::memory_order_relaxed);
				while (sum > seen && !g_frame_max.compare_exchange_weak(seen, sum)) {
				}
			}
		}
	}
	g_frame_ns.fetch_add(took, std::memory_order_relaxed);
}
std::array<Kind, kMost> g_kinds;
std::atomic<size_t> g_count{0};
std::atomic<uint64_t> g_report_at{0};
std::mutex g_report_mutex, g_take_mutex;
uint64_t g_report_frames = 0, g_report_ns = 0, g_take_frames = 0, g_take_at = 0;

constexpr uint64_t kEvery = 30ULL * 1000000000ULL;
constexpr double kSlowCall = 3.0;  // a single call this long (ms) is worth a line
constexpr double kBusyShare = 2.0; // so is a kind that took this share (%) of the time
constexpr double kFrameLine = 0.1; // and all ticks together taking this long (ms) a frame

void most_of(std::atomic<uint64_t> &most, uint64_t took)
{
	uint64_t seen = most.load(std::memory_order_relaxed);
	while (took > seen && !most.compare_exchange_weak(seen, took)) {
	}
}

void add(std::atomic<uint64_t> &sum, std::atomic<uint64_t> &calls, std::atomic<uint64_t> &most,
	 std::atomic<uint64_t> &take_most, uint64_t took)
{
	sum.fetch_add(took, std::memory_order_relaxed);
	calls.fetch_add(1, std::memory_order_relaxed);
	most_of(most, took);
	most_of(take_most, took);
}

// The frames OBS ticked its sources for: a frame it was too late for is
// counted in the total but not ticked.
uint64_t ticked_frames()
{
	const uint64_t total = obs_get_total_frames(), lagged = obs_get_lagged_frames();
	return total > lagged ? total - lagged : 0;
}

// A kind's name as the per-frame line shows it: without "stagekit_".
std::string short_name(const std::string &id)
{
	return id.rfind("stagekit_", 0) == 0 ? id.substr(9) : id;
}

// Writes the lines for the time since the last report (30 s, or less when
// the tests ask for it now).
void write_report()
{
	std::lock_guard lock(g_report_mutex);
	const uint64_t now = os_gettime_ns();
	const double period_ms = g_report_ns && now > g_report_ns ? (now - g_report_ns) / 1000000.0
								  : kEvery / 1000000.0;
	g_report_ns = now;
	const uint64_t frames_now = ticked_frames();
	const uint64_t frames = frames_now > g_report_frames ? frames_now - g_report_frames : 0;
	g_report_frames = frames_now;
	struct Share {
		std::string name;
		double ms;
	};
	std::vector<Share> shares;
	double ticks_ms = 0.0, drawing_ms = 0.0;
	for (size_t i = 0; i < g_count.load(); i++) {
		Kind &k = g_kinds[i];
		const uint64_t tn_all = k.tick_ns.load(), tc_all = k.tick_calls.load();
		const uint64_t rn_all = k.render_ns.load(), rc_all = k.render_calls.load();
		const uint64_t tn = tn_all - k.rep_tick_ns, tc = tc_all - k.rep_tick_calls;
		const uint64_t rn = rn_all - k.rep_render_ns, rc = rc_all - k.rep_render_calls;
		k.rep_tick_ns = tn_all;
		k.rep_tick_calls = tc_all;
		k.rep_render_ns = rn_all;
		k.rep_render_calls = rc_all;
		const uint64_t tm = k.tick_max.exchange(0), rm = k.render_max.exchange(0);
		const double tick_ms = tn / 1000000.0, render_ms = rn / 1000000.0;
		const double tick_most = tm / 1000000.0, render_most = rm / 1000000.0;
		ticks_ms += tick_ms;
		drawing_ms += render_ms;
		if (tc)
			shares.push_back({short_name(k.id), tick_ms});
		if (tick_most < kSlowCall && render_most < kSlowCall &&
		    (tick_ms + render_ms) / period_ms * 100.0 < kBusyShare)
			continue;
		obs_log(LOG_INFO,
			"timing, last 30 s: %s: tick %llu calls, %.3f ms each, longest %.1f ms; "
			"drawing %llu calls, %.3f ms each, longest %.1f ms; %.1f%% of the time",
			k.id.c_str(), (unsigned long long)tc, tc ? tick_ms / tc : 0.0, tick_most,
			(unsigned long long)rc, rc ? render_ms / rc : 0.0, render_most,
			(tick_ms + render_ms) / period_ms * 100.0);
	}
	// One line for all of StageKit's ticks a frame: what OBS's own
	// `tick_sources` holds of StageKit. Cheap kinds that are many add up
	// here while no line of their own is written.
	if (!frames || ticks_ms / frames < kFrameLine)
		return;
	std::sort(shares.begin(), shares.end(), [](const Share &a, const Share &b) { return a.ms > b.ms; });
	std::string parts;
	for (size_t i = 0; i < shares.size() && i < 8; i++) {
		char part[96];
		snprintf(part, sizeof(part), "%s%s %.3f", i ? ", " : "", shares[i].name.c_str(), shares[i].ms / frames);
		parts += part;
	}
	obs_log(LOG_INFO,
		"timing, last 30 s: StageKit ticks %.3f ms a frame (%s); drawing %.3f ms a frame; over %llu frames",
		ticks_ms / frames, parts.c_str(), drawing_ms / frames, (unsigned long long)frames);
}

void report(uint64_t now)
{
	uint64_t due = g_report_at.load(std::memory_order_relaxed);
	if (due == 0) {
		if (g_report_at.compare_exchange_strong(due, now + kEvery)) {
			std::lock_guard lock(g_report_mutex);
			g_report_frames = ticked_frames();
			g_report_ns = now;
		}
		return;
	}
	if (now < due || !g_report_at.compare_exchange_strong(due, now + kEvery))
		return;
	write_report();
}

template<size_t I> void timed_tick(void *data, float seconds)
{
	Kind &k = g_kinds[I];
	const uint64_t began = os_gettime_ns();
	k.tick(data, seconds);
	const uint64_t ended = os_gettime_ns();
	add(k.tick_ns, k.tick_calls, k.tick_max, k.take_tick_max, ended - began);
	g_tick_spread[I].add(ended - began);
	if (ended - began >= 1000000) {
		k.take_tick_long_ns.fetch_add(ended - began, std::memory_order_relaxed);
		k.take_tick_long_calls.fetch_add(1, std::memory_order_relaxed);
	}
	frame_add(ended - began);
	report(ended);
}

template<size_t I> void timed_render(void *data, gs_effect_t *effect)
{
	Kind &k = g_kinds[I];
	const uint64_t began = os_gettime_ns();
	k.render(data, effect);
	const uint64_t took = os_gettime_ns() - began;
	add(k.render_ns, k.render_calls, k.render_max, k.take_render_max, took);
	g_render_spread[I].add(took);
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

// What every kind has cost since the last take, as JSON; counting starts
// afresh. Times in microseconds.
std::string take()
{
	std::lock_guard lock(g_take_mutex);
	const uint64_t now = os_gettime_ns();
	const uint64_t frames_now = ticked_frames();
	obs_data_t *out = obs_data_create();
	obs_data_set_int(out, "frames", (long long)(frames_now - std::min(frames_now, g_take_frames)));
	obs_data_set_double(out, "seconds", g_take_at ? (now - g_take_at) / 1e9 : 0.0);
	g_take_frames = frames_now;
	g_take_at = now;
	obs_data_array_t *kinds = obs_data_array_create();
	for (size_t i = 0; i < g_count.load(); i++) {
		Kind &k = g_kinds[i];
		const uint64_t tn_all = k.tick_ns.load(), tc_all = k.tick_calls.load();
		const uint64_t rn_all = k.render_ns.load(), rc_all = k.render_calls.load();
		obs_data_t *kind = obs_data_create();
		obs_data_set_string(kind, "id", k.id.c_str());
		obs_data_set_int(kind, "tick_calls", (long long)(tc_all - k.took_tick_calls));
		obs_data_set_double(kind, "tick_us", (tn_all - k.took_tick_ns) / 1000.0);
		obs_data_set_double(kind, "tick_max_us", k.take_tick_max.exchange(0) / 1000.0);
		obs_data_set_double(kind, "tick_long_us", k.take_tick_long_ns.exchange(0) / 1000.0);
		obs_data_set_int(kind, "tick_long_calls", (long long)k.take_tick_long_calls.exchange(0));
		obs_data_set_int(kind, "render_calls", (long long)(rc_all - k.took_render_calls));
		obs_data_set_double(kind, "render_us", (rn_all - k.took_render_ns) / 1000.0);
		obs_data_set_double(kind, "render_max_us", k.take_render_max.exchange(0) / 1000.0);
		std::vector<double> at;
		g_tick_spread[i].take({0.5, 0.95}, at);
		g_render_spread[i].take({0.5, 0.95}, at);
		obs_data_set_double(kind, "tick_p50_us", at[0]);
		obs_data_set_double(kind, "tick_p95_us", at[1]);
		obs_data_set_double(kind, "render_p50_us", at[2]);
		obs_data_set_double(kind, "render_p95_us", at[3]);
		k.took_tick_ns = tn_all;
		k.took_tick_calls = tc_all;
		k.took_render_ns = rn_all;
		k.took_render_calls = rc_all;
		obs_data_array_push_back(kinds, kind);
		obs_data_release(kind);
	}
	obs_data_set_array(out, "kinds", kinds);
	// All of StageKit's ticks a frame (frames StageKit ticked in).
	std::vector<double> at;
	g_frame_spread.take({0.5, 0.95}, at);
	obs_data_set_double(out, "frame_tick_p50_us", at[0]);
	obs_data_set_double(out, "frame_tick_p95_us", at[1]);
	obs_data_set_double(out, "frame_tick_max_us", g_frame_max.exchange(0) / 1000.0);
	obs_data_array_release(kinds);
	std::string json = obs_data_get_json(out);
	obs_data_release(out);
	return json;
}

// A kind for the tests: its tick waits as long as it is told, so what the
// timing says can be checked against a known cost.
struct Probe {
	std::atomic<long long> wait_us{0};
};

void register_probe()
{
	static bool registered = false;
	if (registered)
		return;
	registered = true;
	static obs_source_info info = {};
	info.id = "stagekit_timing_probe";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = [](void *) {
		return "StageKit timing probe";
	};
	info.create = [](obs_data_t *settings, obs_source_t *) -> void * {
		auto *probe = new Probe;
		probe->wait_us = obs_data_get_int(settings, "wait_us");
		return probe;
	};
	info.destroy = [](void *data) {
		delete static_cast<Probe *>(data);
	};
	info.update = [](void *data, obs_data_t *settings) {
		static_cast<Probe *>(data)->wait_us = obs_data_get_int(settings, "wait_us");
	};
	info.get_width = [](void *) -> uint32_t {
		return 0;
	};
	info.get_height = [](void *) -> uint32_t {
		return 0;
	};
	info.video_tick = [](void *data, float) {
		const long long wait = static_cast<Probe *>(data)->wait_us.load();
		const uint64_t until = os_gettime_ns() + (uint64_t)std::max(0LL, wait) * 1000ULL;
		while (os_gettime_ns() < until) {
		}
	};
	register_timed(&info);
}

uint32_t uuid_hash(obs_source_t *source)
{
	const char *uuid = source ? obs_source_get_uuid(source) : nullptr;
	uint32_t hash = 2166136261u;
	for (const char *c = uuid ? uuid : ""; *c; c++)
		hash = (hash ^ (unsigned char)*c) * 16777619u;
	return hash;
}

} // namespace

void register_timed(obs_source_info *info)
{
	const size_t at = g_count.load();
	if (at >= kMost) {
		obs_log(LOG_WARNING, "timing: more kinds than kept time of; %s is not timed", info->id);
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

void timing_add_procs()
{
	static bool added = false;
	if (added)
		return;
	added = true;
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(
		ph, "void stagekit_timing_take(out string json)",
		[](void *, calldata_t *cd) { calldata_set_string(cd, "json", take().c_str()); }, nullptr);
	proc_handler_add(ph, "void stagekit_timing_report()", [](void *, calldata_t *) { write_report(); }, nullptr);
	proc_handler_add(ph, "void stagekit_timing_probe()", [](void *, calldata_t *) { register_probe(); }, nullptr);
}

double stagger_phase(obs_source_t *source, double period)
{
	if (!(period > 0.0))
		return 0.0;
	return (uuid_hash(source) % 10007u) / 10007.0 * period;
}

int stagger_phase(obs_source_t *source, int period)
{
	return period > 0 ? (int)(uuid_hash(source) % (uint32_t)period) : 0;
}

} // namespace stagekit
