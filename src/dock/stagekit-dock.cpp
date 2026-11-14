/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

// The StageKit dock: point-and-click control for the current scene.
//
//   - pick an item, click one of nine positions and it glides there
//   - turn those positions into Place Item filters ("keys") in one click, so
//     a Stream Deck Filter key or a hotkey can trigger them
//   - save the item's current spot as a key
//   - add a StageKit effect to the item without hunting through menus
//   - it follows the item selected in OBS's own preview
//   - the start image, with a thumbnail: turn the wheel on it for a closer
//     look and drag to move around (the thumbnail only, never the scene)
//
// Moves are carried out by Send To filters on the item's own source, so the
// dock and the keys always behave the same way, and a key is a filter on the
// camera itself (what a Stream Deck Filter key points at).

#include "stagekit-dock.hpp"

#include "../update/updater.hpp"

#include "../common/easing.hpp"

#include "../common/scene-util.hpp"
#include "../common/start-image.hpp"
#include "../placement/place-item.hpp"
#include "arrange.hpp"
#include "arrange-tools.hpp"
#include "preview-guides.hpp"
#include "../placement/scene-tools.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QGuiApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QApplication>
#include <QContextMenuEvent>
#include <QElapsedTimer>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QPainter>
#include <QPainterPath>
#include <QDesktopServices>
#include <QMessageBox>
#include <QPointer>
#include <QTabWidget>
#include <QToolButton>
#include <QUrl>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace stagekit {

namespace {

const char *const kDockMoveFilter = "StageKit dock move";
const char *const kDestinationText[] = {"Place.TopLeft",    "Place.TopCenter",    "Place.TopRight",
					"Place.MiddleLeft", "Place.Center",       "Place.MiddleRight",
					"Place.BottomLeft", "Place.BottomCenter", "Place.BottomRight"};
const char *const kArrows[] = {"↖", "↑", "↗", "←", "•", "→", "↙", "↓", "↘"};
// Send To's "a spot on the screen" destination, which the dock's slides use.
constexpr int kSpotDestination = 10;
const char *const kSlideTips[] = {"SlideTip.UpLeft",   "SlideTip.Up",     "SlideTip.UpRight",
				  "SlideTip.Left",     "SlideTip.Centre", "SlideTip.Right",
				  "SlideTip.DownLeft", "SlideTip.Down",   "SlideTip.DownRight"};
const char *const kSlideText[] = {"Slide.UpLeft", "Slide.Up",       "Slide.UpRight", "Slide.Left",     "Slide.Centre",
				  "Slide.Right",  "Slide.DownLeft", "Slide.Down",    "Slide.DownRight"};

QString tr_text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// The background picture, small. The wheel zooms it and dragging pans it;
// what it shows is the part the moving background will use.
class Thumb : public QLabel {
public:
	using QLabel::QLabel;
	std::function<void(double steps)> on_wheel;
	std::function<void(double dx, double dy)> on_drag; // in fractions of the thumbnail
	std::function<void()> on_reset;
	std::function<void()> on_resize;

protected:
	void resizeEvent(QResizeEvent *event) override
	{
		QLabel::resizeEvent(event);
		if (on_resize && event->size().width() != event->oldSize().width())
			on_resize();
	}
	void wheelEvent(QWheelEvent *event) override
	{
		if (on_wheel)
			on_wheel(event->angleDelta().y() / 120.0);
		event->accept();
	}
	void mousePressEvent(QMouseEvent *event) override { last_ = event->position(); }
	void mouseMoveEvent(QMouseEvent *event) override
	{
		const QPointF now = event->position();
		if (on_drag && width() > 0 && height() > 0)
			on_drag((now.x() - last_.x()) / width(), (now.y() - last_.y()) / height());
		last_ = now;
	}
	void mouseDoubleClickEvent(QMouseEvent *) override
	{
		if (on_reset)
			on_reset();
	}

private:
	QPointF last_;
};

// For the tests, which have no mouse: stands in for a button being held.
bool g_test_mouse_down = false;
// For the tests: how long a snap or a resize limit takes to glide into
// place; -1 for the dock's own glide time.
int g_test_correct_ms = -1;

// Adds a StageKit menu to the menu OBS opens on a right-click in its preview,
// so lining up and snapping are at hand where the items are being moved. OBS
// has no hook for that menu: this watches for it opening instead.
class PreviewMenu : public QObject {
public:
	using QObject::QObject;

	bool eventFilter(QObject *object, QEvent *event) override
	{
		if (event->type() == QEvent::ContextMenu) {
			if (object->objectName() == QLatin1String("preview"))
				asked_.start();
		} else if (event->type() == QEvent::Show && asked_.isValid() && asked_.elapsed() < 1500) {
			if (auto *menu = qobject_cast<QMenu *>(object)) {
				asked_.invalidate();
				extend(menu);
			}
		}
		return false;
	}

	static void extend(QMenu *menu)
	{
		menu->addSeparator();
		QMenu *sub = menu->addMenu(tr_text("Arrange.Menu"));
		sub->setObjectName("stagekitArrangeMenu");
		for (int a = 0; a < (int)ArrangeAction::Count; a++) {
			const auto action = static_cast<ArrangeAction>(a);
			if (action == ArrangeAction::SpaceAcross || action == ArrangeAction::SameWidth ||
			    action == ArrangeAction::Top)
				sub->addSeparator();
			// The hotkey names read "StageKit: Align left"; the menu is already StageKit's.
			QAction *entry =
				sub->addAction(tr_text(arrange_info(action).hotkey).section(QStringLiteral(": "), 1));
			entry->setObjectName(QString("menu_") + arrange_info(action).name);
			// Run once the menu has closed.
			QObject::connect(entry, &QAction::triggered, qApp, [action] {
				QTimer::singleShot(0, qApp, [action] { run_arrange_on_selection(action); });
			});
		}
		sub->addSeparator();
		const ArrangeOptions o = arrange_options();
		auto option = [&](const char *name, const char *text, bool on, void (*set)(ArrangeOptions &, bool)) {
			QAction *entry = sub->addAction(tr_text(text).replace("&", "&&"));
			entry->setObjectName(name);
			entry->setCheckable(true);
			entry->setChecked(on);
			QObject::connect(entry, &QAction::toggled, qApp, [set](bool checked) {
				ArrangeOptions changed = arrange_options();
				set(changed, checked);
				set_arrange_options(changed);
			});
		};
		QMenu *to = sub->addMenu(tr_text("Arrange.Against"));
		struct {
			const char *name, *text;
			Against against;
		} targets[] = {
			{"menuToScreen", "Arrange.Against.Screen", Against::Screen},
			{"menuToEachOther", "Arrange.Against.Selection", Against::Selection},
			{"menuToFirst", "Arrange.Against.First", Against::First},
			{"menuToLast", "Arrange.Against.Last", Against::Last},
		};
		for (const auto &t : targets) {
			QAction *entry = to->addAction(tr_text(t.text));
			entry->setObjectName(t.name);
			entry->setCheckable(true);
			entry->setChecked(o.against == t.against);
			const Against against = t.against;
			QObject::connect(entry, &QAction::triggered, qApp, [against] {
				ArrangeOptions changed = arrange_options();
				changed.against = against;
				set_arrange_options(changed);
			});
		}
		option("menuVisible", "Arrange.Visible", o.how.visible_pixels,
		       [](ArrangeOptions &c, bool on) { c.how.visible_pixels = on; });
		option("menuNoShadow", "Arrange.NoShadow", o.how.leave_out_shadows,
		       [](ArrangeOptions &c, bool on) { c.how.leave_out_shadows = on; });
		option("menuKeepShape", "Arrange.KeepShape", o.keep_shape,
		       [](ArrangeOptions &c, bool on) { c.keep_shape = on; });
		option("menuLiveSnap", "Arrange.LiveSnap", o.live_snap,
		       [](ArrangeOptions &c, bool on) { c.live_snap = on; });
		option("menuGuides", "Arrange.Guides", o.guides, [](ArrangeOptions &c, bool on) { c.guides = on; });
		option("menuLimitResize", "Arrange.LimitResize", o.limit_resize,
		       [](ArrangeOptions &c, bool on) { c.limit_resize = on; });
	}

private:
	QElapsedTimer asked_;
};

PreviewMenu *g_preview_menu = nullptr;

// The start image and the moving background, in a dock of their own.
class BackgroundDock : public QWidget {
public:
	explicit BackgroundDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		auto *layout = new QVBoxLayout(this);
		// The start image: chosen here, shown by the Start Image source and
		// used by any Ambient Background set to follow it.
		auto *start_box = new QGroupBox(tr_text("Dock.StartBox"), this);
		auto *start_layout = new QVBoxLayout(start_box);
		start_label_ = new QLabel(start_box);
		start_label_->setObjectName("startImage");
		// A long file name must not set the dock's width: the label takes
		// whatever room there is and the name is shortened to fit.
		start_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		start_label_->setMinimumWidth(0);
		start_label_->setFixedHeight(start_label_->fontMetrics().height() + 6);
		start_label_->setAlignment(Qt::AlignCenter);
		start_thumb_ = new Thumb(start_box);
		start_thumb_->setObjectName("startThumb");
		start_thumb_->setCursor(Qt::OpenHandCursor);
		// Zooming and dragging only change this small picture, for a closer
		// look at it. The scene itself is not touched.
		start_thumb_->on_wheel = [this](double steps) {
			thumb_zoom_ = std::clamp(thumb_zoom_ * std::pow(1.1, steps), 1.0, 8.0);
			show_thumb();
		};
		start_thumb_->on_drag = [this](double dx, double dy) {
			// Dragging the picture right shows more of its left.
			thumb_x_ = std::clamp(thumb_x_ - dx / thumb_zoom_, 0.0, 1.0);
			thumb_y_ = std::clamp(thumb_y_ - dy / thumb_zoom_, 0.0, 1.0);
			show_thumb();
		};
		start_thumb_->on_reset = [this] {
			thumb_zoom_ = 1.0;
			thumb_x_ = thumb_y_ = 0.5;
			show_thumb();
		};
		start_thumb_->setAlignment(Qt::AlignCenter);
		start_thumb_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		start_thumb_->setFixedHeight(kThumbHeight);
		// The picture is as wide as the dock allows.
		start_thumb_->on_resize = [this] {
			show_thumb();
		};
		auto *choose = new QPushButton(tr_text("Dock.StartChoose"), start_box);
		choose->setObjectName("chooseStartImage");
		QObject::connect(choose, &QPushButton::clicked, this, [this] {
			const QString current = QString::fromUtf8(start_image_path().c_str());
			const QString file = QFileDialog::getOpenFileName(
				this, tr_text("Dock.StartChoose"), current,
				"Images (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp *.avif);;All files (*.*)");
			if (!file.isEmpty()) {
				choose_start_image(file.toUtf8().constData());
				refresh();
			}
		});
		// The moving background uses the start image unless it is given a
		// picture of its own here.
		auto *other = new QPushButton(tr_text("Dock.BackgroundOther"), start_box);
		other->setObjectName("backgroundOther");
		QObject::connect(other, &QPushButton::clicked, this, [this] {
			const QString file = QFileDialog::getOpenFileName(
				this, tr_text("Dock.BackgroundOther"),
				QString::fromUtf8(ambient_picture_path().c_str()),
				"Images (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp *.avif);;All files (*.*)");
			if (!file.isEmpty()) {
				AmbientView view = ambient_view();
				view.image = file.toUtf8().constData();
				set_ambient_view(view);
				refresh();
			}
		});
		background_same_ = new QPushButton(tr_text("Dock.BackgroundSame"), start_box);
		background_same_->setObjectName("backgroundSame");
		QObject::connect(background_same_, &QPushButton::clicked, this, [this] {
			AmbientView view = ambient_view();
			view.image.clear();
			set_ambient_view(view);
			refresh();
		});
		auto *background_settings = new QPushButton(tr_text("Dock.BackgroundSettings"), start_box);
		background_settings->setObjectName("backgroundSettings");
		QObject::connect(background_settings, &QPushButton::clicked, this,
				 [this] { open_background_settings(); });
		start_layout->addWidget(start_thumb_);
		start_layout->addWidget(start_label_);
		start_layout->addWidget(choose);
		start_layout->addWidget(other);
		start_layout->addWidget(background_same_);
		start_layout->addWidget(background_settings);
		layout->addWidget(start_box);
		status_ = new QLabel(this);
		status_->setObjectName("backgroundStatus");
		status_->setWordWrap(true);
		layout->addWidget(status_);
		layout->addStretch(1);

		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] {
			if (!measuring_stopped()) // not while OBS is closing
				refresh();
		});
		timer->start(400);
		refresh();
	}

	void refresh()
	{
		const QString start = QString::fromUtf8(start_image_path().c_str());
		const QString shown = QString::fromUtf8(ambient_picture_path().c_str());
		const bool own = shown != start;
		QString name = shown.isEmpty() ? tr_text("Dock.StartNone") : QFileInfo(shown).fileName();
		if (name.size() > 40)
			name = name.left(22) + QStringLiteral("…") + name.right(16);
		start_label_->setText(own ? tr_text("Dock.BackgroundIs").arg(name) : name);
		start_label_->setToolTip(shown);
		background_same_->setVisible(own);
		// A picture that could not be read yet (a drive still waking up) is
		// tried again a little later.
		const bool again = retry_in_ > 0 && --retry_in_ == 0;
		if (shown != thumb_path_ || again) {
			const bool same_picture = shown == thumb_path_;
			// Read the picture already shrunk, so a large image costs little.
			thumb_path_ = shown;
			if (!same_picture) {
				tries_ = 0;
				thumb_zoom_ = 1.0;
				thumb_x_ = thumb_y_ = 0.5;
			}
			thumb_full_ = load_picture(shown);
			retry_in_ = thumb_full_.isNull() && !shown.isEmpty() && ++tries_ < 6 ? 5 : 0;
			start_thumb_->setToolTip(tr_text("Dock.ThumbHint") + QStringLiteral("\n") + shown);
			show_thumb();
		}
	}

