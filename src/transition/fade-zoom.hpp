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

inline constexpr const char *kFadeZoomId = "stagekit_fade_zoom";

// A transition: what goes fades out while it shrinks a little, what comes
// fades in while it grows to its size. For a change of scene, and for one
// item shown or hidden (OBS's own show and hide transition, and the "Show
// with" lists of StageKit's filters).
void register_fade_zoom();

} // namespace stagekit
