/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "dock-canvas.hpp"

#include "../common/scene-util.hpp"
#include "../zoom/ramp-zoom.hpp"

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace stagekit {

namespace {

constexpr int kPictureWidth = 480; // the picture is drawn no wider than this
constexpr double kHandle = 9.0;    // how close, in widget pixels, counts as "on the cross"

struct Entry {
	obs_sceneitem_t *item;
	Box box;
};

// Every visible item with its box in scene coordinates, bottom-most first,
// items inside groups included.
std::vector<Entry> entries_of(obs_source_t *scene_source)
{
	struct Collect {
		obs_source_t *scene;
		std::vector<Entry> *out;
	};
	std::vector<Entry> out;
	obs_scene_t *scene = scene_of(scene_source);
	if (!scene)
		return out;
	static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
		auto *c = static_cast<Collect *>(param);
		if (obs_sceneitem_visible(item) && !obs_sceneitem_is_group(item))
			c->out->push_back({item, scene_box(c->scene, item)});
		return true;
	};
	Collect collect{scene_source, &out};
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *s, obs_sceneitem_t *item, void *param) {
			if (obs_sceneitem_is_group(item)) {
				if (obs_sceneitem_visible(item))
					obs_sceneitem_group_enum_items(item, add, param);
			} else {
				add(s, item, param);
			}
			return true;
		},
		&collect);
	return out;
}

// The first Ramp Zoom filter on a source. The caller releases it.
obs_source_t *zoom_filter_of(obs_source_t *source)
{
	obs_source_t *found = nullptr;
	if (!source)
		return nullptr;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *param) {
			auto **out = static_cast<obs_source_t **>(param);
			const char *id = obs_source_get_unversioned_id(filter);
			if (!*out && id && strcmp(id, kRampZoomId) == 0)
				*out = obs_source_get_ref(filter);
		},
		&found);
	return found;
}

void set_filter_doubles(obs_source_t *filter, std::initializer_list<std::pair<const char *, double>> values)
{
	obs_data_t *settings = obs_source_get_settings(filter);
	for (const auto &[name, value] : values)
		obs_data_set_double(settings, name, value);
	obs_source_update(filter, settings);
	obs_data_release(settings);
}

// Moves an item so its box's top-left is at (x, y) in scene coordinates.
void move_box_to(obs_source_t *scene, obs_sceneitem_t *item, double x, double y)
{
	const Box now = scene_box(scene, item);
	const Space space = item_space(scene, item);
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += (float)((x - now.x) / space.sx);
	pos.y += (float)((y - now.y) / space.sy);
	obs_sceneitem_set_pos(item, &pos);
}

} // namespace

DockCanvas::DockCanvas(QWidget *parent) : QWidget(parent)
{
	setMinimumHeight(150);
	setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	setFixedHeight(190);
	setMouseTracking(false);
	setCursor(Qt::OpenHandCursor);
	timer_ = new QTimer(this);
	QObject::connect(timer_, &QTimer::timeout, this, [this] {
		if (isVisible())
			update_picture();
	});
	timer_->start(250);
}

DockCanvas::~DockCanvas()
{
	watch(nullptr);
}

// Keeps the scene on show counted as "showing", so captures in a scene that
// is not on air still produce a picture.
void DockCanvas::watch(obs_source_t *scene_source)
{
	obs_source_t *old = shown_ ? obs_weak_source_get_source(shown_) : nullptr;
	if (old == scene_source) {
		obs_source_release(old);
		return;
	}
	if (old) {
		obs_source_dec_showing(old);
		obs_source_release(old);
	}
	obs_weak_source_release(shown_);
	shown_ = nullptr;
	if (scene_source) {
		obs_source_inc_showing(scene_source);
		shown_ = obs_source_get_weak_source(scene_source);
	}
}

// Mouse positions are worked out from the scene's size, so it is read
// whenever the scene is in hand, not only when the picture is redrawn.
void DockCanvas::measure_scene(obs_source_t *scene_source)
{
	const uint32_t w = scene_source ? obs_source_get_width(scene_source) : 0;
	const uint32_t h = scene_source ? obs_source_get_height(scene_source) : 0;
	if (w && h) {
		scene_w_ = w;
		scene_h_ = h;
	}
}

void DockCanvas::set_live_picture(bool live)
{
	live_ = live;
	if (!live_) {
		picture_ = QImage();
		watch(nullptr);
	}
	update_picture();
}

