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

// For the scene collection's save data.
void start_image_save(obs_data_t *save_data);
void start_image_load(obs_data_t *save_data);

// Registers the "Start Image (StageKit)" source and a global
// "stagekit_set_start_image" procedure.
void register_start_image();

} // namespace stagekit
