/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// A filter on each source, kept by a filter on the scene. Scene Standby
// puts a Standby Image filter on each capture it covers, Scene Corners a
// Corners & Edges filter on each source it covers: made when missing, its
// look pushed from the scene filter, and taken off again once no scene
// filter of that kind covers the source. One set of rules for both:
//
// * A filter made this way says so in its settings: `managed` (true) and
//   `managed_by` (the scene's name). Both are a source's own settings for
//   the Copy tab (`kOwnSettings`).
// * It takes its look only from the scene filter in the scene it names. When
//   that one is gone, the first other scene filter of the kind covering the
//   source (by scene name) takes it over and writes its own scene's name.
// * A filter of the kind the user made (no `managed`) is left alone: no
//   second one is added.
// * One is taken off only from a scene filter's tick, after its source has
//   been covered by no scene filter of the kind for `kManagedGraceS`, never
//   when a scene filter is destroyed (which also happens when OBS changes
//   scene collection or closes).
// * With `mirror_enabled`, the scene filter is the switch: switching it off
//   switches its filters off, and on again. Without it a filter switched off
//   by hand stays off.
//
// Nothing here holds a scene item or a source past one call: scenes and
// sources are found again by name or weak reference each time.

#pragma once

#include <obs.h>

#include <cstddef>
#include <map>
#include <set>
#include <string>

namespace stagekit {

// How long a source may be covered by no scene filter before its filter is
// taken off.
inline constexpr double kManagedGraceS = 3.0;

enum class LookType { Int, Double, Bool, String };

struct LookKey {
	const char *name;
	LookType type;
};

struct ManagedSpec {
	const char *scene_kind;  // the scene filter's id (the registry is per kind)
	const char *filter_kind; // the per-source filter's id
	const char *name_key;    // locale key: the name a new per-source filter gets
	const char *log_name;    // "Scene Standby", for the log
	const LookKey *look;     // the settings pushed to each filter
	size_t look_count;
	bool mirror_enabled; // the scene filter switches its filters off and on
};

// What the registry knows about one source, for its filter.
struct ManagedState {
	bool listed = false;      // some scene filter of the kind keeps a filter on it (enabled or not)
	bool covered = false;     // some enabled one covers it (`covered` below)
	std::string owner_scene;  // the scene of the one it takes its look from, if any
	bool owner_found = false; // that one is there
};

// One scene filter's part. Owned by the scene filter; every call from its
// own tick, `withdraw` from its destroy or filter_remove.
class ManagedSet {
public:
	ManagedSet(const ManagedSpec &spec, obs_source_t *scene_filter) : spec_(spec), self_(scene_filter) {}
	~ManagedSet() { withdraw(); }

	// Says what this scene filter covers now. `kept` are the sources it keeps
	// a filter on; `covered` those of them for which it is in force (Scene
	// Standby: the ones that show the image when empty). `look` holds every
	// look key, as the per-source filters are to have it.
	void publish(obs_source_t *scene, bool enabled, const std::set<std::string> &kept,
		     const std::set<std::string> &covered, obs_data_t *look);

	// Makes, adopts, pushes to and takes off the per-source filters. `now`
	// is in seconds, any clock. Filters to take off are looked for on the
	// scene's own items and on the sources this scene filter kept before.
	void sync(obs_source_t *scene, double now);

	// Takes this scene filter out of the registry.
	void withdraw();

	// The status line's parts: the sources kept, and those that already had
	// a filter of the kind of the user's own.
	const std::set<std::string> &kept() const { return kept_; }
	const std::set<std::string> &own_ones() const { return own_ones_; }

private:
	const ManagedSpec &spec_;
	obs_source_t *self_;
	std::set<std::string> kept_, own_ones_, seen_;
	std::map<std::string, double> uncovered_since_;
	int last_enabled_ = -1;
};

// Is the source covered by a scene filter of the kind, and which one sets its
// filter's look? `managed_by` is what its filter says. Safe from any thread.
ManagedState managed_state(const char *scene_kind, obs_source_t *source, const char *managed_by);

// The scene names of the scene filters of the kind that keep a filter on the
// source. Safe from any thread.
std::set<std::string> managed_scenes(const char *scene_kind, const std::string &source);

// Puts "%1" in a locale text.
std::string text_with(const char *key, const std::string &value);

} // namespace stagekit
