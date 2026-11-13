/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#pragma once

class QWidget;

namespace stagekit {

// Guide lines over OBS's preview: where an item being dragged lines up with
// the screen or another item, and where a line-up tool has just put things.
//
// OBS has no way for a plugin to draw in its preview. The lines are drawn in
// a see-through window of our own that sits exactly over the preview, lets
// every click through and only exists while there is a line to show.

// Where OBS draws the canvas inside its preview while the preview is set to
// "Scale to Window": the canvas's scale and its corner, in the widget's own
// (device-independent) pixels.
struct PreviewMap {
	double scale = 1.0, x = 0.0, y = 0.0;
};
PreviewMap preview_map(double widget_w, double widget_h, double pixel_ratio, double canvas_w, double canvas_h);

// Starts watching, on the UI thread. `preview` is OBS's preview widget; when
// it is null it is looked for in `main_window` by the name OBS gives it.
// Returns the overlay window (hidden until there is something to show).
QWidget *install_preview_guides(QWidget *main_window, QWidget *preview = nullptr);

// Whether the left mouse button is held. The tests, which have no mouse, can
// say that it is.
bool guides_mouse_down();
void guides_test_mouse(bool down);

} // namespace stagekit
