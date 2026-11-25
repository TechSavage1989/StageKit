/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <obs-frontend-api.h>

class QMainWindow;
class QMenu;

namespace stagekit {

// OBS's window layout, kept by StageKit: where every dock is, docked or
// floating, and how the docks share the window. The owner: "I'd like
// stagekit to also have a tool to store my OBS dock and layout, and also auto
// load it at startup". OBS keeps its own layout too, but brings it back
// before every plugin's docks are there; this one is brought back once OBS
// has finished loading.

// Saves the layout of `window` to StageKit's own file. False when it could
// not be written.
bool window_layout_save(QMainWindow *window);
// Brings the saved layout back. False when there is none, or it does not fit
// this window.
bool window_layout_restore(QMainWindow *window);
bool window_layout_saved();
// Brings the saved layout back by itself (OBS started, a collection loaded, a
// dock came late): only a layout that can be trusted, never one an older
// version took as OBS closed, after OBS had taken its browser docks down.
bool window_layout_restore_auto(QMainWindow *window);
bool window_layout_trusted();
// Saves the layout when `window` is asked to close, if *Save it when OBS
// closes* is ticked: before OBS takes any dock down. Returns how many times
// it saved (0 when just installed; -1 with no window and no watch).
int window_layout_watch_close(QMainWindow *window);
// Sets one of the two choices ("at_start", "at_exit"), as the menu does.
void window_layout_set_choice(const char *name, bool on);
// For `for_ms` after the layout came back, a dock added to `window` brings
// the layout back again once docks stop coming (0.4 s), so docks that are
// added late (a service's browser dock, Quick Access docks) are put where
// they were saved. Replaces an earlier watch.
int window_layout_follow_late_docks(QMainWindow *window, int for_ms);
// How often the current watch brought the layout back (-1 without one).
int window_layout_late_restores();

// The entries for StageKit's menu under OBS's Tools: save, restore, and the
// two tick boxes (restore when OBS starts, save when OBS closes).
void window_layout_menu(QMenu *menu);
// To be told OBS's own events: the layout comes back after OBS has loaded,
// and is saved as OBS closes, each when asked for.
void window_layout_event(enum obs_frontend_event event);

// Whether the saved layout is brought back after this event (when the owner
// has *Restore it when a collection loads* ticked): OBS starting, and a change
// of scene collection.
bool window_layout_restores_after(enum obs_frontend_event event);

} // namespace stagekit
