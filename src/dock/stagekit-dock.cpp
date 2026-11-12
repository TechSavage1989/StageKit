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
//   - a small picture of the scene: drag an item to move it, turn the wheel
//     to resize it, drag the cross to set where its Ramp Zoom zooms in
//
// Moves are carried out by Place Item filters on the scene, so the dock and
// the keys always behave the same way.

#include "stagekit-dock.hpp"

#include "dock-canvas.hpp"

#include "../common/scene-util.hpp"
#include "../common/start-image.hpp"
#include "../placement/place-item.hpp"
#include "../placement/scene-tools.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
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
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

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

		// A picture of the scene to work on directly.
		canvas_ = new DockCanvas(this);
		canvas_->setObjectName("canvas");
		canvas_->scene = [this] {
			return current_scene();
		};
		canvas_->selected = [this] {
			return item_->currentText();
		};
		canvas_->on_pick = [this](const QString &name) {
			const int index = item_->findText(name);
			if (index >= 0)
				item_->setCurrentIndex(index);
		};
		QObject::connect(item_, &QComboBox::currentIndexChanged, canvas_, [this](int) { canvas_->update(); });
		layout->addWidget(canvas_);
		auto *live = new QCheckBox(tr_text("Dock.CanvasLive"), this);
		live->setObjectName("canvasLive");
		live->setChecked(true);
		QObject::connect(live, &QCheckBox::toggled, canvas_,
				 [this](bool on) { canvas_->set_live_picture(on); });
		layout->addWidget(live);
		auto *canvas_hint = new QLabel(tr_text("Dock.CanvasHint"), this);
		canvas_hint->setWordWrap(true);
		layout->addWidget(canvas_hint);

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
		auto *keys_hint = new QLabel(tr_text("Dock.KeysHint"), keys_box);
		keys_hint->setWordWrap(true);
		keys_layout->addWidget(make_keys);
		keys_layout->addWidget(save_spot);
		keys_layout->addWidget(keys_hint);
		layout->addWidget(keys_box);

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
		start_thumb_ = new QLabel(start_box);
		start_thumb_->setObjectName("startThumb");
		start_thumb_->setAlignment(Qt::AlignCenter);
		start_thumb_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		start_thumb_->setFixedHeight(kThumbHeight);
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
		start_layout->addWidget(start_thumb_);
		start_layout->addWidget(start_label_);
		start_layout->addWidget(choose);
		layout->addWidget(start_box);

		auto *add_box = new QGroupBox(tr_text("Dock.AddBox"), this);
		auto *add_layout = new QGridLayout(add_box);
		struct {
			const char *object_name, *text, *filter_id;
			bool on_scene;
		} effects[] = {
			{"addBlur", "Dock.AddBlur", "stagekit_ramp_blur", false},
			{"addShadow", "Dock.AddShadow", "stagekit_shadow_backdrop", false},
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
		timer->start(1000);
		refresh();
	}

	void refresh()
	{
		const QString start = QString::fromUtf8(start_image_path().c_str());
		QString name = start.isEmpty() ? tr_text("Dock.StartNone") : QFileInfo(start).fileName();
		if (name.size() > 40)
			name = name.left(22) + QStringLiteral("…") + name.right(16);
		start_label_->setText(name);
		start_label_->setToolTip(start);
		if (start != thumb_path_) {
			// Read the picture already shrunk, so a large image costs little.
			thumb_path_ = start;
			QPixmap picture;
			if (!start.isEmpty()) {
				QImageReader reader(start);
				reader.setAutoTransform(true);
				const QSize size = reader.size();
				if (size.isValid())
					reader.setScaledSize(
						size.scaled(kThumbHeight * 3, kThumbHeight, Qt::KeepAspectRatio));
				picture = QPixmap::fromImageReader(&reader);
				if (!picture.isNull() && picture.height() > kThumbHeight)
					picture = picture.scaledToHeight(kThumbHeight, Qt::SmoothTransformation);
			}
			start_thumb_->setPixmap(picture);
			start_thumb_->setVisible(!picture.isNull());
			start_thumb_->setToolTip(start);
		}

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
		obs_data_set_string(d, "item", item_->currentText().toUtf8().constData());
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
		obs_source_t *scene = current_scene();
		if (!scene || item_->currentText().isEmpty()) {
			say(tr_text("Dock.PickItem"));
			obs_source_release(scene);
			return;
		}
		obs_data_t *settings = place_settings(destination);
		obs_source_t *filter = ensure_filter(scene, kPlaceItemId, kDockMoveFilter, settings);
		obs_data_release(settings);
		if (filter) {
			calldata_t cd = {};
			proc_handler_call(obs_source_get_proc_handler(filter), "trigger", &cd);
			calldata_free(&cd);
			say(tr_text("Dock.Moved").arg(item_->currentText(), tr_text(kDestinationText[destination])));
		}
		obs_source_release(filter);
		obs_source_release(scene);
	}

	std::string key_name(const QString &place) const
	{
		return (item_->currentText() + " → " + place).toUtf8().constData();
	}

	void make_all_keys()
	{
		obs_source_t *scene = current_scene();
		if (!scene || item_->currentText().isEmpty()) {
			say(tr_text("Dock.PickItem"));
			obs_source_release(scene);
			return;
		}
		for (int i = 0; i < 9; i++) {
			obs_data_t *settings = place_settings(i);
			obs_source_t *filter =
				ensure_filter(scene, kPlaceItemId, key_name(tr_text(kDestinationText[i])), settings);
			obs_data_release(settings);
			obs_source_release(filter);
		}
		say(tr_text("Dock.KeysMade").arg(item_->currentText()));
		obs_source_release(scene);
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
		obs_source_t *filter = ensure_filter(scene, kPlaceItemId, key_name(name), settings);
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
	static constexpr int kThumbHeight = 120;
	QLabel *start_label_ = nullptr;
	DockCanvas *canvas_ = nullptr;
	QLabel *start_thumb_ = nullptr;
	QString thumb_path_ = QStringLiteral("\x01"); // nothing loaded yet
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
	// The dock has more in it than fits most screens: it scrolls instead of
	// forcing its window to be tall.
	auto *scroll = new QScrollArea();
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scroll->setWidget(new Dock());
	if (!obs_frontend_add_dock_by_id("stagekit-dock", obs_module_text("Dock.Title"), scroll)) {
		obs_log(LOG_WARNING, "could not add the StageKit dock");
		delete scroll;
	}
}

} // namespace stagekit