private:
	void say(const QString &text) { status_->setText(text); }

	// Reads a picture already shrunk, so a large image costs little. Qt in
	// OBS reads only a few formats; the rest go through OBS's own loader.
	static QPixmap load_picture(const QString &path)
	{
		if (path.isEmpty())
			return QPixmap();
		QImageReader reader(path);
		reader.setAutoTransform(true);
		const QSize size = reader.size();
		if (size.isValid())
			reader.setScaledSize(size.scaled(1280, 720, Qt::KeepAspectRatio));
		QPixmap picture = QPixmap::fromImageReader(&reader);
		if (!picture.isNull())
			return picture;
		gs_color_format format = GS_UNKNOWN;
		uint32_t cx = 0, cy = 0;
		uint8_t *data = gs_create_texture_file_data(path.toUtf8().constData(), &format, &cx, &cy);
		if (data && cx && cy && (format == GS_RGBA || format == GS_BGRA || format == GS_BGRX)) {
			const QImage::Format as = format == GS_RGBA   ? QImage::Format_RGBA8888
						  : format == GS_BGRA ? QImage::Format_ARGB32
								      : QImage::Format_RGB32;
			const QImage image(data, (int)cx, (int)cy, (int)cx * 4, as);
			picture = QPixmap::fromImage(
				image.scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation));
		}
		bfree(data);
		if (picture.isNull())
			obs_log(LOG_WARNING, "dock: could not read the picture '%s' (%s)", path.toUtf8().constData(),
				reader.errorString().toUtf8().constData());
		return picture;
	}

	// Draws the picture small, at the zoom and place chosen with the wheel
	// and by dragging. Zoomed right out, the whole picture is shown.
	void show_thumb()
	{
		QPixmap picture;
		if (!thumb_full_.isNull()) {
			const double pw = thumb_full_.width(), ph = thumb_full_.height();
			const double w = pw / thumb_zoom_, h = ph / thumb_zoom_;
			const double x = std::clamp(thumb_x_ * pw - w / 2.0, 0.0, pw - w);
			const double y = std::clamp(thumb_y_ * ph - h / 2.0, 0.0, ph - h);
			// As wide as the dock, but never so tall that the buttons below
			// are pushed out of a short dock.
			const int room_w = start_thumb_->width() > 40 ? start_thumb_->width() : 320;
			const QWidget *around = parentWidget();
			const int room_h = std::max(kThumbHeight, (around ? around->height() : 100000) - kBelowThumb);
			const QSize fit = QSizeF(w, h).scaled(room_w, room_h, Qt::KeepAspectRatio).toSize();
			picture = thumb_full_.copy(QRectF(x, y, w, h).toRect())
					  .scaled(fit, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
			if (start_thumb_->height() != fit.height())
				start_thumb_->setFixedHeight(std::max(1, fit.height()));
		}
		start_thumb_->setPixmap(picture);
		start_thumb_->setVisible(!picture.isNull());
	}

	// Opens the settings of the collection's moving background.
	void open_background_settings()
	{
		obs_source_t *found = nullptr;
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto **out = static_cast<obs_source_t **>(param);
				const char *id = obs_source_get_unversioned_id(source);
				if (!*out && id && strcmp(id, "stagekit_ambient_background") == 0)
					*out = obs_source_get_ref(source);
				return true;
			},
			&found);
		if (!found) {
			say(tr_text("Dock.BackgroundNone"));
			return;
		}
		if (obs_frontend_get_main_window())
			obs_frontend_open_source_properties(found);
		obs_source_release(found);
	}

	static constexpr int kThumbHeight = 120;
	static constexpr int kBelowThumb = 200; // the label and buttons under the picture
	QLabel *start_label_ = nullptr;
	Thumb *start_thumb_ = nullptr;
	QPixmap thumb_full_;
	double thumb_zoom_ = 1.0, thumb_x_ = 0.5, thumb_y_ = 0.5; // the thumbnail's own view
	QPushButton *background_same_ = nullptr;
	QString thumb_path_ = QStringLiteral("\x01"); // nothing loaded yet
	int retry_in_ = 0, tries_ = 0;
	QLabel *status_ = nullptr;
};

class Dock : public QWidget {
public:
	// fixed_scene: a scene name to control instead of OBS's current scene
	// (used by the tests, where there is no OBS window).
	explicit Dock(const std::string &fixed_scene = {}, QWidget *parent = nullptr)
		: QWidget(parent),
		  fixed_scene_(fixed_scene)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(8, 8, 8, 8);
		layout->setSpacing(8);

		// What it works on, always in view; below it one tab for each kind
		// of job, so the dock is four short pages instead of one long one.
		auto *form = new QFormLayout();
		form->setContentsMargins(0, 0, 0, 0);
		// Any scene can be worked on, not only the one on screen, so items
		// that live in a nested scene are reachable.
		scene_ = new QComboBox(this);
		scene_->setObjectName("scene");
		QObject::connect(scene_, &QComboBox::currentIndexChanged, this, [this](int) { refresh(); });
		form->addRow(tr_text("Dock.SceneRow"), scene_);
		item_ = new QComboBox(this);
		item_->setObjectName("item");
		linked_ = new QComboBox(this);
		linked_->setObjectName("linked");
		form->addRow(tr_text("Dock.Item"), item_);
		layout->addLayout(form);

		auto *tabs = new QTabWidget(this);
		tabs->setObjectName("stagekitTabs");
		tabs->setDocumentMode(true);
		auto page = [tabs](const char *title, const char *name) {
			auto *content = new QWidget(tabs);
			content->setObjectName(name);
			auto *column = new QVBoxLayout(content);
			column->setContentsMargins(0, 10, 0, 0);
			column->setSpacing(10);
			tabs->addTab(content, tr_text(title));
			return column;
		};
		QVBoxLayout *place_page = page("Dock.Tab.Place", "pagePlace");
		QVBoxLayout *arrange_page = page("Dock.Tab.Arrange", "pageArrange");
		QVBoxLayout *copy_page = page("Dock.Tab.Copy", "pageCopy");
		QVBoxLayout *effects_page = page("Dock.Tab.Effects", "pageEffects");
		layout->addWidget(tabs, 1);

		auto *move_box = new QWidget(this);
		auto *move_layout = new QVBoxLayout(move_box);
		move_layout->setContentsMargins(0, 0, 0, 0);
		move_layout->setSpacing(8);
		auto *grid = new QGridLayout();
		grid->setSpacing(4);
		for (int i = 0; i < 9; i++) {
			auto *button = new QPushButton(QString::fromUtf8(kArrows[i]), move_box);
			button->setObjectName(QString("grid%1").arg(i));
			button->setToolTip(tr_text(kDestinationText[i]));
			button->setMinimumHeight(44);
			QFont arrow = button->font();
			arrow.setPointSizeF(arrow.pointSizeF() * 1.5);
			button->setFont(arrow);
			QObject::connect(button, &QPushButton::clicked, this, [this, i] { move_now(i); });
			grid->addWidget(button, i / 3, i % 3);
		}
		move_layout->addLayout(grid);

