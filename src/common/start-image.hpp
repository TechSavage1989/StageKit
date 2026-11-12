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

inline constexpr const char *kStartImageId = "stagekit_start_image";

// The start image: one picture, chosen in one place, that the Start Image
// source shows and the Ambient Background can use as its picture. It is
// stored with the scene collection.
std::string start_image_path();
void set_start_image_path(const std::string &path);
// The same, when a person picks the image (the dock, the Start Image source):
// Ambient Backgrounds showing a picture of their own then switch to it.
void choose_start_image(const std::string &path);
// Goes up every time a person picks a start image.
uint64_t start_image_choice();
// Goes up every time the path changes, so users of it can tell cheaply.
uint64_t start_image_revision();

// The moving background's picture, set once for the whole collection: empty
// means "the start image". The zoom and focus are kept at their neutral
// values: the owner wanted zooming in the dock's thumbnail only, not in the
// scene (0.1.0-beta20 and beta21 applied them to the background).
struct AmbientView {
	std::string image;
	double zoom = 1.0;
	double focus_x = 0.5, focus_y = 0.5;
};
AmbientView ambient_view();
// Counts as a change of the start image for everything that follows it.
void set_ambient_view(const AmbientView &view);
// The picture the background shows: its own if one is set, else the start image.
std::string ambient_picture_path();

// For the scene collection's save data.
void start_image_save(obs_data_t *save_data);
void start_image_load(obs_data_t *save_data);

// Registers the "Start Image (StageKit)" source and a global
// "stagekit_set_start_image" procedure.
void register_start_image();

} // namespace stagekit
