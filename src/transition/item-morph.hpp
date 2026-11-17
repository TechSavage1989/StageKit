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
// Morph's *Unpaired sources* value for Make Room.
inline constexpr const char *kUnpairedRoom = "morph_make_room";

// What a Morph transition's settings say that matters here.
struct MorphSaid {
	MorphLinks links;
	bool pair_main = true;
	bool seen = true;       // two sources change into each other by what shows of them
	bool no_shadows = true; // and by their pictures, without shadows, glows and outlines
	EaseCurve curve = EaseCurve::Cubic;
	EaseMode mode = EaseMode::InOut;
	// Make Room: a Morph whose *Unpaired sources* is Make Room, or the Make
	// Room transition itself. An item left by itself is moved off an edge of
	// the screen by a layer (it cannot leave its own frame otherwise).
	bool room = false;
	bool room_push = true;  // away from the change under way in its scene; else the nearest edge
	bool room_fade = false; // fading as it goes
	// False for the Make Room transition: its items never pair with another.
	bool pairs = true;
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

// The nearest edge of its scene, as the change began, of the item a Morph
// transition is the show or hide transition of ("left", "right", "up" or
// "down", the way it goes); null when not known. Nothing is held: the item
// is never handed out (see item-morph.cpp, Entry).
const char *item_morph_edge(obs_source_t *transition);

void register_item_morph();
// The scene collection is changing: every change under way is ended and
// everything held is let go of, before OBS looks for sources left over.
void item_morph_let_go();
// OBS is closing, or the plugin is being unloaded: the same, and nothing
// more is started.
void item_morph_stop();

} // namespace stagekit