		auto *options = new QFormLayout();
		margin_ = new QSpinBox(move_box);
		margin_->setObjectName("margin");
		margin_->setRange(0, 400);
		margin_->setValue(30);
		margin_->setSuffix(" px");
		duration_ = new QSpinBox(move_box);
		duration_->setObjectName("duration");
		duration_->setRange(0, 5000);
		duration_->setSingleStep(50);
		duration_->setValue(500);
		duration_->setSuffix(" ms");
		options->addRow(tr_text("Place.Linked"), linked_);
		options->addRow(tr_text("Place.Margin"), margin_);
		options->addRow(tr_text("Place.Duration"), duration_);
		// How the move glides: the same curves the filters offer.
		curve_ = new QComboBox(move_box);
		curve_->setObjectName("sendCurve");
		for (const char *name :
		     {"Easing.Linear", "Easing.Sine", "Easing.Quad", "Easing.Cubic", "Easing.Quart", "Easing.Quint",
		      "Easing.Expo", "Easing.Circ", "Easing.Back", "Easing.Elastic", "Easing.Bounce"})
			curve_->addItem(tr_text(name));
		curve_->setToolTip(tr_text("Dock.Motion.Tip"));
		ease_mode_ = new QComboBox(move_box);
		ease_mode_->setObjectName("sendMode");
		for (const char *name : {"Easing.InOut", "Easing.In", "Easing.Out"})
			ease_mode_->addItem(tr_text(name));
		ease_mode_->setToolTip(tr_text("Dock.Motion.Tip"));
		// Curve and direction side by side: one row, not two.
		auto *motion = new QHBoxLayout();
		motion->setSpacing(4);
		motion->addWidget(curve_, 1);
		motion->addWidget(ease_mode_, 1);
		options->addRow(tr_text("Dock.Motion"), motion);
		move_layout->addLayout(options);
		slide_ = new QCheckBox(tr_text("Dock.Slide"), move_box);
		slide_->setObjectName("sendSlide");
		slide_->setToolTip(tr_text("Dock.Slide.Tip"));
		move_layout->addWidget(slide_);
		// The arrows say what they will do.
		QObject::connect(slide_, &QCheckBox::toggled, this, [this, move_box](bool sliding) {
			for (int i = 0; i < 9; i++)
				if (auto *button = move_box->findChild<QPushButton *>(QString("grid%1").arg(i)))
					button->setToolTip(tr_text(sliding ? kSlideTips[i] : kDestinationText[i]));
		});
		every_scene_ = new QCheckBox(tr_text("Dock.SendEvery"), move_box);
		every_scene_->setObjectName("sendEveryScene");
		every_scene_->setToolTip(tr_text("Dock.SendEvery.Tip"));
		every_scene_->setChecked(true);
		move_layout->addWidget(every_scene_);
		place_page->addWidget(move_box);

		auto *keys_box = new QGroupBox(tr_text("Dock.KeysBox"), this);
		auto *keys_layout = new QVBoxLayout(keys_box);
		auto *make_keys = new QPushButton(tr_text("Dock.MakeKeys"), keys_box);
		make_keys->setObjectName("makeKeys");
		QObject::connect(make_keys, &QPushButton::clicked, this, [this] { make_all_keys(); });
		auto *save_spot = new QPushButton(tr_text("Dock.SaveSpot"), keys_box);
		save_spot->setObjectName("saveSpot");
		QObject::connect(save_spot, &QPushButton::clicked, this, [this] {
			bool ok = false;
			const QString name = QInputDialog::getText(this, tr_text("Dock.SaveSpot"),
								   tr_text("Dock.SaveSpotPrompt"), QLineEdit::Normal,
								   QString(), &ok);
			if (ok && !name.trimmed().isEmpty())
				save_spot_as(name.trimmed());
		});
		// Explanations live in tooltips: the dock itself stays short.
		keys_box->setToolTip(tr_text("Dock.KeysHint"));
		keys_layout->addWidget(make_keys);
		keys_layout->addWidget(save_spot);
		place_page->addWidget(keys_box);
		place_page->addStretch(1);

		auto *add_box = new QWidget(this);
		auto *add_layout = new QGridLayout(add_box);
		add_layout->setContentsMargins(0, 0, 0, 0);
		add_layout->setSpacing(6);
		struct {
			const char *object_name, *text, *filter_id;
			bool on_scene;
		} effects[] = {
			{"addBlur", "Dock.AddBlur", "stagekit_ramp_blur", false},
			{"addShadow", "Dock.AddShadow", "stagekit_shadow_backdrop", false},
			{"addCorners", "Dock.AddCorners", "stagekit_corners", false},
			{"addZoom", "Dock.AddZoom", "stagekit_ramp_zoom", false},
			{"addShowHide", "Dock.AddShowHide", kShowHideId, true},
		};
		int n = 0;
		for (const auto &effect : effects) {
			// Qt reads a single "&" as a keyboard-shortcut marker.
			auto *button = new QPushButton(tr_text(effect.text).replace("&", "&&"), add_box);
			button->setObjectName(effect.object_name);
			const std::string id = effect.filter_id;
			const bool on_scene = effect.on_scene;
			QObject::connect(button, &QPushButton::clicked, this,
					 [this, id, on_scene] { add_effect(id, on_scene); });
			button->setMinimumHeight(30);
			// One to a row: the names are long, and a list reads easier.
			add_layout->addWidget(button, n, 0);
			n++;
		}
		effects_page->addWidget(add_box);
		effects_page->addStretch(1);

