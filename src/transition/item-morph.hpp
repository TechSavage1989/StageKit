/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include "morph-paint.hpp"
#include "../common/easing.hpp"

#include <obs.h>

// Morph between two items of one scene: one hidden and another shown at the
// same moment, each with Morph as its way of going and coming. The one moves,
// resizes and changes into the other. See item-morph.cpp.

namespace stagekit {

inline constexpr const char *kMorphLayerId = "stagekit_morph_layer";

// What a Morph transition's settings say that matters here.
struct MorphSaid {
	MorphLinks links;
	bool pair_main = true;
	bool seen = true;       // two sources change into each other by what shows of them
	bool no_shadows = true; // and by their pictures, without shadows, glows and outlines
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
};

// Told by every Morph transition: its settings, each start and stop, and
// that it is no more. Safe from any thread.
void item_morph_said(obs_source_t *transition, const MorphSaid &said);
void item_morph_started(obs_source_t *transition);
void item_morph_stopped(obs_source_t *transition);
void item_morph_gone(obs_source_t *transition);

// What a Morph transition that is an item's own is to draw.
enum class ItemMorph {
	None,    // not known to be an item's: as before
	Waiting, // an item's, and its other half may still come: a hiding item stays as it is, a showing one is not drawn
	Paired, // the change is drawn for it: it draws nothing
	Alone,  // nothing to change into or from: it fades by itself
};
ItemMorph item_morph_now(obs_source_t *transition);

// The item a Morph transition is the show or hide transition of, held, and
// the scene it was found in (held, into `scene`); null when not known.
obs_sceneitem_t *item_morph_item(obs_source_t *transition, obs_source_t **scene);

void register_item_morph();
// OBS is closing, or the plugin is being unloaded: every change under way is
// ended and nothing more is started.
void item_morph_stop();

} // namespace stagekit
