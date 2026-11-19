/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs.h>

namespace stagekit {

// Registers a filter or source like obs_register_source, and keeps time of
// its tick and its drawing. Every 30 s, whatever took long is written to
// OBS's log (nothing is written while all is quick), so a log from a session
// with dropped frames says which part of StageKit to look at: a line per
// kind that was slow (times in ms with three decimals, so microseconds show),
// and one line with what all of StageKit's ticks cost a frame, when that is
// over 0.1 ms.
void register_timed(obs_source_info *info);

// A tick callback that is no source's own (obs_add_tick_callback), timed as
// a kind called `id` like a source's tick: give OBS what this returns, and
// take it off again with the same. (`tick` itself when no more kinds can be
// kept time of.)
using TickCallback = void (*)(void *, float);
TickCallback timed_tick_callback(const char *id, TickCallback tick);

// Adds the global procs of the timing (once): `stagekit_timing_take(out
// string json)`, which returns what every kind has cost since the last take
// and starts counting afresh, for the measuring harness and the tests;
// `stagekit_timing_report()`, which writes the log lines now (tests); and
// `stagekit_timing_probe()`, which registers a kind the tests time
// (`stagekit_timing_probe`, a tick that waits the `wait_us` of its settings).
void timing_add_procs();

// Where in a period a periodic job of this source starts, taken from its
// uuid, so many filters with the same period do not all work in the same
// tick. In [0, period).
double stagger_phase(obs_source_t *source, double period);
int stagger_phase(obs_source_t *source, int period);

// Periodic looks that are skipped when nothing changed, counted for the tests
// (`stagekit_test_looks(out string json)`: how many of each so far).
enum class Look : int {
	SwitchFollow,   // Show in Scenes' "Follow what is shown"
	LinkFollow,     // Link Visibility's "Follow the first source"
	ShadowScale,    // Shadow, Glow & Outline's scale and on-screen check
	AmbientSources, // Ambient Background looking its sources up
	Count,
};
void count_look(Look which);

} // namespace stagekit