		// Lining up, spacing and snapping whatever is selected in OBS.
		auto *arrange_box = new QWidget(this);
		auto *arrange_layout = new QVBoxLayout(arrange_box);
		arrange_layout->setContentsMargins(0, 0, 0, 0);
		arrange_layout->setSpacing(8);
		// Says what the buttons below will move, so nothing is a surprise.
		// Every selected item, in the order selected. It only shows: items
		// are selected in OBS's preview.
		arrange_list_ = new QListWidget(arrange_box);
		arrange_list_->setObjectName("arrangeSelected");
		arrange_list_->setSelectionMode(QAbstractItemView::NoSelection);
		arrange_list_->setFocusPolicy(Qt::NoFocus);
		arrange_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		arrange_list_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		arrange_list_->setToolTip(tr_text("Arrange.Selected.Tip"));
		arrange_layout->addWidget(new QLabel(tr_text("Arrange.Selected"), arrange_box));
		arrange_layout->addWidget(arrange_list_);
		auto *against_row = new QFormLayout();
		against_ = new QComboBox(arrange_box);
		against_->setObjectName("arrangeAgainst");
		against_->addItem(tr_text("Arrange.Against.Screen"), (int)Against::Screen);
		against_->addItem(tr_text("Arrange.Against.Selection"), (int)Against::Selection);
		against_->addItem(tr_text("Arrange.Against.First"), (int)Against::First);
		against_->addItem(tr_text("Arrange.Against.Last"), (int)Against::Last);
		against_->setToolTip(tr_text("Arrange.Against.Tip"));
		against_row->addRow(tr_text("Arrange.Against"), against_);
		arrange_layout->addLayout(against_row);
		auto *arrange_grid = new QGridLayout();
		auto arrange_button = [this](ArrangeAction action, QWidget *parent) {
			const ArrangeInfo &info = arrange_info(action);
			auto *button = new QPushButton(tr_text(info.text), parent);
			button->setObjectName(QString("arrange_") + info.name);
			button->setIcon(tool_icon(info.name, button));
			button->setMinimumHeight(30);
			button->setToolTip(tr_text((std::string(info.text) + ".Tip").c_str()));
			QObject::connect(button, &QPushButton::clicked, this, [this, action] {
				obs_source_t *scene = arrange_scene();
				const ArrangeOutcome out = run_arrange(action, scene, arrange_items(scene));
				obs_source_release(scene);
				const bool copied_nothing = out.had > 0 && out.changed == 0;
				const bool to_sources = !arrange_options().copy_sources.empty();
				if (action == ArrangeAction::SameChosen && out.had > 0 &&
				    arrange_options().copy_scenes.empty())
					say(tr_text("Arrange.ChooseScenes"));
				else if (to_sources && copied_nothing &&
					 (action == ArrangeAction::SameChosen ||
					  action == ArrangeAction::SameEverywhere))
					say(tr_text("Arrange.NoneOfSources"));
				else if (action == ArrangeAction::SameChosen && copied_nothing)
					say(tr_text("Arrange.NoneChosen"));
				else if (action == ArrangeAction::SameEverywhere && copied_nothing)
					say(tr_text("Arrange.NoneElsewhere"));
				else
					say(arrange_message(out));
			});
			return button;
		};
		// The line-up and size tools, three to a row. The two that copy to
		// other scenes follow in a box of their own, named for what they do.
		const int first_copy = (int)ArrangeAction::SameEverywhere;
		for (int a = 0; a < first_copy; a++)
			arrange_grid->addWidget(arrange_button(static_cast<ArrangeAction>(a), arrange_box), a / 3,
						a % 3);
		arrange_layout->addLayout(arrange_grid);
		arrange_grid->setSpacing(4);
		auto *copy_box = new QWidget(this);
		auto *copy_layout = new QVBoxLayout(copy_box);
		copy_layout->setContentsMargins(0, 0, 0, 0);
		copy_layout->setSpacing(6);
		copy_layout->addWidget(arrange_button(ArrangeAction::SameEverywhere, copy_box));
		copy_layout->addWidget(arrange_button(ArrangeAction::SameChosen, copy_box));
		// The scenes the second one copies to: choose to add, one row each
		// with its own remove button, as every list in StageKit.
		auto *copy_label = new QLabel(tr_text("Arrange.CopyScenes"), copy_box);
		copy_label->setToolTip(tr_text("Arrange.CopyScenes.Tip"));
		copy_layout->addWidget(copy_label);
		copy_rows_ = new QVBoxLayout();
		copy_rows_->setSpacing(2);
		copy_layout->addLayout(copy_rows_);
		copy_add_ = new QComboBox(copy_box);
		copy_add_->setObjectName("copyScenesAdd");
		copy_add_->setToolTip(tr_text("Arrange.CopyScenes.Tip"));
		QObject::connect(copy_add_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
			const QString name = copy_add_->itemData(index).toString();
			if (name.isEmpty())
				return;
			ArrangeOptions o = arrange_options();
			o.copy_scenes.push_back(name.toUtf8().toStdString());
			set_arrange_options(o);
			show_copy_scenes();
		});
		copy_layout->addWidget(copy_add_);
		// The sources that take the transform: the same kind of list. Left
		// empty, it is the selected item's own source.
		auto *sources_label = new QLabel(tr_text("Arrange.CopySources"), copy_box);
		sources_label->setToolTip(tr_text("Arrange.CopySources.Tip"));
		copy_layout->addWidget(sources_label);
		source_rows_ = new QVBoxLayout();
		source_rows_->setSpacing(2);
		copy_layout->addLayout(source_rows_);
		source_add_ = new QComboBox(copy_box);
		source_add_->setObjectName("copySourcesAdd");
		source_add_->setToolTip(tr_text("Arrange.CopySources.Tip"));
		QObject::connect(source_add_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
			const QString name = source_add_->itemData(index).toString();
			if (name.isEmpty())
				return;
			ArrangeOptions o = arrange_options();
			o.copy_sources.push_back(name.toUtf8().toStdString());
			set_arrange_options(o);
			show_copy_sources();
		});
		copy_layout->addWidget(source_add_);
		copy_page->addWidget(copy_box);
		copy_page->addStretch(1);
		visible_ = new QCheckBox(tr_text("Arrange.Visible"), arrange_box);
		visible_->setObjectName("arrangeVisible");
		visible_->setToolTip(tr_text("Arrange.Visible.Tip"));
		no_shadow_ = new QCheckBox(tr_text("Arrange.NoShadow").replace("&", "&&"), arrange_box);
		no_shadow_->setObjectName("arrangeNoShadow");
		no_shadow_->setToolTip(tr_text("Arrange.NoShadow.Tip"));
		keep_shape_ = new QCheckBox(tr_text("Arrange.KeepShape"), arrange_box);
		keep_shape_->setObjectName("arrangeKeepShape");
		keep_shape_->setToolTip(tr_text("Arrange.KeepShape.Tip"));
		live_snap_ = new QCheckBox(tr_text("Arrange.LiveSnap"), arrange_box);
		live_snap_->setObjectName("arrangeLiveSnap");
		live_snap_->setToolTip(tr_text("Arrange.LiveSnap.Tip"));
		guides_ = new QCheckBox(tr_text("Arrange.Guides"), arrange_box);
		guides_->setObjectName("arrangeGuides");
		guides_->setToolTip(tr_text("Arrange.Guides.Tip"));
		// Says at once whether guides can show: a frame flashes round the
		// canvas in the preview, or the preview was not found.
		QObject::connect(guides_, &QCheckBox::toggled, this, [this](bool on) {
			if (!on || storing_)
				return;
			QTimer::singleShot(600, this, [this] {
				if (!guides_->isChecked())
					return;
				QString text = tr_text(guides_preview_state() == 2 ? "Arrange.Guides.NoPreview"
										   : "Arrange.Guides.On");
				say(text);
			});
		});
		auto *within_row = new QFormLayout();
		snap_within_ = new QSpinBox(arrange_box);
		snap_within_->setObjectName("snapWithin");
		snap_within_->setRange(1, 200);
		snap_within_->setSuffix(" px");
		within_row->addRow(tr_text("Arrange.Within"), snap_within_);
		limit_resize_ = new QCheckBox(tr_text("Arrange.LimitResize"), arrange_box);
		limit_resize_->setObjectName("arrangeLimitResize");
		limit_resize_->setToolTip(tr_text("Arrange.LimitResize.Tip"));
		limit_px_ = new QSpinBox(arrange_box);
		limit_px_->setObjectName("limitPx");
		limit_px_->setRange(0, 1000);
		limit_px_->setSuffix(" px");
		limit_px_->setToolTip(tr_text("Arrange.LimitResize.Tip"));
		within_row->addRow(tr_text("Arrange.LimitPx"), limit_px_);
		// How the tools measure, and what happens when an item is let go:
		// two short groups instead of one run of tick boxes.
		auto *measure_box = new QGroupBox(tr_text("Dock.MeasureBox"), arrange_box);
		auto *measure_layout = new QVBoxLayout(measure_box);
		measure_layout->addWidget(visible_);
		measure_layout->addWidget(no_shadow_);
		measure_layout->addWidget(keep_shape_);
		auto *drop_box = new QGroupBox(tr_text("Dock.DropBox"), arrange_box);
		auto *drop_layout = new QVBoxLayout(drop_box);
		drop_layout->addWidget(live_snap_);
		drop_layout->addWidget(guides_);
		drop_layout->addWidget(limit_resize_);
		drop_layout->addLayout(within_row);
		options_measure_ = measure_box;
		options_drop_ = drop_box;
		// The options are shared with the hotkeys and the right-click menu.
		show_arrange_options();
		for (QCheckBox *box :
		     {visible_, no_shadow_, keep_shape_, live_snap_, guides_, every_scene_, slide_, limit_resize_})
			QObject::connect(box, &QCheckBox::toggled, this, [this](bool) { store_arrange_options(); });
		QObject::connect(against_, &QComboBox::currentIndexChanged, this,
				 [this](int) { store_arrange_options(); });
		QObject::connect(snap_within_, &QSpinBox::valueChanged, this, [this](int) { store_arrange_options(); });
		QObject::connect(limit_px_, &QSpinBox::valueChanged, this, [this](int) { store_arrange_options(); });
		for (QComboBox *box : {curve_, ease_mode_})
			QObject::connect(box, &QComboBox::currentIndexChanged, this,
					 [this](int) { store_arrange_options(); });
		arrange_page->addWidget(arrange_box);

		// Snapping on letting go: watch the selected items a few times a second.
		auto *snap_timer = new QTimer(this);
		QObject::connect(snap_timer, &QTimer::timeout, this, [this] {
			if (!measuring_stopped())
				watch_for_drop();
		});
		snap_timer->start(120);
		// A Stream Deck key or a remote control can ask for a slide.
		set_slide_handler([this](int direction) {
			if (!measuring_stopped())
				slide_now(direction);
		});
		glide_timer_ = new QTimer(this);
		QObject::connect(glide_timer_, &QTimer::timeout, this, [this] { step_glides(); });

		auto *order_box = new QGroupBox(tr_text("Dock.OrderBox"), this);
		auto *order_layout = new QGridLayout(order_box);
		order_layout->setSpacing(4);
		// Two columns: all the way (front above back) and one step (forward
		// above back), each pair one above the other.
		struct {
			const char *object_name, *text;
			obs_order_movement movement;
			int row, column;
		} orders[] = {
			{"orderTop", "Dock.OrderTop", OBS_ORDER_MOVE_TOP, 0, 0},
			{"orderBottom", "Dock.OrderBottom", OBS_ORDER_MOVE_BOTTOM, 1, 0},
			{"orderUp", "Dock.OrderUp", OBS_ORDER_MOVE_UP, 0, 1},
			{"orderDown", "Dock.OrderDown", OBS_ORDER_MOVE_DOWN, 1, 1},
		};
		for (const auto &order : orders) {
			auto *button = new QPushButton(tr_text(order.text), order_box);
			button->setObjectName(order.object_name);
			const obs_order_movement movement = order.movement;
			QObject::connect(button, &QPushButton::clicked, this, [this, movement] { reorder(movement); });
			button->setIcon(tool_icon(order.object_name, button));
			order_layout->addWidget(button, order.row, order.column);
		}
		arrange_page->addWidget(order_box);
		arrange_page->addWidget(options_measure_);
		arrange_page->addWidget(options_drop_);
		arrange_page->addStretch(1);

		// A newer StageKit: said here, with one button that installs it.
		// Hidden until there is one.
		update_bar_ = new QWidget(this);
		update_bar_->setObjectName("updateBar");
		auto *update_row = new QHBoxLayout(update_bar_);
		update_row->setContentsMargins(0, 0, 0, 0);
		update_text_ = new QLabel(update_bar_);
		update_text_->setObjectName("updateText");
		update_text_->setWordWrap(true);
		auto *update_now = new QPushButton(tr_text("Update.Now"), update_bar_);
		update_now->setObjectName("updateNow");
		update_now->setToolTip(tr_text("Update.Now.Tip"));
		QObject::connect(update_now, &QPushButton::clicked, this, [this] { install_now(); });
		auto *whats_new = new QPushButton(tr_text("Update.WhatsNew"), update_bar_);
		whats_new->setObjectName("updateWhatsNew");
		QObject::connect(whats_new, &QPushButton::clicked, this, [this] {
			if (!found_.page.empty())
				QDesktopServices::openUrl(QUrl(QString::fromStdString(found_.page)));
		});
		update_row->addWidget(update_text_, 1);
		update_row->addWidget(whats_new);
		update_row->addWidget(update_now);
		update_bar_->hide();
		layout->addWidget(update_bar_);

		// What the last press did, under whichever tab is open, and beside it
		// a small menu for the updates.
		auto *footer = new QHBoxLayout();
		footer->setContentsMargins(0, 0, 0, 0);
		status_ = new QLabel(this);
		status_->setObjectName("status");
		status_->setWordWrap(true);
		status_->setEnabled(false); // the theme's quieter text colour
		footer->addWidget(status_, 1);
		auto *more = new QToolButton(this);
		more->setObjectName("updateMenu");
		more->setText(QString::fromUtf8("\xE2\x8B\xAF")); // three dots
		more->setAutoRaise(true);
		more->setPopupMode(QToolButton::InstantPopup);
		more->setToolTip(tr_text("Update.Menu.Tip"));
		auto *menu = new QMenu(more);
		QAction *about = menu->addAction(QString("StageKit %1").arg(QString::fromUtf8(current_release())));
		about->setEnabled(false);
		menu->addSeparator();
		QAction *check_now = menu->addAction(tr_text("Update.CheckNow"));
		QObject::connect(check_now, &QAction::triggered, this, [this] { look_for_update(true); });
		const UpdateSettings updates = update_settings();
		QAction *at_start = menu->addAction(tr_text("Update.AtStart"));
		at_start->setCheckable(true);
		at_start->setChecked(updates.check_at_start);
		QAction *betas = menu->addAction(tr_text("Update.Betas"));
		betas->setCheckable(true);
		betas->setChecked(updates.betas);
		auto store = [at_start, betas] {
			UpdateSettings changed;
			changed.check_at_start = at_start->isChecked();
			changed.betas = betas->isChecked();
			set_update_settings(changed);
		};
		QObject::connect(at_start, &QAction::toggled, this, [store](bool) { store(); });
		QObject::connect(betas, &QAction::toggled, this, [store](bool) { store(); });
		more->setMenu(menu);
		footer->addWidget(more, 0, Qt::AlignBottom);
		layout->addLayout(footer);
		// A look when OBS has started, if that is wanted: a little later, so
		// it is not one more thing at start-up.
		if (updates.check_at_start && obs_frontend_get_main_window())
			QTimer::singleShot(10000, this, [this] {
				if (!measuring_stopped())
					look_for_update(false);
			});

		// Scenes and their items change behind our back; a light poll keeps
		// the lists current without hooking every scene signal.
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] {
			if (!measuring_stopped()) // not while OBS is closing
				refresh();
		});
		timer->start(400);
		refresh();
	}

	void refresh()
	{
		follow_selection();
		show_arrange_target();
		if (shown_options_ != arrange_options_revision())
			show_arrange_options();

		// Scene list: "the scene on screen" first, then every scene by name.
		QStringList scenes;
		obs_enum_scenes(
			[](void *param, obs_source_t *source) {
				const char *scene_name = obs_source_get_name(source);
				if (scene_name && *scene_name && !obs_source_is_group(source))
					static_cast<QStringList *>(param)->append(QString::fromUtf8(scene_name));
				return true;
			},
			&scenes);
		// Every source shown in a scene, for 'Sources to copy to'.
		QStringList sources;
		obs_enum_scenes(
			[](void *param, obs_source_t *source) {
				if (obs_source_is_group(source))
					return true;
				static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *p) {
					auto *list = static_cast<QStringList *>(p);
					const char *name = obs_source_get_name(obs_sceneitem_get_source(item));
					if (name && *name && !list->contains(QString::fromUtf8(name)))
						list->append(QString::fromUtf8(name));
					return true;
				};
				obs_scene_enum_items(
					obs_scene_from_source(source),
					[](obs_scene_t *sc, obs_sceneitem_t *item, void *p) {
						add(sc, item, p);
						if (obs_sceneitem_is_group(item))
							obs_sceneitem_group_enum_items(item, add, p);
						return true;
					},
					param);
				return true;
			},
			&sources);
		sources.sort(Qt::CaseInsensitive);
		if (sources != sources_) {
			sources_ = sources;
			show_copy_sources();
		}
		if (scenes != scenes_) {
			scenes_ = scenes;
			show_copy_scenes();
			const QString chosen = scene_->currentData().toString();
			scene_->blockSignals(true);
			scene_->clear();
			scene_->addItem(tr_text("Dock.SceneOnScreen"), QString());
			for (const QString &n : scenes)
				scene_->addItem(n, n);
			scene_->setCurrentIndex(std::max(0, scene_->findData(chosen)));
			scene_->blockSignals(false);
		}

		obs_source_t *scene = current_scene();
		// Top-most first, as in OBS's own list; items inside groups follow their group.
		QStringList names;
		if (obs_scene_t *s = scene_of(scene)) {
			static const auto add = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				const char *item_name = obs_source_get_name(obs_sceneitem_get_source(item));
				if (item_name && *item_name)
					static_cast<QStringList *>(param)->prepend(QString::fromUtf8(item_name));
				return true;
			};
			obs_scene_enum_items(
				s,
				[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
					if (obs_sceneitem_is_group(item))
						obs_sceneitem_group_enum_items(item, add, param);
					add(sc, item, param);
					return true;
				},
				&names);
		}
		obs_source_release(scene);
		if (names == names_)
			return;
		names_ = names;

		const QString item = item_->currentText(), linked = linked_->currentData().toString();
		item_->blockSignals(true);
		linked_->blockSignals(true);
		item_->clear();
		linked_->clear();
		linked_->addItem(tr_text("Place.Linked.None"), QString());
		for (const QString &n : names) {
			item_->addItem(n);
			linked_->addItem(n, n);
		}
		item_->setCurrentIndex(std::max(0, item_->findText(item)));
		linked_->setCurrentIndex(std::max(0, linked_->findData(linked)));
		item_->blockSignals(false);
		linked_->blockSignals(false);
	}