void DockCanvas::update_picture()
{
	obs_source_t *scene_source = scene ? scene() : nullptr;
	if (!scene_source) {
		picture_ = QImage();
		watch(nullptr);
		update();
		return;
	}
	const uint32_t sw = obs_source_get_width(scene_source), sh = obs_source_get_height(scene_source);
	measure_scene(scene_source);
	if (live_ && sw && sh) {
		watch(scene_source);
		const uint32_t w = (uint32_t)std::min<int>(kPictureWidth, (int)sw);
		const uint32_t h = std::max<uint32_t>(1, (uint32_t)std::lround((double)w * sh / sw));
		QImage image((int)w, (int)h, QImage::Format_RGBA8888);
		bool drawn = false;
		obs_enter_graphics();
		gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
		if (gs_texrender_begin(render, w, h)) {
			vec4 clear;
			vec4_set(&clear, 0.0f, 0.0f, 0.0f, 1.0f);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
			obs_source_video_render(scene_source);
			gs_blend_state_pop();
			gs_texrender_end(render);
			gs_stage_texture(stage, gs_texrender_get_texture(render));
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(stage, &data, &linesize)) {
				for (uint32_t y = 0; y < h; y++)
					memcpy(image.scanLine((int)y), data + (size_t)y * linesize, (size_t)w * 4);
				gs_stagesurface_unmap(stage);
				drawn = true;
			}
		}
		gs_stagesurface_destroy(stage);
		gs_texrender_destroy(render);
		obs_leave_graphics();
		picture_ = drawn ? image : QImage();
	}
	obs_source_release(scene_source);
	update();
}

QRectF DockCanvas::picture_area() const
{
	const double scale = std::min(width() / scene_w_, height() / scene_h_);
	const double w = scene_w_ * scale, h = scene_h_ * scale;
	return QRectF((width() - w) / 2.0, (height() - h) / 2.0, w, h);
}

QPointF DockCanvas::to_scene(const QPointF &p) const
{
	const QRectF area = picture_area();
	const double scale = area.width() / scene_w_;
	return QPointF((p.x() - area.x()) / scale, (p.y() - area.y()) / scale);
}

QPointF DockCanvas::to_widget(double x, double y) const
{
	const QRectF area = picture_area();
	const double scale = area.width() / scene_w_;
	return QPointF(area.x() + x * scale, area.y() + y * scale);
}

void DockCanvas::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	obs_source_t *scene_source = scene ? scene() : nullptr;
	measure_scene(scene_source);
	const QRectF area = picture_area();
	painter.fillRect(area, QColor(16, 16, 16));
	if (!picture_.isNull())
		painter.drawImage(area, picture_);
	if (!scene_source)
		return;
	const std::string chosen = selected ? selected().toUtf8().toStdString() : std::string();
	painter.setClipRect(area);
	for (const Entry &entry : entries_of(scene_source)) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(entry.item));
		const bool is_chosen = name && chosen == name;
		// Without a picture every item gets an outline; with one, only the chosen item.
		if (!is_chosen && !picture_.isNull())
			continue;
		const QRectF box(to_widget(entry.box.x, entry.box.y),
				 to_widget(entry.box.x + entry.box.w, entry.box.y + entry.box.h));
		painter.setPen(QPen(is_chosen ? QColor(80, 170, 255) : QColor(150, 150, 150), is_chosen ? 2.0 : 1.0));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(box);
		if (!is_chosen)
			continue;
		// The point a Ramp Zoom on this item zooms in on.
		if (obs_source_t *zoom = zoom_filter_of(obs_sceneitem_get_source(entry.item))) {
			obs_data_t *settings = obs_source_get_settings(zoom);
			const QPointF point = to_widget(
				entry.box.x + entry.box.w * obs_data_get_double(settings, "center_x") / 100.0,
				entry.box.y + entry.box.h * obs_data_get_double(settings, "center_y") / 100.0);
			obs_data_release(settings);
			obs_source_release(zoom);
			painter.setPen(QPen(QColor(0, 0, 0), 4.0));
			painter.drawLine(point - QPointF(8, 0), point + QPointF(8, 0));
			painter.drawLine(point - QPointF(0, 8), point + QPointF(0, 8));
			painter.setPen(QPen(QColor(255, 210, 60), 2.0));
			painter.drawLine(point - QPointF(8, 0), point + QPointF(8, 0));
			painter.drawLine(point - QPointF(0, 8), point + QPointF(0, 8));
			painter.drawEllipse(point, 5.0, 5.0);
		}
	}
	obs_source_release(scene_source);
}

