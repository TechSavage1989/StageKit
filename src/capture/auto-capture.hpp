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
	std::string window_class;
	// Which window this is, while it is open (on Windows its handle): two
	// windows of one program, such as two browser profiles, are told apart
	// by it. 0 where it is not known.
	uint64_t handle = 0;
};
// Every open window, everyday programs included. Looked up at most five
// times a second however many callers ask.
std::vector<OpenWindow> open_windows();

// Aims a Game Capture or a Window Capture at the window in front right now,
// for a key that hooks a capture by hand. An Auto Capture on that source is
// switched off, or it would aim the capture somewhere else again a moment
// later; switching it back on hands the capture back to it. On success
// `what` is the program taken; otherwise `error` says why not.
// With `transition` (an OBS transition's id) and `transition_ms`, every item
// showing the capture is hidden with that transition, the capture is aimed
// while it is out of sight, and the items come back with it. "none" or an
// empty id changes over at once.
bool hook_front_window(obs_source_t *capture, std::string &what, std::string &error,
		       const std::string &transition = std::string(), uint32_t transition_ms = 0);

// Aims a Window Capture (or a Game Capture) at a window, at once. The capture
// finds it by its program's file name, or with `by_title` by its title, for
// two programs that share a file name.
void aim_capture_at(obs_source_t *capture, const OpenWindow &window, bool by_title);

// Aims a Window Capture (or a Game Capture) at no window: a program no
// window has, so OBS's capture takes nothing, not even another window of the
// program it was aimed at last. And whether a capture is aimed so.
void aim_capture_at_nothing(obs_source_t *capture);
bool capture_aimed_at_nothing(obs_source_t *capture);

// For the tests: the window to take as the one in front (an empty
// executable goes back to the real ones).
void set_front_window_for_tests(const std::string &title, const std::string &window_class,
				const std::string &executable);

// Stops the thread that keeps the list of open windows. For when the plugin
// is unloaded.
void stop_window_watch();

} // namespace stagekit