private:
	// Returns a new reference, or nullptr.
	obs_source_t *current_scene() const
	{
		const QString chosen = scene_ ? scene_->currentData().toString() : QString();
		if (!chosen.isEmpty())
			return obs_get_source_by_name(chosen.toUtf8().constData());
		if (!fixed_scene_.empty())
			return obs_get_source_by_name(fixed_scene_.c_str());
		if (obs_frontend_preview_program_mode_active())
			return obs_frontend_get_current_preview_scene();
		return obs_frontend_get_current_scene();
	}

	void say(const QString &text) { status_->setText(text); }

	// Looks for a newer release. `asked`: somebody pressed "Check now", so the
	// answer is said either way; a look at start-up only speaks up when there
	// is something.
	void look_for_update(bool asked)
	{
		if (asked)
			say(tr_text("Update.Looking"));
		QPointer<Dock> self(this);
		check_for_update([self, asked](bool found, const Release &release) {
			// Called on another thread: handed to the window's own.
			QMetaObject::invokeMethod(
				qApp,
				[self, asked, found, release] {
					if (self)
						self->show_update(asked, found, release);
				},
				Qt::QueuedConnection);
		});
	}

	void show_update(bool asked, bool found, const Release &release)
	{
		found_ = found ? release : Release();
		const bool newer = found && !release.version.empty();
		update_bar_->setVisible(newer);
		if (newer)
			update_text_->setText(tr_text("Update.Available").arg(QString::fromStdString(release.version)));
		if (asked)
			say(!found  ? tr_text("Update.NoAnswer")
			    : newer ? tr_text("Update.Available").arg(QString::fromStdString(release.version))
				    : tr_text("Update.Newest").arg(QString::fromUtf8(current_release())));
	}

	// One press: after a yes, the update is downloaded and checked, OBS
	// closes, the installer runs and OBS opens again.
	void install_now()
	{
		if (found_.version.empty())
			return;
		if (!can_install_updates()) {
			QDesktopServices::openUrl(QUrl(QString::fromStdString(found_.page)));
			return;
		}
		if (QMessageBox::question(this, tr_text("Update.Title"),
					  tr_text("Update.Confirm").arg(QString::fromStdString(found_.version)),
					  QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
			return;
		std::string error;
		if (install_update(found_, error))
			say(tr_text("Update.Started"));
		else
			say(QString::fromStdString(error));
	}

	// Lists what the Arrange buttons will act on, and marks the item the
	// others will come to when lining up to the first or last selected.
	void show_arrange_target()
	{
		obs_source_t *scene = arrange_scene();
		const std::vector<obs_sceneitem_t *> items = scene ? arrange_items(scene)
								   : std::vector<obs_sceneitem_t *>();
		const Against against = arrange_options().against;
		QStringList rows;
		int key = -1;
		if (items.size() > 1 && against == Against::First)
			key = 0;
		else if (items.size() > 1 && against == Against::Last)
			key = (int)items.size() - 1;
		for (size_t i = 0; i < items.size(); i++) {
			const QString name = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(items[i])));
			rows << ((int)i == key ? tr_text("Arrange.KeyItem").arg(name) : name);
		}
		obs_source_release(scene);
		if (rows.isEmpty())
			rows << tr_text("Arrange.SelectFirst");
		if (rows == arrange_rows_)
			return;
		arrange_rows_ = rows;
		arrange_list_->clear();
		arrange_list_->addItems(rows);
		if (items.empty())
			arrange_list_->item(0)->setForeground(
				arrange_list_->palette().color(QPalette::Disabled, QPalette::Text));
		if (key >= 0) {
			QFont bold = arrange_list_->font();
			bold.setBold(true);
			arrange_list_->item(key)->setFont(bold);
		}
		// Tall enough for up to five; more than that scrolls.
		const int row = std::max(18, arrange_list_->sizeHintForRow(0));
		arrange_list_->setFixedHeight(row * std::clamp((int)rows.size(), 1, 5) +
					      2 * arrange_list_->frameWidth());
	}

	Measure measure() const { return arrange_options().how; }

	QString arrange_message(const ArrangeOutcome &out) const
	{
		if (out.had == 0)
			return tr_text("Arrange.SelectFirst");
		if (out.had < out.needs)
			return tr_text(out.needs >= 3 ? "Arrange.NeedThree" : "Arrange.NeedTwo");
		return tr_text("Arrange.Done").arg(out.changed);
	}

	// Puts the shared options into the dock's controls.
	void show_arrange_options()
	{
		const ArrangeOptions o = arrange_options();
		shown_options_ = arrange_options_revision();
		storing_ = true;
		against_->setCurrentIndex(std::max(0, against_->findData((int)o.against)));
		visible_->setChecked(o.how.visible_pixels);
		no_shadow_->setChecked(o.how.leave_out_shadows);
		keep_shape_->setChecked(o.keep_shape);
		live_snap_->setChecked(o.live_snap);
		limit_resize_->setChecked(o.limit_resize);
		limit_px_->setValue(o.limit_px);
		guides_->setChecked(o.guides);
		every_scene_->setChecked(o.send_every_scene);
		slide_->setChecked(o.send_slide);
		curve_->setCurrentIndex(std::clamp(o.send_curve, 0, curve_->count() - 1));
		ease_mode_->setCurrentIndex(std::clamp(o.send_mode, 0, ease_mode_->count() - 1));
		snap_within_->setValue(o.within);
		storing_ = false;
		show_copy_scenes();
		show_copy_sources();
	}

	// The list of sources to copy to, and the sources that can still be
	// added. With none listed, one plain row says what is copied to.
	void show_copy_sources()
	{
		const ArrangeOptions o = arrange_options();
		while (QLayoutItem *row = source_rows_->takeAt(0)) {
			if (row->widget())
				row->widget()->deleteLater();
			delete row;
		}
		if (o.copy_sources.empty()) {
			auto *same = new QLabel(tr_text("Arrange.CopySources.Same"));
			same->setObjectName(QStringLiteral("copySourcesSame"));
			same->setEnabled(false);
			source_rows_->addWidget(same);
		}
		for (const std::string &name : o.copy_sources) {
			auto *row = new QWidget();
			auto *row_layout = new QHBoxLayout(row);
			row_layout->setContentsMargins(0, 0, 0, 0);
			row_layout->addWidget(new QLabel(QString::fromStdString(name), row), 1);
			auto *remove = new QPushButton(QStringLiteral("x"), row);
			remove->setObjectName(QStringLiteral("copySourcesRemove"));
			remove->setFixedWidth(28);
			remove->setToolTip(tr_text("Arrange.CopySources.Remove"));
			QObject::connect(remove, &QPushButton::clicked, this, [this, name] {
				ArrangeOptions changed = arrange_options();
				changed.copy_sources.erase(std::remove(changed.copy_sources.begin(),
								       changed.copy_sources.end(), name),
							   changed.copy_sources.end());
				set_arrange_options(changed);
				show_copy_sources();
			});
			row_layout->addWidget(remove);
			source_rows_->addWidget(row);
		}
		source_add_->blockSignals(true);
		source_add_->clear();
		source_add_->addItem(tr_text("Arrange.CopySources.Add"), QString());
		for (const QString &source : sources_)
			if (std::find(o.copy_sources.begin(), o.copy_sources.end(), source.toUtf8().toStdString()) ==
			    o.copy_sources.end())
				source_add_->addItem(source, source);
		source_add_->setCurrentIndex(0);
		source_add_->blockSignals(false);
		shown_options_ = arrange_options_revision();
	}

	// The list of scenes to copy to, and the scenes that can still be added.
	void show_copy_scenes()
	{
		const ArrangeOptions o = arrange_options();
		while (QLayoutItem *row = copy_rows_->takeAt(0)) {
			if (row->widget())
				row->widget()->deleteLater();
			delete row;
		}
		for (const std::string &name : o.copy_scenes) {
			auto *row = new QWidget();
			auto *row_layout = new QHBoxLayout(row);
			row_layout->setContentsMargins(0, 0, 0, 0);
			row_layout->addWidget(new QLabel(QString::fromStdString(name), row), 1);
			auto *remove = new QPushButton(QStringLiteral("x"), row);
			remove->setObjectName(QStringLiteral("copyScenesRemove"));
			remove->setFixedWidth(28);
			remove->setToolTip(tr_text("Arrange.CopyScenes.Remove"));
			QObject::connect(remove, &QPushButton::clicked, this, [this, name] {
				ArrangeOptions changed = arrange_options();
				changed.copy_scenes.erase(std::remove(changed.copy_scenes.begin(),
								      changed.copy_scenes.end(), name),
							  changed.copy_scenes.end());
				set_arrange_options(changed);
				show_copy_scenes();
			});
			row_layout->addWidget(remove);
			copy_rows_->addWidget(row);
		}
		copy_add_->blockSignals(true);
		copy_add_->clear();
		copy_add_->addItem(tr_text("Arrange.CopyScenes.Add"), QString());
		for (const QString &scene : scenes_)
			if (std::find(o.copy_scenes.begin(), o.copy_scenes.end(), scene.toUtf8().toStdString()) ==
			    o.copy_scenes.end())
				copy_add_->addItem(scene, scene);
		copy_add_->setCurrentIndex(0);
		copy_add_->blockSignals(false);
		shown_options_ = arrange_options_revision();
	}

	void store_arrange_options()
	{
		if (storing_)
			return;
		ArrangeOptions o = arrange_options(); // keeps what has no control here
		o.against = static_cast<Against>(against_->currentData().toInt());
		o.how.visible_pixels = visible_->isChecked();
		o.how.leave_out_shadows = no_shadow_->isChecked();
		o.keep_shape = keep_shape_->isChecked();
		o.live_snap = live_snap_->isChecked();
		o.limit_resize = limit_resize_->isChecked();
		o.limit_px = limit_px_->value();
		o.guides = guides_->isChecked();
		o.send_every_scene = every_scene_->isChecked();
		o.send_slide = slide_->isChecked();
		o.send_curve = curve_->currentIndex();
		o.send_mode = ease_mode_->currentIndex();
		o.within = snap_within_->value();
		set_arrange_options(o);
		shown_options_ = arrange_options_revision();
	}

	// Where the arrange tools act: the scene being edited in OBS, where items
	// are selected in its preview. Only when nothing is selected there do
	// they fall back to the scene chosen in the dock, for its Item list.
	// Returns a new reference, or nullptr.
	obs_source_t *arrange_scene() const
	{
		obs_source_t *editing = editing_scene();
		if (editing && !selected_items(editing).empty())
			return editing;
		obs_source_release(editing);
		return current_scene();
	}

	// What the arrange buttons act on: the items selected in OBS, or failing
	// that the one chosen in the dock's list.
	std::vector<obs_sceneitem_t *> arrange_items(obs_source_t *scene) const
	{
		std::vector<obs_sceneitem_t *> items = selection_in_order(scene);
		if (items.empty()) {
			if (obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData()))
				items.push_back(item);
		}
		return items;
	}

	// "Snap when I let go": once a selected item has been moved and the mouse
	// button is up again, move it the last few pixels onto the nearest line.
	// An item's place and size, as a snap or a resize limit may change them.
	struct Placed {
		vec2 pos, scale, bounds;
		bool operator==(const Placed &o) const
		{
			return std::fabs(pos.x - o.pos.x) < 0.01f && std::fabs(pos.y - o.pos.y) < 0.01f &&
			       std::fabs(scale.x - o.scale.x) < 0.00001f && std::fabs(scale.y - o.scale.y) < 0.00001f &&
			       std::fabs(bounds.x - o.bounds.x) < 0.01f && std::fabs(bounds.y - o.bounds.y) < 0.01f;
		}
	};
	static Placed placed_of(obs_sceneitem_t *item)
	{
		Placed p;
		obs_sceneitem_get_pos(item, &p.pos);
		obs_sceneitem_get_scale(item, &p.scale);
		obs_sceneitem_get_bounds(item, &p.bounds);
		return p;
	}
	static void place_at(obs_sceneitem_t *item, const Placed &p)
	{
		obs_sceneitem_defer_update_begin(item);
		obs_sceneitem_set_pos(item, &p.pos);
		obs_sceneitem_set_scale(item, &p.scale);
		if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE)
			obs_sceneitem_set_bounds(item, &p.bounds);
		obs_sceneitem_defer_update_end(item);
	}
	struct Glide {
		obs_sceneitem_t *item; // holds a reference
		Placed from, to;
		double at, lasts;
	};
	// One step of every correction still gliding. Taking hold of an item
	// again ends them where they are.
	void step_glides()
	{
		const double dt = glide_clock_.restart() / 1000.0;
		const bool grabbed = g_test_mouse_down || guides_mouse_down();
		const auto curve = static_cast<EaseCurve>(curve_->currentIndex());
		const auto mode = static_cast<EaseMode>(ease_mode_->currentIndex());
		for (auto it = glides_.begin(); it != glides_.end();) {
			bool done = grabbed || measuring_stopped();
			if (!done) {
				it->at = std::min(1.0, it->at + dt / it->lasts);
				const float t = (float)ease(curve, mode, it->at);
				auto mix = [t](const vec2 &a, const vec2 &b) {
					vec2 out;
					vec2_set(&out, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
					return out;
				};
				Placed now{mix(it->from.pos, it->to.pos), mix(it->from.scale, it->to.scale),
					   mix(it->from.bounds, it->to.bounds)};
				if (it->at >= 1.0) {
					now = it->to;
					done = true;
				}
				place_at(it->item, now);
			}
			if (done) {
				obs_sceneitem_release(it->item);
				it = glides_.erase(it);
			} else {
				++it;
			}
		}
		if (glides_.empty())
			glide_timer_->stop();
	}

	// A small drawing of what a tool does, in the theme's own text colour, so
	// the buttons can be told apart at a glance in any OBS theme.
	static QIcon tool_icon(const std::string &name, const QWidget *on)
	{
		const int n = 64;
		QPixmap pixmap(n, n);
		pixmap.fill(Qt::transparent);
		QPainter p(&pixmap);
		p.setRenderHint(QPainter::Antialiasing);
		const QColor ink = on->palette().color(QPalette::ButtonText);
		QColor soft = ink;
		soft.setAlphaF(0.45f);
		const QPen line(ink, 5, Qt::SolidLine, Qt::RoundCap);
		auto bar = [&](qreal x, qreal y, qreal w, qreal h, const QColor &c) {
			QPainterPath path;
			path.addRoundedRect(QRectF(x, y, w, h), 4, 4);
			p.fillPath(path, c);
		};
		auto arrow = [&](QPointF from, QPointF to) {
			p.setPen(line);
			p.drawLine(from, to);
			const QLineF l(to, from);
			QLineF a = l, b = l;
			a.setLength(11);
			b.setLength(11);
			a.setAngle(l.angle() + 35);
			b.setAngle(l.angle() - 35);
			p.drawLine(a);
			p.drawLine(b);
		};
		p.setPen(line);
		if (name == "left" || name == "center" || name == "right") {
			const qreal x = name == "left" ? 8 : name == "center" ? 32 : 56;
			p.drawLine(QPointF(x, 6), QPointF(x, 58));
			const qreal w1 = 38, w2 = 24;
			auto at = [&](qreal w) {
				return name == "left" ? x + 5 : name == "center" ? x - w / 2 : x - 5 - w;
			};
			bar(at(w1), 16, w1, 12, name == "center" ? soft : ink);
			bar(at(w2), 36, w2, 12, name == "center" ? soft : ink);
		} else if (name == "top" || name == "middle" || name == "bottom") {
			const qreal y = name == "top" ? 8 : name == "middle" ? 32 : 56;
			p.drawLine(QPointF(6, y), QPointF(58, y));
			const qreal h1 = 38, h2 = 24;
			auto at = [&](qreal h) {
				return name == "top" ? y + 5 : name == "middle" ? y - h / 2 : y - 5 - h;
			};
			bar(16, at(h1), 12, h1, name == "middle" ? soft : ink);
			bar(36, at(h2), 12, h2, name == "middle" ? soft : ink);
		} else if (name == "space_across" || name == "space_down") {
			for (int i = 0; i < 3; i++) {
				if (name == "space_across")
					bar(8 + i * 19, 14, 10, 36, ink);
				else
					bar(14, 8 + i * 19, 36, 10, ink);
			}
		} else if (name == "snap_now") {
			bar(6, 18, 22, 28, soft);
			bar(36, 18, 22, 28, ink);
			arrow(QPointF(36, 54), QPointF(28, 54));
		} else if (name == "same_width" || name == "same_height" || name == "same_size") {
			p.setPen(QPen(ink, 4));
			p.drawRoundedRect(QRectF(10, 10, 44, 44), 5, 5);
			p.setPen(line);
			if (name != "same_height") {
				arrow(QPointF(32, 32), QPointF(18, 32));
				arrow(QPointF(32, 32), QPointF(46, 32));
			}
			if (name != "same_width") {
				arrow(QPointF(32, 32), QPointF(32, 18));
				arrow(QPointF(32, 32), QPointF(32, 46));
			}
		} else if (name == "same_everywhere" || name == "same_chosen") {
			// One arrangement handed on to other scenes: all of them, or some.
			bar(6, 22, 20, 20, ink);
			for (int i = 0; i < 3; i++)
				bar(40, 6 + i * 19, 18, 14, name == "same_chosen" && i == 1 ? soft : ink);
			arrow(QPointF(28, 32), QPointF(37, 32));
		} else if (name.rfind("order", 0) == 0) {
			// Two sheets, one above the other, and which way this one goes.
			const bool up = name == "orderTop" || name == "orderUp";
			bar(8, up ? 30 : 10, 34, 24, soft);
			bar(22, up ? 10 : 30, 34, 24, ink);
		}
		p.end();
		return QIcon(pixmap);
	}

	void watch_for_drop()
	{
		show_arrange_target();
		const ArrangeOptions options = arrange_options();
		if (!options.live_snap && !options.limit_resize) {
			watched_.clear();
			return;
		}
		obs_source_t *scene = arrange_scene();
		const std::vector<obs_sceneitem_t *> items = selected_items(scene);
		auto look = [&] {
			std::vector<Watched> out;
			for (obs_sceneitem_t *item : items) {
				vec2 pos;
				obs_sceneitem_get_pos(item, &pos);
				out.push_back({obs_sceneitem_get_id(item), pos.x, pos.y, scene_box(scene, item)});
			}
			return out;
		};
		std::vector<Watched> now = look();
		const bool same_items = now.size() == watched_.size() &&
					std::equal(now.begin(), now.end(), watched_.begin(),
						   [](const Watched &a, const Watched &b) { return a.id == b.id; });
		const bool moved = same_items && !(now == watched_);
		const bool button_up = !g_test_mouse_down && !guides_mouse_down();
		// Only a drag with the mouse counts. Items moved by a key, a filter or
		// a Stream Deck button are where they were sent and must stay there.
		if (moved && !button_up && !dragging_) {
			dragging_ = true;
			drag_start_ = watched_;
		}
		if (dragging_ && !moved && button_up) {
			dragging_ = false;
			const std::string before = transform_snapshot(scene);
			bool snapped = false, limited = false;
			std::vector<Placed> dropped;
			for (obs_sceneitem_t *item : items)
				dropped.push_back(placed_of(item));
			for (size_t i = 0; i < items.size(); i++) {
				const Box &began = i < drag_start_.size() ? drag_start_[i].box : now[i].box;
				const bool resized = std::fabs(now[i].box.w - began.w) >= 0.5 ||
						     std::fabs(now[i].box.h - began.h) >= 0.5;
				// A resized item is pulled back to the limit and left where
				// that puts it; one that was only moved is snapped.
				if (resized && options.limit_resize)
					limited = limit_resize(scene, items[i], began, options.limit_px, options.how) ||
						  limited;
				else if (!resized && options.live_snap)
					snapped = snap_item(scene, items[i], options.within, options.how) || snapped;
			}
			if (limited || snapped)
				add_transform_undo(obs_module_text(limited ? "Arrange.LimitResize.Undo"
									   : "Hotkey.Arrange.SnapNow"),
						   before, transform_snapshot(scene));
			if (limited)
				say(tr_text("Arrange.LimitResize.Done").arg(options.limit_px));
			// The correction glides: each item goes back to where it was let
			// go and travels to its place from there, over the dock's glide
			// time and curve. (Undo already holds the finished places.)
			const int glide_ms = g_test_correct_ms >= 0 ? g_test_correct_ms : duration_->value();
			if ((limited || snapped) && glide_ms > 0) {
				for (size_t i = 0; i < items.size(); i++) {
					const Placed to = placed_of(items[i]);
					if (to == dropped[i])
						continue;
					obs_sceneitem_addref(items[i]);
					glides_.push_back({items[i], dropped[i], to, 0.0, glide_ms / 1000.0});
					place_at(items[i], dropped[i]);
				}
				if (!glides_.empty() && !glide_timer_->isActive()) {
					glide_clock_.start();
					glide_timer_->start(16);
				}
			}
			now = look();
		}
		if (!same_items)
			dragging_ = false;
		watched_ = now;
		obs_source_release(scene);
	}

	// The dock works on whatever is selected in OBS's own preview. Choosing
	// another item in the dock's list holds until the selection changes.
	void follow_selection()
	{
		obs_source_t *scene = current_scene();
		QString selected;
		if (obs_scene_t *s = scene_of(scene)) {
			static const auto pick = [](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto *out = static_cast<QString *>(param);
				if (out->isEmpty() && obs_sceneitem_selected(item))
					*out = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
				return true;
			};
			obs_scene_enum_items(
				s,
				[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
					// A selected group is worked on as one thing, like any
					// other item; otherwise what is selected inside it is.
					if (obs_sceneitem_is_group(item) && !obs_sceneitem_selected(item))
						obs_sceneitem_group_enum_items(item, pick, param);
					pick(sc, item, param);
					return true;
				},
				&selected);
		}
		obs_source_release(scene);
		if (selected.isEmpty() || selected == followed_)
			return;
		followed_ = selected;
		const int index = item_->findText(selected);
		if (index >= 0)
			item_->setCurrentIndex(index);
	}

	// The source the selected item shows, in the scene being worked on.
	// Returns a new reference, or nullptr.
	obs_source_t *item_source() const
	{
		obs_source_t *scene = current_scene();
		obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData());
		obs_source_t *source = item ? obs_source_get_ref(obs_sceneitem_get_source(item)) : nullptr;
		obs_source_release(scene);
		return source;
	}

	// Moves the selected item in front of or behind the others.
	void reorder(obs_order_movement movement)
	{
		obs_source_t *scene = current_scene();
		obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData());
		if (!item) {
			say(tr_text("Dock.PickItem"));
		} else {
			obs_sceneitem_set_order(item, movement);
			names_.clear(); // the list order changed
			const QString keep = item_->currentText();
			refresh();
			item_->setCurrentText(keep);
			say(tr_text("Dock.Reordered").arg(keep));
		}
		obs_source_release(scene);
	}

	obs_data_t *place_settings(int destination) const
	{
		obs_data_t *d = obs_data_create();
		// In every scene the source is in, or only in the scene on screen.
		obs_data_set_int(d, "apply_in", every_scene_->isChecked() ? 0 : 1);
		obs_data_set_string(d, "linked", linked_->currentData().toString().toUtf8().constData());
		obs_data_set_int(d, "destination", destination);
		obs_data_set_double(d, "margin", margin_->value());
		obs_data_set_int(d, "duration_ms", duration_->value());
		obs_data_set_int(d, "ease_curve", curve_->currentIndex());
		obs_data_set_int(d, "ease_mode", ease_mode_->currentIndex());
		// The dock's own measuring options go with what the dock sends; a
		// filter set up by hand has its own.
		const Measure how = arrange_options().how;
		obs_data_set_bool(d, "visible_pixels", how.visible_pixels);
		obs_data_set_bool(d, "no_shadows", how.leave_out_shadows);
		return d;
	}

	// Finds the named filter on a source or creates it. Returns a new reference.
	static obs_source_t *ensure_filter(obs_source_t *owner, const char *id, const std::string &name,
					   obs_data_t *settings)
	{
		obs_source_t *filter = obs_source_get_filter_by_name(owner, name.c_str());
		if (filter) {
			obs_source_update(filter, settings);
			return filter;
		}
		filter = obs_source_create(id, name.c_str(), settings, nullptr);
		if (filter)
			obs_source_filter_add(owner, filter);
		return filter;
	}

	void move_now(int destination)
	{
		if (slide_->isChecked()) {
			slide_now(destination);
			return;
		}
		obs_data_t *settings = place_settings(destination);
		send_with(settings,
			  tr_text("Dock.Moved").arg(item_->currentText(), tr_text(kDestinationText[destination])));
		obs_data_release(settings);
	}

	// Slides the chosen item that way until it comes within the edge
	// distance of another item or of the screen's edge; the centre button
	// centres it in the free space around it. Measured by the Arrange
	// options, so what is visible and whether shadows count apply here too.
	void slide_now(int direction)
	{
		obs_source_t *scene = current_scene();
		obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData());
		if (!item) {
			say(tr_text("Dock.PickItem"));
			obs_source_release(scene);
			return;
		}
		// What travels with it is not in its way.
		std::vector<obs_sceneitem_t *> along;
		const std::string linked = linked_->currentData().toString().toUtf8().toStdString();
		if (obs_sceneitem_t *brought = linked.empty() ? nullptr : find_item(scene, linked))
			along.push_back(brought);
		refresh_item(item);
		const Measure how = arrange_options().how;
		const std::string scene_name = obs_source_get_name(scene) ? obs_source_get_name(scene) : "";
		// Pressed again while the item is still on its way: this slide goes
		// on from where the last one ends, so it passes that edge.
		const bool on_its_way = slide_item_ == obs_sceneitem_get_id(item) && slide_scene_ == scene_name &&
					slide_clock_.isValid() && slide_clock_.elapsed() < slide_ms_ + 150;
		const int gave_up_before = measuring_gave_up();
		const Box from = on_its_way ? slide_measured_ : measure_item(scene, item, how);
		const Box box = on_its_way ? slide_box_ : scene_box(scene, item);
		const Offset by = slide_offset(scene, item, direction % 3 - 1, direction / 3 - 1, margin_->value(), how,
					       along, &from);
		// OBS did not get round to measuring in time: better to say so than
		// to slide by the boxes and stop in the wrong place.
		if (measuring_gave_up() != gave_up_before) {
			say(tr_text("Dock.Busy"));
			obs_source_release(scene);
			return;
		}
		// For a report that a slide stopped in the wrong place: what was measured.
		obs_log(LOG_INFO,
			"Slide: '%s' counts as %.0f x %.0f at %.0f, %.0f (its box is %.0f x %.0f at %.0f, %.0f; "
			"visible pixels %s, shadows & outlines %s); moving %.0f, %.0f",
			item_->currentText().toUtf8().constData(), from.w, from.h, from.x, from.y, box.w, box.h, box.x,
			box.y, how.visible_pixels ? "on" : "off", how.leave_out_shadows ? "ignored" : "counted", by.x,
			by.y);
		if (std::fabs(by.x) < 0.5 && std::fabs(by.y) < 0.5) {
			say(tr_text("Dock.SlideNoRoom").arg(item_->currentText()));
			obs_source_release(scene);
			return;
		}
		// Where the item's box starts on the screen once it has moved that
		// far: the same spot in every scene, whatever group it is in there.
		slide_item_ = obs_sceneitem_get_id(item);
		slide_scene_ = scene_name;
		slide_measured_ = {from.x + by.x, from.y + by.y, from.w, from.h};
		slide_box_ = {box.x + by.x, box.y + by.y, box.w, box.h};
		slide_ms_ = duration_->value();
		slide_clock_.start();
		obs_data_t *settings = place_settings(kSpotDestination);
		obs_data_set_double(settings, "spot_x", slide_box_.x);
		obs_data_set_double(settings, "spot_y", slide_box_.y);
		obs_source_release(scene);
		send_with(settings, tr_text("Dock.Slid").arg(item_->currentText(), tr_text(kSlideText[direction])));
		obs_data_release(settings);
	}

	// Sends the chosen item with these Send To settings, as one step for
	// Undo, and says `done` when it is on its way.
	void send_with(obs_data_t *settings, const QString &done)
	{
		obs_source_t *source = item_source();
		if (!source) {
			say(tr_text("Dock.PickItem"));
			return;
		}
		// Everywhere, or only in the scene the dock is working on: by name,
		// as that may not be the one on screen (the preview in Studio Mode,
		// or another scene chosen in the dock).
		const bool everywhere = every_scene_->isChecked();
		obs_source_t *working = current_scene();
		const std::string scene_name = working && obs_source_get_name(working) ? obs_source_get_name(working)
										       : "";
		obs_data_set_string(settings, "in_scene", everywhere ? "" : scene_name.c_str());
		obs_source_t *filter = ensure_filter(source, kSendToId, kDockMoveFilter, settings);
		// One step for OBS's Undo, added when the item has finished gliding.
		// Presses made while it is still moving make one step together, from
		// where it was before the first of them.
		if (filter) {
			const bool chained = undo_chain_ && undo_clock_.isValid() &&
					     undo_clock_.elapsed() < undo_ms_ + 250 &&
					     undo_chain_->everywhere == everywhere && undo_chain_->scene == scene_name;
			if (!chained) {
				undo_chain_ = std::make_shared<UndoChain>();
				undo_chain_->before = everywhere ? transform_snapshot_all()
								 : transform_snapshot(working);
				undo_chain_->everywhere = everywhere;
				undo_chain_->scene = scene_name;
			}
			undo_clock_.start();
			undo_ms_ = duration_->value();
			const std::shared_ptr<UndoChain> chain = undo_chain_;
			const uint64_t serial = ++chain->latest;
			QTimer::singleShot(duration_->value() + 250, this, [chain, serial] {
				if (serial != chain->latest)
					return; // a later press in the same run adds the step
				obs_source_t *moved_in = obs_get_source_by_name(chain->scene.c_str());
				add_transform_undo(obs_module_text("Dock.SendUndo"), chain->before,
						   chain->everywhere ? transform_snapshot_all()
								     : transform_snapshot(moved_in));
				obs_source_release(moved_in);
			});
		}
		obs_source_release(working);
		if (filter) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(filter), "trigger", &cd);
			calldata_free(&cd);
			say(done);
		}
		obs_source_release(filter);
		obs_source_release(source);
	}

	// One key per screen position: Send To filters on the item's own source,
	// named after the position.
	void make_all_keys()
	{
		obs_source_t *source = item_source();
		if (!source) {
			say(tr_text("Dock.PickItem"));
			return;
		}
		for (int i = 0; i < 9; i++) {
			obs_data_t *settings = place_settings(i);
			obs_source_t *filter = ensure_filter(
				source, kSendToId, tr_text(kDestinationText[i]).toUtf8().toStdString(), settings);
			obs_data_release(settings);
			obs_source_release(filter);
		}
		say(tr_text("Dock.KeysMade").arg(item_->currentText()));
		obs_source_release(source);
	}