void DockCanvas::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	obs_source_t *scene_source = scene ? scene() : nullptr;
	if (!scene_source)
		return;
	measure_scene(scene_source);
	const QPointF at = to_scene(event->position());
	const std::string chosen = selected ? selected().toUtf8().toStdString() : std::string();
	const std::vector<Entry> entries = entries_of(scene_source);
	drag_ = Drag::None;

	// The zoom cross of the chosen item comes first: it sits on top of it.
	for (const Entry &entry : entries) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(entry.item));
		if (!name || chosen != name)
			continue;
		if (obs_source_t *zoom = zoom_filter_of(obs_sceneitem_get_source(entry.item))) {
			obs_data_t *settings = obs_source_get_settings(zoom);
			const QPointF point = to_widget(
				entry.box.x + entry.box.w * obs_data_get_double(settings, "center_x") / 100.0,
				entry.box.y + entry.box.h * obs_data_get_double(settings, "center_y") / 100.0);
			obs_data_release(settings);
			obs_source_release(zoom);
			const QPointF away = point - event->position();
			if (std::hypot(away.x(), away.y()) <= kHandle) {
				drag_ = Drag::ZoomPoint;
				drag_item_ = QString::fromUtf8(name);
			}
		}
	}
	// Otherwise the top-most item under the mouse.
	if (drag_ == Drag::None) {
		for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
			const Box &box = entry->box;
			if (at.x() < box.x || at.y() < box.y || at.x() > box.x + box.w || at.y() > box.y + box.h)
				continue;
			const char *name = obs_source_get_name(obs_sceneitem_get_source(entry->item));
			if (!name || !*name)
				continue;
			drag_item_ = QString::fromUtf8(name);
			if (on_pick)
				on_pick(drag_item_);
			if (!obs_sceneitem_locked(entry->item)) {
				drag_ = Drag::Item;
				grab_offset_ = QPointF(at.x() - box.x, at.y() - box.y);
				setCursor(Qt::ClosedHandCursor);
			}
			break;
		}
	}
	obs_source_release(scene_source);
	update();
}

void DockCanvas::mouseMoveEvent(QMouseEvent *event)
{
	if (drag_ == Drag::None)
		return;
	obs_source_t *scene_source = scene ? scene() : nullptr;
	obs_sceneitem_t *item = find_item(scene_source, drag_item_.toUtf8().toStdString());
	if (item) {
		const QPointF at = to_scene(event->position());
		if (drag_ == Drag::Item) {
			move_box_to(scene_source, item, at.x() - grab_offset_.x(), at.y() - grab_offset_.y());
		} else if (obs_source_t *zoom = zoom_filter_of(obs_sceneitem_get_source(item))) {
			const Box box = scene_box(scene_source, item);
			if (box.w > 0.0 && box.h > 0.0)
				set_filter_doubles(
					zoom, {{"center_x", std::clamp((at.x() - box.x) / box.w * 100.0, 0.0, 100.0)},
					       {"center_y", std::clamp((at.y() - box.y) / box.h * 100.0, 0.0, 100.0)}});
			obs_source_release(zoom);
		}
	}
	obs_source_release(scene_source);
	update();
}

void DockCanvas::mouseReleaseEvent(QMouseEvent *)
{
	drag_ = Drag::None;
	setCursor(Qt::OpenHandCursor);
	update_picture();
}

void DockCanvas::wheelEvent(QWheelEvent *event)
{
	const double steps = event->angleDelta().y() / 120.0;
	obs_source_t *scene_source = scene ? scene() : nullptr;
	const std::string chosen = selected ? selected().toUtf8().toStdString() : std::string();
	measure_scene(scene_source);
	obs_sceneitem_t *item = find_item(scene_source, chosen);
	if (!item || steps == 0.0 || obs_sceneitem_locked(item)) {
		obs_source_release(scene_source);
		event->ignore(); // let the dock scroll
		return;
	}
	if (event->modifiers() & Qt::ControlModifier) {
		// How far its Ramp Zoom zooms in.
		if (obs_source_t *zoom = zoom_filter_of(obs_sceneitem_get_source(item))) {
			obs_data_t *settings = obs_source_get_settings(zoom);
			const double amount = obs_data_get_double(settings, "zoom");
			obs_data_release(settings);
			set_filter_doubles(zoom, {{"zoom", std::clamp(amount + steps * 10.0, 100.0, 800.0)}});
			obs_source_release(zoom);
		}
	} else {
		// Bigger or smaller, about its own centre.
		refresh_item(item);
		const Box before = scene_box(scene_source, item);
		const float factor = (float)std::pow(1.05, steps);
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE) {
			vec2 bounds;
			obs_sceneitem_get_bounds(item, &bounds);
			vec2_mulf(&bounds, &bounds, factor);
			obs_sceneitem_set_bounds(item, &bounds);
		} else {
			vec2 scale;
			obs_sceneitem_get_scale(item, &scale);
			vec2_mulf(&scale, &scale, factor);
			obs_sceneitem_set_scale(item, &scale);
		}
		const Box after = scene_box(scene_source, item);
		move_box_to(scene_source, item, before.x + (before.w - after.w) / 2.0,
			    before.y + (before.h - after.h) / 2.0);
	}
	obs_source_release(scene_source);
	event->accept();
	update_picture();
}

} // namespace stagekit
