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
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <cstring>
#include <functional>
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
			QAction *entry = sub->addAction(tr_text(text));
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
	}

private:
	QElapsedTimer asked_;
};

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
		QObject::connect(timer, &QTimer::timeout, this, [this] { refresh(); });
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

		auto *form = new QFormLayout();
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
		form->addRow(tr_text("Place.Linked"), linked_);
		layout->addLayout(form);

		auto *move_box = new QGroupBox(tr_text("Dock.MoveBox"), this);
		auto *move_layout = new QVBoxLayout(move_box);
		auto *grid = new QGridLayout();
		for (int i = 0; i < 9; i++) {
			auto *button = new QPushButton(QString::fromUtf8(kArrows[i]), move_box);
			button->setObjectName(QString("grid%1").arg(i));
			button->setToolTip(tr_text(kDestinationText[i]));
			button->setMinimumHeight(34);
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
		options->addRow(tr_text("Place.Margin"), margin_);
		options->addRow(tr_text("Place.Duration"), duration_);
		move_layout->addLayout(options);
		every_scene_ = new QCheckBox(tr_text("Dock.SendEvery"), move_box);
		every_scene_->setObjectName("sendEveryScene");
		every_scene_->setToolTip(tr_text("Dock.SendEvery.Tip"));
		every_scene_->setChecked(true);
		move_layout->addWidget(every_scene_);
		layout->addWidget(move_box);

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
		layout->addWidget(keys_box);

		auto *add_box = new QGroupBox(tr_text("Dock.AddBox"), this);
		auto *add_layout = new QGridLayout(add_box);
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
			add_layout->addWidget(button, n / 2, n % 2);
			n++;
		}
		layout->addWidget(add_box);

		// Lining up, spacing and snapping whatever is selected in OBS.
		auto *arrange_box = new QGroupBox(tr_text("Arrange.Box"), this);
		auto *arrange_layout = new QVBoxLayout(arrange_box);
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
		for (int a = 0; a < (int)ArrangeAction::Count; a++) {
			const auto action = static_cast<ArrangeAction>(a);
			const ArrangeInfo &info = arrange_info(action);
			auto *button = new QPushButton(tr_text(info.text), arrange_box);
			button->setObjectName(QString("arrange_") + info.name);
			button->setToolTip(tr_text((std::string(info.text) + ".Tip").c_str()));
			QObject::connect(button, &QPushButton::clicked, this, [this, action] {
				obs_source_t *scene = current_scene();
				const ArrangeOutcome out = run_arrange(action, scene, arrange_items(scene));
				obs_source_release(scene);
				say(arrange_message(out));
			});
			arrange_grid->addWidget(button, a / 3, a % 3);
		}
		arrange_layout->addLayout(arrange_grid);
		visible_ = new QCheckBox(tr_text("Arrange.Visible"), arrange_box);
		visible_->setObjectName("arrangeVisible");
		visible_->setToolTip(tr_text("Arrange.Visible.Tip"));
		no_shadow_ = new QCheckBox(tr_text("Arrange.NoShadow"), arrange_box);
		no_shadow_->setObjectName("arrangeNoShadow");
		keep_shape_ = new QCheckBox(tr_text("Arrange.KeepShape"), arrange_box);
		keep_shape_->setObjectName("arrangeKeepShape");
		keep_shape_->setToolTip(tr_text("Arrange.KeepShape.Tip"));
		live_snap_ = new QCheckBox(tr_text("Arrange.LiveSnap"), arrange_box);
		live_snap_->setObjectName("arrangeLiveSnap");
		live_snap_->setToolTip(tr_text("Arrange.LiveSnap.Tip"));
		guides_ = new QCheckBox(tr_text("Arrange.Guides"), arrange_box);
		guides_->setObjectName("arrangeGuides");
		guides_->setToolTip(tr_text("Arrange.Guides.Tip"));
		auto *within_row = new QFormLayout();
		snap_within_ = new QSpinBox(arrange_box);
		snap_within_->setObjectName("snapWithin");
		snap_within_->setRange(1, 200);
		snap_within_->setSuffix(" px");
		within_row->addRow(tr_text("Arrange.Within"), snap_within_);
		arrange_layout->addWidget(visible_);
		arrange_layout->addWidget(no_shadow_);
		arrange_layout->addWidget(keep_shape_);
		arrange_layout->addWidget(live_snap_);
		arrange_layout->addWidget(guides_);
		arrange_layout->addLayout(within_row);
		// The options are shared with the hotkeys and the right-click menu.
		show_arrange_options();
		for (QCheckBox *box : {visible_, no_shadow_, keep_shape_, live_snap_, guides_, every_scene_})
			QObject::connect(box, &QCheckBox::toggled, this, [this](bool) { store_arrange_options(); });
		QObject::connect(against_, &QComboBox::currentIndexChanged, this,
				 [this](int) { store_arrange_options(); });
		QObject::connect(snap_within_, &QSpinBox::valueChanged, this, [this](int) { store_arrange_options(); });
		layout->addWidget(arrange_box);

		// Snapping on letting go: watch the selected items a few times a second.
		auto *snap_timer = new QTimer(this);
		QObject::connect(snap_timer, &QTimer::timeout, this, [this] { watch_for_drop(); });
		snap_timer->start(120);

		auto *order_box = new QGroupBox(tr_text("Dock.OrderBox"), this);
		auto *order_layout = new QGridLayout(order_box);
		struct {
			const char *object_name, *text;
			obs_order_movement movement;
		} orders[] = {
			{"orderTop", "Dock.OrderTop", OBS_ORDER_MOVE_TOP},
			{"orderUp", "Dock.OrderUp", OBS_ORDER_MOVE_UP},
			{"orderDown", "Dock.OrderDown", OBS_ORDER_MOVE_DOWN},
			{"orderBottom", "Dock.OrderBottom", OBS_ORDER_MOVE_BOTTOM},
		};
		int order_index = 0;
		for (const auto &order : orders) {
			auto *button = new QPushButton(tr_text(order.text), order_box);
			button->setObjectName(order.object_name);
			const obs_order_movement movement = order.movement;
			QObject::connect(button, &QPushButton::clicked, this, [this, movement] { reorder(movement); });
			order_layout->addWidget(button, order_index / 2, order_index % 2);
			order_index++;
		}
		layout->addWidget(order_box);

		status_ = new QLabel(this);
		status_->setObjectName("status");
		status_->setWordWrap(true);
		layout->addWidget(status_);
		layout->addStretch(1);

		// Scenes and their items change behind our back; a light poll keeps
		// the lists current without hooking every scene signal.
		auto *timer = new QTimer(this);
		QObject::connect(timer, &QTimer::timeout, this, [this] { refresh(); });
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
		if (scenes != scenes_) {
			scenes_ = scenes;
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

	// Lists what the Arrange buttons will act on, and marks the item the
	// others will come to when lining up to the first or last selected.
	void show_arrange_target()
	{
		obs_source_t *scene = current_scene();
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
		guides_->setChecked(o.guides);
		every_scene_->setChecked(o.send_every_scene);
		snap_within_->setValue(o.within);
		storing_ = false;
	}

	void store_arrange_options()
	{
		if (storing_)
			return;
		ArrangeOptions o;
		o.against = static_cast<Against>(against_->currentData().toInt());
		o.how.visible_pixels = visible_->isChecked();
		o.how.leave_out_shadows = no_shadow_->isChecked();
		o.keep_shape = keep_shape_->isChecked();
		o.live_snap = live_snap_->isChecked();
		o.guides = guides_->isChecked();
		o.send_every_scene = every_scene_->isChecked();
		o.within = snap_within_->value();
		set_arrange_options(o);
		shown_options_ = arrange_options_revision();
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
	void watch_for_drop()
	{
		// Looked at often, so the order items are selected in is noticed.
		show_arrange_target();
		const ArrangeOptions options = arrange_options();
		if (!options.live_snap) {
			watched_.clear();
			return;
		}
		obs_source_t *scene = current_scene();
		const std::vector<obs_sceneitem_t *> items = selected_items(scene);
		std::vector<std::pair<int64_t, std::pair<float, float>>> now;
		for (obs_sceneitem_t *item : items) {
			vec2 pos;
			obs_sceneitem_get_pos(item, &pos);
			now.push_back({obs_sceneitem_get_id(item), {pos.x, pos.y}});
		}
		const bool same_items = now.size() == watched_.size() &&
					std::equal(now.begin(), now.end(), watched_.begin(),
						   [](const auto &a, const auto &b) { return a.first == b.first; });
		const bool moved = same_items && now != watched_;
		const bool button_up = !g_test_mouse_down && QGuiApplication::mouseButtons() == Qt::NoButton;
		// Only a drag with the mouse counts. Items moved by a key, a filter or
		// a Stream Deck button are where they were sent and must stay there.
		if (moved && !button_up)
			dragging_ = true;
		if (dragging_ && !moved && button_up) {
			dragging_ = false;
			const std::string before = transform_snapshot(scene);
			bool snapped = false;
			for (obs_sceneitem_t *item : items)
				snapped = snap_item(scene, item, options.within, options.how) || snapped;
			if (snapped)
				add_transform_undo(obs_module_text("Hotkey.Arrange.SnapNow"), before,
						   transform_snapshot(scene));
			now.clear();
			for (obs_sceneitem_t *item : items) {
				vec2 pos;
				obs_sceneitem_get_pos(item, &pos);
				now.push_back({obs_sceneitem_get_id(item), {pos.x, pos.y}});
			}
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
				if (out->isEmpty() && obs_sceneitem_selected(item) && !obs_sceneitem_is_group(item))
					*out = QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)));
				return true;
			};
			obs_scene_enum_items(
				s,
				[](obs_scene_t *sc, obs_sceneitem_t *item, void *param) {
					if (obs_sceneitem_is_group(item))
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
		obs_source_t *source = item_source();
		if (!source) {
			say(tr_text("Dock.PickItem"));
			return;
		}
		obs_data_t *settings = place_settings(destination);
		// Everywhere, or only in the scene the dock is working on, which may
		// not be the one on screen.
		const bool everywhere = every_scene_->isChecked();
		const QString chosen = scene_->currentData().toString();
		const std::string in_scene = everywhere          ? std::string()
					     : !chosen.isEmpty() ? chosen.toUtf8().toStdString()
								 : fixed_scene_;
		obs_data_set_string(settings, "in_scene", in_scene.c_str());
		obs_source_t *filter = ensure_filter(source, kSendToId, kDockMoveFilter, settings);
		obs_data_release(settings);
		// One step for OBS's Undo, added when the item has finished gliding.
		if (filter) {
			obs_source_t *scene = current_scene();
			const std::string scene_name = scene ? obs_source_get_name(scene) : "";
			const std::string before = everywhere ? transform_snapshot_all() : transform_snapshot(scene);
			obs_source_release(scene);
			QTimer::singleShot(duration_->value() + 250, this, [before, scene_name, everywhere] {
				obs_source_t *moved_in = obs_get_source_by_name(scene_name.c_str());
				add_transform_undo(obs_module_text("Dock.SendUndo"), before,
						   everywhere ? transform_snapshot_all()
							      : transform_snapshot(moved_in));
				obs_source_release(moved_in);
			});
		}
		if (filter) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(filter), "trigger", &cd);
			calldata_free(&cd);
			say(tr_text("Dock.Moved").arg(item_->currentText(), tr_text(kDestinationText[destination])));
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
	QListWidget *arrange_list_ = nullptr;
	QStringList arrange_rows_;
	QCheckBox *visible_ = nullptr;
	QCheckBox *no_shadow_ = nullptr;
	QCheckBox *keep_shape_ = nullptr;
	uint64_t shown_options_ = 0;
	bool storing_ = false;
	QCheckBox *live_snap_ = nullptr;
	QCheckBox *guides_ = nullptr;
	QCheckBox *every_scene_ = nullptr;
	QSpinBox *snap_within_ = nullptr;
	std::vector<std::pair<int64_t, std::pair<float, float>>> watched_;
	bool dragging_ = false;
	QStringList names_;
	QStringList scenes_;
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
	// For the tests: the guide overlay for a stand-in preview widget, and
	// where OBS would draw the canvas in a preview of a given size.
	proc_handler_add(
		obs_get_proc_handler(), "void stagekit_install_preview_guides(in ptr preview, out ptr overlay)",
		[](void *, calldata_t *cd) {
			auto *preview = static_cast<QWidget *>(calldata_ptr(cd, "preview"));
			calldata_set_ptr(cd, "overlay", install_preview_guides(nullptr, preview));
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
			static PreviewMenu *menu = nullptr;
			if (!menu && qApp) {
				menu = new PreviewMenu(qApp);
				qApp->installEventFilter(menu);
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
	add("stagekit-dock", "Dock.Title", new Dock());
	add("stagekit-background-dock", "Dock.BackgroundTitle", new BackgroundDock());
}

} // namespace stagekit
