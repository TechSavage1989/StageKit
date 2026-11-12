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

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QWidget>

#include <functional>

class QTimer;

namespace stagekit {

// A small picture of a scene to work on directly:
//
//   - click an item to select it, drag it to move it
//   - turn the mouse wheel over it to make it bigger or smaller
//   - if the item has a Ramp Zoom filter, drag the cross to set the point it
//     zooms in on, and hold Ctrl while turning the wheel to set how far
//
// The picture is drawn a few times a second and only while the widget can
// be seen. It can be switched off, leaving outlines of the items.
class DockCanvas : public QWidget {
public:
	explicit DockCanvas(QWidget *parent = nullptr);
	~DockCanvas() override;

	// Which scene to show (returns a new reference, or nullptr) and which
	// item is selected. Set by the dock.
	std::function<obs_source_t *()> scene;
	std::function<QString()> selected;
	// Called when an item is clicked.
	std::function<void(const QString &)> on_pick;

	void set_live_picture(bool live);
	// Draws the scene into the picture now (the timer does this by itself).
	void update_picture();
	// Where the scene sits inside the widget.
	QRectF picture_area() const;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

private:
	enum class Drag { None, Item, ZoomPoint };

	QPointF to_scene(const QPointF &widget_point) const;
	QPointF to_widget(double x, double y) const;
	void watch(obs_source_t *scene_source);
	void measure_scene(obs_source_t *scene_source);

	QTimer *timer_ = nullptr;
	QImage picture_;
	bool live_ = true;
	double scene_w_ = 1920.0, scene_h_ = 1080.0;
	Drag drag_ = Drag::None;
	QString drag_item_;
	QPointF grab_offset_;                // from the item's top-left to the mouse, in scene pixels
	obs_weak_source_t *shown_ = nullptr; // the scene kept "showing" so its captures run
};

} // namespace stagekit
