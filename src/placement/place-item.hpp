/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

namespace stagekit {

inline constexpr const char *kPlaceItemId = "stagekit_place_item";
inline constexpr const char *kSendToId = "stagekit_send_to";

// Registers Place Item (on a scene) and Send To (on the source it moves).
void register_place_item();

} // namespace stagekit
