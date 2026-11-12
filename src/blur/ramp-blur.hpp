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

#include <functional>

namespace stagekit {

inline constexpr const char *kRampBlurId = "stagekit_ramp_blur";

class RampBlur;

void register_ramp_blur();

// Returns the RampBlur behind a filter source, or nullptr if the source is
// not a StageKit Ramp Blur.
RampBlur *ramp_blur_from_source(obs_source_t *filter);

// Calls fn for every Ramp Blur filter that currently exists. The filter
// source is kept alive for the duration of the call.
void for_each_ramp_blur(const std::function<void(RampBlur *, obs_source_t *filter)> &fn);

// Thread-safe controls used by hotkeys, obs-websocket and the Stream Deck.
// duration_ms < 0 uses the filter's configured ramp time.
void ramp_blur_set_active(RampBlur *blur, bool active, double duration_ms = -1.0);
void ramp_blur_toggle(RampBlur *blur);

// "Blur All": drives every Ramp Blur that has "Include in Blur All" ticked.
// Toggle turns everything off if any is on, otherwise everything on.
// Returns the resulting state.
enum class BlurAllMode { Off, On, Toggle };
bool ramp_blur_set_all(BlurAllMode mode);
bool ramp_blur_is_active(RampBlur *blur);
double ramp_blur_level(RampBlur *blur);
const char *ramp_blur_type_name(RampBlur *blur);

} // namespace stagekit
