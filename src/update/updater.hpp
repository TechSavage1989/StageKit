/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

#include <functional>
#include <string>

namespace stagekit {

// Updates from inside OBS: the plugin looks at the project's releases on
// GitHub, says in the dock when there is a newer one, and on one press
// downloads it, checks it, and installs it (which closes and reopens OBS).
// Looking works wherever the plugin can reach the web; installing is for
// Windows, where there is an installer to run.

struct Release {
	std::string version;       // "0.1.0-beta82"
	std::string page;          // the release's page, for "What's new"
	std::string installer_url; // the Windows installer
	std::string installer_sha; // its SHA-256, from the release notes
	std::string deck_url;      // the Stream Deck plugin's file, if the release has one
	std::string deck_sha;
};

// The release this build is: the newest notes file there was when it was built.
const char *current_release();

// Orders two versions ("0.1.0-beta9" before "0.1.0-beta10" before "0.1.0-rc1"
// before "0.1.0"): below zero, zero or above zero.
int compare_versions(const std::string &a, const std::string &b);

// The newest release in GitHub's list (its "releases" answer, as text) that
// has a Windows installer with a checksum in its notes. With `betas` off,
// pre-releases are passed over. False when there is none.
bool newest_release(const std::string &json, bool betas, Release &out);

// The settings, kept with the plugin (not with a scene collection).
struct UpdateSettings {
	bool check_at_start = true;
	bool betas = true;
};
UpdateSettings update_settings();
void set_update_settings(const UpdateSettings &settings);

// Looks for a newer release without holding anything up, and calls back (on
// another thread) with what it found: `found` is false when the releases
// could not be read; `out.version` is empty when this build is the newest.
void check_for_update(std::function<void(bool found, const Release &out)> done);

// Can this build install an update itself? (Windows only.)
bool can_install_updates();

// Downloads the release, checks it, closes OBS, installs and reopens OBS,
// all from a helper that outlives OBS. False, with a reason to show, when it
// could not be started.
bool install_update(const Release &release, std::string &error);

// How the install's helper is doing, for the dock to say when it gave up:
// one that works closes OBS before it ends. Its steps are written to
// %TEMP%\\StageKitUpdate\\update.log.
enum class UpdateState { None, Running, Stopped };
UpdateState update_state();

// The script the install runs (see updater.cpp); here for the tests.
std::string install_script(const Release &release, const std::string &obs_exe);

// For when the plugin is unloaded: waits for a look that is under way.
void stop_updater();

void register_updater_test_hooks();

} // namespace stagekit