public:
	void save_spot_as(const QString &name)
	{
		obs_source_t *scene = current_scene();
		obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData());
		if (!item) {
			say(tr_text("Dock.PickItem"));
			obs_source_release(scene);
			return;
		}
		vec2 pos, scale;
		obs_sceneitem_get_pos(item, &pos);
		obs_sceneitem_get_scale(item, &scale);
		obs_data_t *settings = place_settings(9); // a saved spot
		obs_data_set_double(settings, "saved_x", pos.x);
		obs_data_set_double(settings, "saved_y", pos.y);
		obs_data_set_double(settings, "saved_scale_x", scale.x);
		obs_data_set_double(settings, "saved_scale_y", scale.y);
		obs_source_t *filter =
			ensure_filter(obs_sceneitem_get_source(item), kSendToId, name.toUtf8().toStdString(), settings);
		obs_data_release(settings);
		obs_source_release(filter);
		say(tr_text("Dock.SpotSaved").arg(name));
		obs_source_release(scene);
	}

private:
	void add_effect(const std::string &filter_id, bool on_scene)
	{
		obs_source_t *scene = current_scene();
		obs_sceneitem_t *item = find_item(scene, item_->currentText().toUtf8().constData());
		if (!item) {
			say(tr_text("Dock.PickItem"));
			obs_source_release(scene);
			return;
		}
		obs_source_t *owner = on_scene ? scene : obs_sceneitem_get_source(item);

		// A free name: "Ramp Blur", "Ramp Blur 2", ...
		const char *display = obs_source_get_display_name(filter_id.c_str());
		std::string base = display ? display : filter_id;
		const size_t suffix = base.find(" (StageKit)");
		if (suffix != std::string::npos)
			base.erase(suffix);
		if (on_scene)
			base += std::string(": ") + item_->currentText().toUtf8().constData();
		std::string name = base;
		for (int i = 2;; i++) {
			obs_source_t *existing = obs_source_get_filter_by_name(owner, name.c_str());
			if (!existing)
				break;
			obs_source_release(existing);
			name = base + " " + std::to_string(i);
		}

		obs_data_t *settings = obs_data_create();
		if (on_scene)
			obs_data_set_string(settings, "item", item_->currentText().toUtf8().constData());
		obs_source_t *filter = obs_source_create(filter_id.c_str(), name.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (filter) {
			obs_source_filter_add(owner, filter);
			// Ramp filters start in the "off" look so nothing changes on stream yet.
			if (filter_id == "stagekit_ramp_blur" || filter_id == "stagekit_ramp_zoom")
				obs_source_set_enabled(filter, false);
			say(tr_text("Dock.Added")
				    .arg(QString::fromUtf8(name.c_str()),
					 QString::fromUtf8(obs_source_get_name(owner))));
			if (fixed_scene_.empty())
				obs_frontend_open_source_filters(owner);
		}
		obs_source_release(filter);
		obs_source_release(scene);
	}

	std::string fixed_scene_;
	QString followed_;
	QComboBox *against_ = nullptr;
	QVBoxLayout *copy_rows_ = nullptr;
	QComboBox *copy_add_ = nullptr;
	QVBoxLayout *source_rows_ = nullptr;
	QComboBox *source_add_ = nullptr;
	QListWidget *arrange_list_ = nullptr;
	QStringList arrange_rows_;
	QCheckBox *visible_ = nullptr;
	QCheckBox *no_shadow_ = nullptr;
	QCheckBox *keep_shape_ = nullptr;
	uint64_t shown_options_ = 0;
	bool storing_ = false;
	QCheckBox *live_snap_ = nullptr;
	QCheckBox *limit_resize_ = nullptr;
	QComboBox *curve_ = nullptr;
	QComboBox *ease_mode_ = nullptr;
	QSpinBox *limit_px_ = nullptr;
	QCheckBox *guides_ = nullptr;
	QCheckBox *every_scene_ = nullptr;
	QCheckBox *slide_ = nullptr;
	// The dock's last slide, while the item may still be on its way there:
	// what was measured and the item's box, where they end up.
	int64_t slide_item_ = -1;
	std::string slide_scene_;
	Box slide_measured_, slide_box_;
	QElapsedTimer slide_clock_;
	int slide_ms_ = 0;
	// One Undo step for a run of moves made while the item is still moving:
	// from where things were before the first, to where the last left them.
	struct UndoChain {
		std::string before, scene;
		bool everywhere = false;
		uint64_t latest = 0;
	};
	std::shared_ptr<UndoChain> undo_chain_;
	QElapsedTimer undo_clock_;
	int undo_ms_ = 0;
	QSpinBox *snap_within_ = nullptr;
	// Each selected item's id with its place and size, at the last look.
	struct Watched {
		int64_t id = 0;
		float x = 0, y = 0;
		Box box;
		bool operator==(const Watched &o) const
		{
			return id == o.id && x == o.x && y == o.y && std::fabs(box.x - o.box.x) < 0.01 &&
			       std::fabs(box.y - o.box.y) < 0.01 && std::fabs(box.w - o.box.w) < 0.01 &&
			       std::fabs(box.h - o.box.h) < 0.01;
		}
	};
	std::vector<Watched> watched_;
	std::vector<Watched> drag_start_; // as they were when the drag began
	bool dragging_ = false;
	QWidget *options_measure_ = nullptr, *options_drop_ = nullptr;
	// The newer release found, and the row that says so.
	Release found_;
	QWidget *update_bar_ = nullptr;
	QLabel *update_text_ = nullptr;
	std::vector<Glide> glides_;
	QTimer *glide_timer_ = nullptr;
	QElapsedTimer glide_clock_;
	QStringList names_;
	QStringList scenes_;
	QStringList sources_;
	QComboBox *scene_ = nullptr;
	QComboBox *item_ = nullptr;
	QComboBox *linked_ = nullptr;
	QSpinBox *margin_ = nullptr;
	QSpinBox *duration_ = nullptr;
	QLabel *status_ = nullptr;
};

} // namespace

