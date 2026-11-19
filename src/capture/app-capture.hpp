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

#include <cstdint>
#include <string>

namespace stagekit {

inline constexpr const char *kAppCaptureId = "stagekit_app_capture";

// Capture App in Use: a filter on one Window Capture that aims it at
// whichever of several listed apps is in use. See app-capture.cpp.
void register_app_capture();

// Whether a Capture App in Use on this source is showing the last picture it
// had while the capture itself has none: between two apps, or just after
// the app closed. For Standby Picture, which must not stand in meanwhile.
bool app_capture_holding(obs_source_t *capture);

// How many times the Automatic Window Switchers on this source have changed
// window: something that shows the capture's picture elsewhere (Ambient
// Background) can tell that it is another window's now.
uint64_t app_capture_changes(obs_source_t *capture);

// Whether an enabled Automatic Window Switcher lists this program (its file
// name, or a window title, both in lower case). Auto Capture leaves those
// alone: they are windows to capture as windows, not games to hook.
bool app_capture_lists(const std::string &executable, const std::string &title);

// A change from one window's picture to another's, while it runs: the size
// the capture reports now, the size it is on its way to, how long the change
// takes in all and how far along it is (0 to 1), and its easing. `id` counts
// the changes. For Fit to Space, which takes the new picture's space with the
// change instead of after it.
struct AppCaptureChange {
	uint64_t id = 0;
	double now_w = 0.0, now_h = 0.0, to_w = 0.0, to_h = 0.0;
	double total_s = 0.0, done = 0.0;
	int curve = 0, mode = 0;
};
// False when no Automatic Window Switcher on the source is changing over.
bool app_capture_change(obs_source_t *capture, AppCaptureChange &change);

// Whether the source has an enabled Automatic Window Switcher: it glides the
// capture's size from one window's to the next itself, so what comes after
// it (Standby Picture) follows that size as it is and does not glide again.
bool app_capture_present(obs_source_t *capture);

// The window an enabled Automatic Window Switcher on this capture shows now:
// the one whose image it draws (by way of a capture kept ready for it, or
// the capture itself). Since beta119 the capture itself is left aimed at an
// earlier window while a ready capture is drawn, so the capture's own
// settings do not say what is on screen: anything that goes by the program
// shown (Auto Frame's saved shapes) asks this first.
struct AppCaptureShown {
	// False: nothing is shown (nothing listed is open, or its window has
	// closed and its last image is no longer held).
	bool any = false;
	std::string executable; // the program's file name, as Windows gives it ("firefox.exe")
	std::string title;      // the window's title now
};
// False when no enabled switcher on the capture has a say in what it shows:
// there is none, it is switched off, or it has chosen no window since OBS
// started or it was switched on and passes the capture's own image on as it
// is (it lists nothing, or has not looked yet). The capture's own settings
// say what it shows then. Safe from any thread; holds nothing after it
// returns.
bool app_capture_shown_window(obs_source_t *capture, AppCaptureShown &shown);

struct OpenWindow;
// Hands a window picked by hand to an enabled Capture App in Use on this
// capture: it changes over to it in its own way, holds it, and remembers its
// program among the apps switched to. False when there is no such filter.
bool app_capture_hold(obs_source_t *capture, const OpenWindow &window);

} // namespace stagekit
