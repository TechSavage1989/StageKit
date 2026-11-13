/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "preview-guides.hpp"

#include "arrange-tools.hpp"
#include "arrange.hpp"

#include <obs-module.h>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

namespace stagekit {

PreviewMap preview_map(double widget_w, double widget_h, double pixel_ratio, double canvas_w, double canvas_h)
{
	// The same sums OBS does for its preview: ten pixels of edge all round,
	// then the canvas as large as fits, centred. OBS works in real pixels and
	// in whole numbers; the result is given back in the widget's own units.
	constexpr int kEdge = 10;
	PreviewMap map;
	const double ratio = pixel_ratio > 0.0 ? pixel_ratio : 1.0;
	const int room_w = (int)std::lround(widget_w * ratio) - kEdge * 2;
	const int room_h = (int)std::lround(widget_h * ratio) - kEdge * 2;
	if (room_w <= 0 || room_h <= 0 || canvas_w <= 0.0 || canvas_h <= 0.0)
		return map;
	const double canvas_shape = canvas_w / canvas_h;
	double scale;
	int shown_w, shown_h;
	if ((double)room_w / (double)room_h > canvas_shape) {
		scale = (double)room_h / canvas_h;
		shown_w = (int)((double)room_h * canvas_shape);
		shown_h = room_h;
	} else {
		scale = (double)room_w / canvas_w;
		shown_w = room_w;
		shown_h = (int)((double)room_w / canvas_shape);
	}
	map.scale = scale / ratio;
	map.x = (double)(kEdge + room_w / 2 - shown_w / 2) / ratio;
	map.y = (double)(kEdge + room_h / 2 - shown_h / 2) / ratio;
	return map;
}

namespace {
std::atomic<bool> g_test_mouse{false};
} // namespace

bool guides_mouse_down()
{
	return g_test_mouse.load() || QGuiApplication::mouseButtons().testFlag(Qt::LeftButton);
}

void guides_test_mouse(bool down)
{
	g_test_mouse = down;
}

namespace {

// How long a line shown by a line-up tool stays up, and how often the
// preview is looked at while guides are switched on.
constexpr int kFlashMs = 900;
constexpr int kWatchMs = 33;

class GuideOverlay : public QWidget {
public:
	GuideOverlay(QWidget *main_window, QWidget *preview)
		: QWidget(main_window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
					       Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint),
		  main_(main_window),
		  preview_(preview)
	{
		setObjectName("stagekitGuides");
		setAttribute(Qt::WA_TranslucentBackground);
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setFocusPolicy(Qt::NoFocus);
		seen_serial_ = last_arrange_guide().serial;
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { watch(); });
		timer->start(kWatchMs);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QWidget *preview = preview_.data();
		if (!preview || canvas_w_ <= 0.0 || canvas_h_ <= 0.0)
			return;
		const PreviewMap map = preview_map(preview->width(), preview->height(), preview->devicePixelRatioF(),
						   canvas_w_, canvas_h_);
		QPainter painter(this);
		// A bright line with a dark one under it, so it shows on any picture.
		for (int pass = 0; pass < 2; pass++) {
			painter.setPen(pass == 0 ? QPen(QColor(0, 0, 0, 170), 4.0) : QPen(QColor(0, 230, 255), 2.0));
			for (double x : xs_) {
				const double at = map.x + x * map.scale;
				painter.drawLine(QPointF(at, map.y), QPointF(at, map.y + canvas_h_ * map.scale));
			}
			for (double y : ys_) {
				const double at = map.y + y * map.scale;
				painter.drawLine(QPointF(map.x, at), QPointF(map.x + canvas_w_ * map.scale, at));
			}
		}
	}

private:
	void watch()
	{
		const ArrangeOptions options = arrange_options();
		QWidget *preview = find_preview();
		if (!options.guides || !preview || !preview->isVisible()) {
			// Nothing a tool did while guides were off is shown later.
			seen_serial_ = last_arrange_guide().serial;
			flash_timer_.invalidate();
			dragging_ = false;
			clear();
			return;
		}
		obs_source_t *scene = editing_scene();
		if (!scene) {
			clear();
			return;
		}
		std::vector<double> xs, ys;

		// A line-up tool has just run: show the line it used for a moment.
		const ArrangeGuide guide = last_arrange_guide();
		if (guide.serial != seen_serial_) {
			seen_serial_ = guide.serial;
			flash_ = guide;
			flash_timer_.start();
		}
		if (flash_timer_.isValid() && flash_timer_.elapsed() < kFlashMs)
			(flash_.vertical ? xs : ys).push_back(flash_.at);
		else
			flash_timer_.invalidate();

		// An item being dragged with the mouse: the lines it is within reach of.
		const std::vector<obs_sceneitem_t *> items = selected_items(scene);
		std::vector<std::pair<obs_sceneitem_t *, Box>> now;
		for (obs_sceneitem_t *item : items)
			now.push_back({item, scene_box(scene, item)});
		const bool same = now.size() == before_.size() &&
				  std::equal(now.begin(), now.end(), before_.begin(),
					     [](const auto &a, const auto &b) { return a.first == b.first; });
		bool moved = false;
		for (size_t i = 0; same && i < now.size(); i++)
			moved = moved || std::fabs(now[i].second.x - before_[i].second.x) > 0.01 ||
				std::fabs(now[i].second.y - before_[i].second.y) > 0.01;
		const bool button_down = guides_mouse_down();
		if (!button_down || !same) {
			dragging_ = false;
		} else if (moved && !dragging_) {
			// The drag starts: every item is measured once, here, and not
			// again until the next drag.
			dragging_ = true;
			lines_ = snap_lines(scene, items, options.how);
			insets_.clear();
			for (const auto &[item, box] : now) {
				const Box seen = measure_item(scene, item, options.how);
				insets_.push_back({seen.x - box.x, seen.y - box.y, seen.w - box.w, seen.h - box.h});
			}
		}
		if (dragging_ && insets_.size() == now.size()) {
			for (size_t i = 0; i < now.size(); i++) {
				// What is drawn in the item, from where its box is now.
				const Box &box = now[i].second;
				const Box seen{box.x + insets_[i].x, box.y + insets_[i].y, box.w + insets_[i].w,
					       box.h + insets_[i].h};
				const Meeting meeting = meet_lines(seen, lines_, options.within);
				if (meeting.across)
					xs.push_back(meeting.line_x);
				if (meeting.down)
					ys.push_back(meeting.line_y);
			}
		}
		before_ = now;

		canvas_w_ = obs_source_get_width(scene);
		canvas_h_ = obs_source_get_height(scene);
		obs_source_release(scene);
		if (xs.empty() && ys.empty()) {
			clear();
			return;
		}
		// Exactly over the preview, wherever OBS's window has put it.
		const QRect over(preview->mapToGlobal(QPoint(0, 0)), preview->size());
		if (geometry() != over)
			setGeometry(over);
		if (xs != xs_ || ys != ys_) {
			xs_ = xs;
			ys_ = ys;
			// Also kept as text on the window, for the tests to read.
			QString text;
			for (double x : xs_)
				text += QStringLiteral("x:%1 ").arg(std::lround(x));
			for (double y : ys_)
				text += QStringLiteral("y:%1 ").arg(std::lround(y));
			setProperty("guideLines", text.trimmed());
			update();
		}
		if (!isVisible())
			show();
	}

	void clear()
	{
		xs_.clear();
		ys_.clear();
		setProperty("guideLines", QString());
		if (isVisible())
			hide();
	}

	QWidget *find_preview()
	{
		if (!preview_ && main_)
			preview_ = main_->findChild<QWidget *>(QStringLiteral("preview"));
		return preview_.data();
	}

	QPointer<QWidget> main_;
	QPointer<QWidget> preview_;
	std::vector<double> xs_, ys_; // the lines showing, in canvas pixels
	double canvas_w_ = 0.0, canvas_h_ = 0.0;
	uint64_t seen_serial_ = 0;
	ArrangeGuide flash_;
	QElapsedTimer flash_timer_;
	bool dragging_ = false;
	std::vector<std::pair<obs_sceneitem_t *, Box>> before_; // compared, never used as items
	SnapLines lines_;
	std::vector<Box> insets_; // what is drawn, relative to each dragged item's box
};

} // namespace

QWidget *install_preview_guides(QWidget *main_window, QWidget *preview)
{
	return new GuideOverlay(main_window, preview);
}

} // namespace stagekit
