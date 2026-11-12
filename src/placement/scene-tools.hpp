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

inline constexpr const char *kShowHideId = "stagekit_show_hide";
inline constexpr const char *kMatchItemId = "stagekit_match_item";
inline constexpr const char *kLayoutId = "stagekit_layout";

// Scene filters:
//   Show/Hide Animation  slides or pops an item in and out when it is shown
//                        or hidden
//   Match Item           keeps an item lined up with an item in another scene
//   Layout               saves an arrangement of the scene's items and
//                        glides back to it on demand
void register_show_hide();
void register_match_item();
void register_layout();

} // namespace stagekit
