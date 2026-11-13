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
#include <plugin-support.h>

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
std::atomic<int> g_preview_state{0};
} // namespace

int guides_preview_state()
{
	return g_preview_state.load();
}

bool guides_mouse_down()
{
	if (g_test_mouse.load() || QGuiApplication::mouseButtons().testFlag(Qt::LeftButton))
		return true;
#ifdef _WIN32
	// Also asked of Windows itself: OBS's preview is a window of its own,
	// and Windows knows about the button whichever window has the mouse.
	// It reports the physical button, so a swapped mouse is allowed for.
	const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
	return (GetAsyncKeyState(button) & 0x8000) != 0;
#else
	return false;
#endif
}

void guides_test_mouse(bool down)
{
	g_test_mouse = down;
}

namespace {

// How long a line shown by a line-up tool stays up, how long the frame
// shown when guides are switched on stays up, and how often the preview is
// looked at while guides are switched on.
constexpr int kFlashMs = 900;
constexpr int kHelloMs = 1500;
constexpr int kWatchMs = 33;

// One guide line: a thin window of its own, laid over OBS's preview. It is
// opaque, a bright line between two dark ones, so it needs nothing from
// Windows' see-through windows and shows over the preview's own drawing
// whatever the graphics card. Clicks go through it.
class LineWindow : public QWidget {
public:
	explicit LineWindow(QWidget *owner)
		// Not Qt::WindowTransparentForInput: on Windows that makes it a
		// layered window, the one thing these lines must not depend on.
		// The window is a few pixels wide, shows only while a line is up,
		// and lets mouse events through to the widgets beneath in Qt.
		: QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
					 Qt::NoDropShadowWindowHint)
	{
		setObjectName("stagekitGuideLine");
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_ShowWithoutActivating);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setFocusPolicy(Qt::NoFocus);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), QColor(0, 0, 0));
		// The bright middle, along the line's length.
		const QRect middle = width() >= height() ? rect().adjusted(0, 1, 0, -1) : rect().adjusted(1, 0, -1, 0);
		painter.fillRect(middle, QColor(0, 230, 255));
	}
};

// Watches the preview and puts a LineWindow where each line belongs.
class GuideOverlay : public QObject {
public:
	GuideOverlay(QWidget *main_window, QWidget *preview) : QObject(nullptr), main_(main_window), preview_(preview)
	{
		setObjectName("stagekitGuides");
		seen_serial_ = last_arrange_guide().serial;
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { watch(); });
		timer->start(kWatchMs);
	}

	~GuideOverlay() override
	{
		for (QPointer<LineWindow> &line : lines_shown_)
			delete line.data();
	}

private:
	void watch()
	{
		const ArrangeOptions options = arrange_options();
		QWidget *preview = find_preview();
		if (options.guides != said_on_) {
			said_on_ = options.guides;
			if (options.guides && preview) {
				obs_log(LOG_INFO, "Guides: on, over OBS's preview (%d x %d)", preview->width(),
					preview->height());
				// A frame round the canvas for a moment, so it is plain at
				// once that lines can show over the preview.
				hello_.start();
				said_line_ = false;
			} else if (options.guides) {
				obs_log(LOG_WARNING, "Guides: on, but OBS's preview was not found");
			}
		}
		g_preview_state = !options.guides ? 0 : preview ? 1 : 2;
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
		const double canvas_w = obs_source_get_width(scene);
		const double canvas_h = obs_source_get_height(scene);
		if (hello_.isValid() && hello_.elapsed() < kHelloMs) {
			xs.insert(xs.end(), {0.0, canvas_w});
			ys.insert(ys.end(), {0.0, canvas_h});
		} else {
			hello_.invalidate();
		}

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
			if (!said_drag_) {
				said_drag_ = true;
				obs_log(LOG_INFO, "Guides: a drag in the preview was seen (%zu item%s)", now.size(),
					now.size() == 1 ? "" : "s");
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

		obs_source_release(scene);
		if (xs.empty() && ys.empty()) {
			clear();
			return;
		}

		// Where each line goes on the screen: the canvas found inside the
		// preview as OBS places it, wherever OBS's window is.
		const PreviewMap map = preview_map(preview->width(), preview->height(), preview->devicePixelRatioF(),
						   canvas_w, canvas_h);
		const QPoint origin = preview->mapToGlobal(QPoint(0, 0));
		const QRect over(origin, preview->size());
		const int left = origin.x() + (int)std::lround(map.x);
		const int top = origin.y() + (int)std::lround(map.y);
		const int across = (int)std::lround(canvas_w * map.scale);
		const int down = (int)std::lround(canvas_h * map.scale);
		std::vector<QRect> rects;
		for (double x : xs)
			rects.push_back(
				QRect(left + (int)std::lround(x * map.scale) - kThickness / 2, top, kThickness, down));
		for (double y : ys)
			rects.push_back(QRect(left, top + (int)std::lround(y * map.scale) - kThickness / 2, across,
					      kThickness));
		for (QRect &r : rects)
			r = r.intersected(over);

		while (lines_shown_.size() < rects.size())
			lines_shown_.push_back(new LineWindow(main_.data()));
		QString text, placed;
		for (double x : xs)
			text += QStringLiteral("x:%1 ").arg(std::lround(x));
		for (double y : ys)
			text += QStringLiteral("y:%1 ").arg(std::lround(y));
		for (size_t i = 0; i < lines_shown_.size(); i++) {
			LineWindow *line = lines_shown_[i].data();
			if (!line)
				continue;
			if (i < rects.size() && !rects[i].isEmpty()) {
				if (line->geometry() != rects[i])
					line->setGeometry(rects[i]);
				if (!line->isVisible())
					line->show();
				line->raise();
				placed += QStringLiteral("%1,%2,%3,%4 ")
						  .arg(rects[i].x())
						  .arg(rects[i].y())
						  .arg(rects[i].width())
						  .arg(rects[i].height());
			} else if (line->isVisible()) {
				line->hide();
			}
		}
		// Also kept as text, for the tests to read.
		setProperty("guideLines", text.trimmed());
		setProperty("lineWindows", placed.trimmed());
		if (!said_line_) {
			said_line_ = true;
			obs_log(LOG_INFO, "Guides: showing %s on the screen at %s", text.trimmed().toUtf8().constData(),
				placed.trimmed().toUtf8().constData());
		}
	}

	void clear()
	{
		setProperty("guideLines", QString());
		setProperty("lineWindows", QString());
		for (QPointer<LineWindow> &line : lines_shown_)
			if (line && line->isVisible())
				line->hide();
	}

	QWidget *find_preview()
	{
		if (!preview_ && main_)
			preview_ = main_->findChild<QWidget *>(QStringLiteral("preview"));
		return preview_.data();
	}

	static constexpr int kThickness = 4;

	QPointer<QWidget> main_;
	QPointer<QWidget> preview_;
	std::vector<QPointer<LineWindow>> lines_shown_;
	uint64_t seen_serial_ = 0;
	ArrangeGuide flash_;
	QElapsedTimer flash_timer_;
	bool dragging_ = false;
	std::vector<std::pair<obs_sceneitem_t *, Box>> before_; // compared, never used as items
	SnapLines lines_;
	std::vector<Box> insets_; // what is drawn, relative to each dragged item's box
	// What has been written to OBS's log, so each is said once.
	bool said_on_ = false, said_drag_ = false, said_line_ = false;
	QElapsedTimer hello_; // the frame shown when guides are switched on
};

} // namespace

QObject *install_preview_guides(QWidget *main_window, QWidget *preview)
{
	return new GuideOverlay(main_window, preview);
}

} // namespace stagekit