void register_dock()
{
	// For the headless tests: build the dock for a named scene.
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_create_dock(in string scene, out ptr widget)",
		[](void *, calldata_t *cd) {
			const char *scene = calldata_string(cd, "scene");
			calldata_set_ptr(cd, "widget", static_cast<QWidget *>(new Dock(scene ? scene : "")));
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_create_background_dock(out ptr widget)",
		[](void *, calldata_t *cd) {
			calldata_set_ptr(cd, "widget", static_cast<QWidget *>(new BackgroundDock()));
		},
		nullptr);
	// For the tests: what a hotkey or the preview's menu does, by the tool's name.
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_run_arrange(in string action)",
		[](void *, calldata_t *cd) {
			const char *name = calldata_string(cd, "action");
			for (int a = 0; name && a < (int)ArrangeAction::Count; a++)
				if (strcmp(arrange_info(static_cast<ArrangeAction>(a)).name, name) == 0)
					run_arrange_on_selection(static_cast<ArrangeAction>(a));
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_replay_arrange(in bool redo)",
		[](void *, calldata_t *cd) { replay_last_arrange(calldata_bool(cd, "redo")); }, nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_mouse_down(in bool down)",
		[](void *, calldata_t *cd) {
			g_test_mouse_down = calldata_bool(cd, "down");
			guides_test_mouse(g_test_mouse_down);
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_test_correct_glide(in int ms)",
		[](void *, calldata_t *cd) { g_test_correct_ms = (int)calldata_int(cd, "ms"); }, nullptr);
	// For the tests: the guide overlay for a stand-in preview widget, and
	// where OBS would draw the canvas in a preview of a given size.
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_install_preview_guides(in ptr preview, out ptr overlay)",
		[](void *, calldata_t *cd) {
			auto *preview = static_cast<QWidget *>(calldata_ptr(cd, "preview"));
			calldata_set_ptr(cd, "overlay",
					 static_cast<QObject *>(install_preview_guides(nullptr, preview)));
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_preview_map(in float width, in float height, in float ratio, in float canvas_width, "
		"in float canvas_height, out float scale, out float x, out float y)",
		[](void *, calldata_t *cd) {
			const PreviewMap map = preview_map(calldata_float(cd, "width"), calldata_float(cd, "height"),
							   calldata_float(cd, "ratio"),
							   calldata_float(cd, "canvas_width"),
							   calldata_float(cd, "canvas_height"));
			calldata_set_float(cd, "scale", map.scale);
			calldata_set_float(cd, "x", map.x);
			calldata_set_float(cd, "y", map.y);
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_install_preview_menu()",
		[](void *, calldata_t *) {
			if (!g_preview_menu && qApp) {
				g_preview_menu = new PreviewMenu(qApp);
				qApp->installEventFilter(g_preview_menu);
			}
		},
		nullptr);
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_dock_save_spot(in ptr widget, in string name)",
		[](void *, calldata_t *cd) {
			auto *dock = static_cast<Dock *>(static_cast<QWidget *>(calldata_ptr(cd, "widget")));
			if (dock)
				dock->save_spot_as(QString::fromUtf8(calldata_string(cd, "name")));
		},
		nullptr);

	// No main window means no OBS front end (the tests): nothing to dock into.
	if (!obs_frontend_get_main_window())
		return;
	// Each dock can hold more than fits a small screen: it scrolls instead
	// of forcing its window to be tall.
	auto add = [](const char *id, const char *title, QWidget *content) {
		auto *scroll = new QScrollArea();
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setWidget(content);
		if (!obs_frontend_add_dock_by_id(id, obs_module_text(title), scroll)) {
			obs_log(LOG_WARNING, "could not add the dock '%s'", id);
			delete scroll;
		}
	};
	{
		calldata_t cd = {};
		proc_handler_call(obs_get_proc_handler(), "stagekit_install_preview_menu", &cd);
		calldata_free(&cd);
	}
	install_preview_guides(static_cast<QWidget *>(obs_frontend_get_main_window()));
	// When OBS closes, everything here that watches on a timer or listens to
	// OBS stops first. Left running, it went on asking OBS about scenes and
	// measuring items while OBS was taking them apart.
	obs_frontend_add_event_callback(
		[](enum obs_frontend_event event, void *) {
			if (event != OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN && event != OBS_FRONTEND_EVENT_EXIT)
				return;
			if (measuring_stopped())
				return;
			measuring_stop();
			set_slide_handler(nullptr);
			remove_preview_guides();
			if (g_preview_menu && qApp)
				qApp->removeEventFilter(g_preview_menu);
			obs_log(LOG_INFO, "OBS is closing: the docks, guides and measuring have stopped");
		},
		nullptr);
	add("stagekit-dock", "Dock.Title", new Dock());
	add("stagekit-background-dock", "Dock.BackgroundTitle", new BackgroundDock());
}

} // namespace stagekit
