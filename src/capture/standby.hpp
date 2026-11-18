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

#include <string>

namespace stagekit {

inline constexpr const char *kStandbyId = "stagekit_standby";

// Registers the "Standby Picture (StageKit)" filter: a picture in a capture's
// place while the capture has nothing to show.
void register_standby();

// Is a Standby Picture on the source showing its picture because the capture
// has nothing? (Not while it changes over to a capture that has arrived.)
bool standby_waiting(obs_source_t *source);

// Shows the picture of an enabled Standby Picture on the source over its
// capture, or takes it away again. False when the source has none.
bool standby_cover_toggle(obs_source_t *source);

// Shows (on) or takes away the picture of an enabled Standby Picture on the
// source over its capture, with a transition lent for that change and the
// one back: `style` is a Standby Picture transition by number (-1: its own),
// `ms` how long, `curve` and `mode` the easing. False when there is none.
bool standby_cover_set(obs_source_t *source, bool on, int style, int ms, int curve, int mode);

// The Standby Image filter on the source named `filter` (the first enabled
// one when empty), covered or asked: `how` "on", "off" or "toggle" shows or
// takes away its image over the source with the filter's own transition,
// "" only asks. Sets `name` (the filter's), `covering` (after the press) and
// `enabled`; false with `why` when there is no such filter, or, to press,
// it is switched off. For the "StandbyCover" and "GetStandbyCover" requests.
bool standby_cover_press(obs_source_t *source, const std::string &filter, const std::string &how, std::string &name,
			 bool &covering, bool &enabled, std::string &why);

// Is an enabled Standby Image on the source showing its image over it on purpose?
bool standby_covered(obs_source_t *source);

// Has the source an enabled Standby Image filter?
bool standby_present(obs_source_t *source);

} // namespace stagekit
