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
#include <vector>

namespace stagekit {

inline constexpr const char *kAutoCaptureId = "stagekit_auto_capture";

void register_auto_capture();

// The file names of the programs with a window open now, everyday programs
// (OBS, browsers, launchers) left out. Empty on systems other than Windows.
std::vector<std::string> open_program_names();

// One open (not minimised) window. Empty list on systems other than Windows.
struct OpenWindow {
	std::string title, executable;
	bool foreground = false;
};
// Every open window, everyday programs included. Looked up at most five
// times a second however many callers ask.
std::vector<OpenWindow> open_windows();

// Aims a Game Capture or a Window Capture at the window in front right now,
// for a key that hooks a capture by hand. An Auto Capture on that source is
// switched off, or it would aim the capture somewhere else again a moment
// later; switching it back on hands the capture back to it. On success
// `what` is the program taken; otherwise `error` says why not.
bool hook_front_window(obs_source_t *capture, std::string &what, std::string &error);

// For the tests: the window to take as the one in front (an empty
// executable goes back to the real ones).
void set_front_window_for_tests(const std::string &title, const std::string &window_class,
				const std::string &executable);

// Stops the thread that keeps the list of open windows. For when the plugin
// is unloaded.
void stop_window_watch();

} // namespace stagekit
