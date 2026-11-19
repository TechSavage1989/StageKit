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

// Work handed to OBS's own thread, the one its window lives on.
//
// Scene items are added and removed there, sources made and let go of there,
// as OBS itself does: never from OBS's drawing thread (a tick callback, a
// source's tick or its drawing). Adding or removing a scene item takes the
// scene's locks, OBS's hotkey lock and each signal's own lock, and runs every
// listener's handler (OBS's window, obs-websocket, other plugins) on the
// calling thread. Done from a tick callback, the drawing thread held those
// and OBS's lock round all tick callbacks while a handler, or the lock
// itself, waited for the window's thread, which at that moment waited for
// one of them: OBS froze for good, with no crash file. See AGENTS.md, *Morph
// between two items of one scene*.

namespace stagekit {

// True in OBS. False where there is no window and so no such thread (the
// tests' programs that set no handler for these tasks): there the caller does
// the work itself, there being nobody to wait for one another with.
bool ui_thread_there();

// `task(param)` on that thread, later. Never waits. False, and nothing is
// queued, where there is no such thread.
bool on_ui_thread_later(obs_task_t task, void *param);

// For the tests (once): `stagekit_test_ui_tasks(in bool there)`, said after
// the test program has set its own handler with obs_set_ui_task_handler.
void ui_task_add_test_calls();

} // namespace stagekit
