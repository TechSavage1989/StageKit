#include "component.hpp"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMap>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace stagekit::native {
namespace {
using Names = QMap<QString, QString>;
struct Pin {
	QString name, uuid;
};
QString field(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(obs_data_get_string(data, key));
}
QMainWindow *main_widget()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}
void add_name(Names &names, obs_source_t *source)
{
	if (!source || obs_obj_is_private(source) || obs_source_removed(source))
		return;
	names.insert(QString::fromUtf8(obs_source_get_name(source)), QString::fromUtf8(obs_source_get_uuid(source)));
}
Names all_sources()
{
	Names result;
	auto collect = [](void *p, obs_source_t *source) {
		add_name(*static_cast<Names *>(p), source);
		return true;
	};
	obs_enum_sources(collect, &result);
	obs_enum_scenes(collect, &result);
	return result;
}
struct SceneContents {
	Names names;
	QSet<obs_scene_t *> visited;
	void visit(obs_scene_t *scene)
	{
		if (!scene || visited.contains(scene))
			return;
		visited.insert(scene);
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *arg) {
				auto &contents = *static_cast<SceneContents *>(arg);
				auto *source = obs_sceneitem_get_source(item);
				add_name(contents.names, source);
				contents.visit(obs_sceneitem_is_group(item) ? obs_sceneitem_group_get_scene(item)
									    : obs_scene_from_source(source));
				return true;
			},
			this);
	}
};
OBSSourceAutoRelease by_id(const QString &uuid)
{
	return obs_get_source_by_uuid(uuid.toUtf8().constData());
}
bool matches(obs_source_t *source, const QString &query)
{
	if (query.isEmpty())
		return true;
	auto test = [&](const char *value) {
		return QString::fromUtf8(value).contains(query, Qt::CaseInsensitive);
	};
	if (test(obs_source_get_name(source)) || test(obs_source_get_id(source)) ||
	    test(obs_source_get_display_name(obs_source_get_id(source))))
		return true;
	QStringList filters;
	obs_source_enum_filters(
		source,
		[](obs_source_t *, obs_source_t *filter, void *data) {
			auto &list = *static_cast<QStringList *>(data);
			list << QString::fromUtf8(obs_source_get_name(filter))
			     << QString::fromUtf8(obs_source_get_id(filter));
		},
		&filters);
	for (const auto &item : filters)
		if (item.contains(query, Qt::CaseInsensitive))
			return true;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	for (const char *key : {"file", "local_file", "url", "input", "window", "capture_window", "text_file"})
		if (test(obs_data_get_string(settings, key)))
			return true;
	return false;
}

class Board : public QWidget {
public:
	QString id, title, mode;
	QList<Pin> pins;
	QLineEdit *search;
	QListWidget *list;
	bool properties, filters, scene_links, clickable;
	explicit Board(obs_data_t *data, QWidget *parent = nullptr)
		: QWidget(parent),
		  id(field(data, "dock_id")),
		  title(field(data, "dock_name")),
		  mode(field(data, "dock_type")),
		  properties(obs_data_get_bool(data, "show_properties")),
		  filters(obs_data_get_bool(data, "show_filters")),
		  scene_links(obs_data_get_bool(data, "show_scenes")),
		  clickable(obs_data_get_bool(data, "clickable_scenes"))
	{
		OBSDataArrayAutoRelease saved = obs_data_get_array(data, "dock_sources");
		for (size_t i = 0; i < obs_data_array_count(saved); ++i) {
			OBSDataAutoRelease item = obs_data_array_item(saved, i);
			pins.append(Pin{field(item, "source_name"), field(item, "source_uuid")});
		}
		auto *layout = new QVBoxLayout(this);
		search = new QLineEdit(this);
		search->setObjectName("nativeQuickSearch");
		search->setPlaceholderText(text("Native.FindSources"));
		layout->addWidget(search);
		list = new QListWidget(this);
		list->setObjectName("nativeQuickSources");
		layout->addWidget(list);
		auto *actions = new QHBoxLayout;
		layout->addLayout(actions);
		auto action = [&](const char *label, auto fn) {
			auto *button = new QPushButton(text(label), this);
			actions->addWidget(button);
			connect(button, &QPushButton::clicked, this, fn);
			return button;
		};
		if (properties)
			action("Native.Properties", [this] {
				auto s = selected();
				if (s)
					obs_frontend_open_source_properties(s);
			});
		if (filters)
			action("Native.Filters", [this] {
				auto s = selected();
				if (s)
					obs_frontend_open_source_filters(s);
			});
		if (scene_links) {
			auto *scene = action("Native.InScenes", [this] {
				auto s = selected();
				if (!s)
					return;
				QStringList found;
				obs_frontend_source_list scenes{};
				obs_frontend_get_scenes(&scenes);
				for (size_t i = 0; i < scenes.sources.num; ++i) {
					auto *candidate = scenes.sources.array[i];
					SceneContents contents;
					contents.visit(obs_scene_from_source(candidate));
					if (candidate == s.Get() ||
					    contents.names.values().contains(QString::fromUtf8(obs_source_get_uuid(s))))
						found.append(QString::fromUtf8(obs_source_get_name(candidate)));
				}
				obs_frontend_source_list_free(&scenes);
				if (found.isEmpty())
					return;
				if (!clickable) {
					QMessageBox::information(this, text("Native.InScenes"), found.join("\n"));
					return;
				}
				bool ok = false;
				auto chosen = QInputDialog::getItem(this, text("Native.InScenes"),
								    text("Native.SwitchScene"), found, 0, false, &ok);
				OBSSourceAutoRelease next = ok ? obs_get_source_by_name(chosen.toUtf8().constData())
							       : nullptr;
				if (next && obs_scene_from_source(next))
					obs_frontend_set_current_scene(next);
			});
			scene->setObjectName("nativeQuickScenes");
		}
		if (mode == "Manual") {
			auto *pin = action("Native.Pin", [this] {
				auto names = all_sources();
				bool ok = false;
				const auto name = QInputDialog::getItem(this, text("Native.Pin"), text("Native.Source"),
									names.keys(), 0, false, &ok);
				if (!ok || !names.contains(name))
					return;
				for (const auto &item : pins)
					if (item.uuid == names.value(name) ||
					    (item.uuid.isEmpty() && item.name == name))
						return;
				pins.append(Pin{name, names.value(name)});
				refresh();
				obs_frontend_save();
			});
			pin->setObjectName("nativeQuickPin");
			action("Native.Unpin", [this] {
				auto s = selected();
				if (!s)
					return;
				for (auto i = pins.begin(); i != pins.end(); ++i) {
					if (i->uuid == QString::fromUtf8(obs_source_get_uuid(s))) {
						pins.erase(i);
						break;
					}
				}
				refresh();
				obs_frontend_save();
			});
		}
		connect(search, &QLineEdit::textChanged, this, [this] { refresh(); });
		connect(list, &QListWidget::itemDoubleClicked, this, [this] {
			auto s = selected();
			if (!s)
				return;
			if (clickable && obs_scene_from_source(s))
				obs_frontend_set_current_scene(s);
			else if (properties)
				obs_frontend_open_source_properties(s);
			else if (filters)
				obs_frontend_open_source_filters(s);
		});
		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] {
			if (isVisible())
				refresh();
		});
		timer->start(1200);
		refresh();
	}
	OBSSourceAutoRelease selected() const
	{
		return list->currentItem() ? by_id(list->currentItem()->data(Qt::UserRole).toString())
					   : OBSSourceAutoRelease{};
	}
	void refresh()
	{
		Names values;
		if (mode == "Manual") {
			for (auto &p : pins) {
				OBSSourceAutoRelease source =
					p.uuid.isEmpty() ? obs_get_source_by_name(p.name.toUtf8().constData())
							 : obs_get_source_by_uuid(p.uuid.toUtf8().constData());
				if (!source)
					continue;
				p.name = QString::fromUtf8(obs_source_get_name(source));
				p.uuid = QString::fromUtf8(obs_source_get_uuid(source));
				values.insert(p.name, p.uuid);
			}
		} else if (mode == "Dynamic") {
			OBSSourceAutoRelease current = obs_frontend_get_current_scene();
			SceneContents entries;
			entries.visit(obs_scene_from_source(current));
			values = entries.names;
		} else
			values = all_sources();
		for (auto it = values.begin(); it != values.end();) {
			auto source = by_id(it.value());
			if (!source || !matches(source, search->text()))
				it = values.erase(it);
			else
				++it;
		}
		if (shown == values)
			return;
		auto current = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
		shown = values;
		list->clear();
		for (auto it = values.cbegin(); it != values.cend(); ++it) {
			auto *row = new QListWidgetItem(it.key(), list);
			row->setData(Qt::UserRole, it.value());
			if (current == it.value())
				list->setCurrentItem(row);
		}
	}
	QDockWidget *dock() const { return qobject_cast<QDockWidget *>(parentWidget()); }
	QByteArray dock_key() const { return ("stagekit-quick-access-dock_" + id).toUtf8(); }
	OBSDataAutoRelease save() const
	{
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "dock_id", id.toUtf8().constData());
		obs_data_set_string(data, "dock_name", title.toUtf8().constData());
		obs_data_set_string(data, "dock_type", mode.toUtf8().constData());
		obs_data_set_bool(data, "show_properties", properties);
		obs_data_set_bool(data, "show_filters", filters);
		obs_data_set_bool(data, "show_scenes", scene_links);
		obs_data_set_bool(data, "clickable_scenes", clickable);
		OBSDataArrayAutoRelease entries = obs_data_array_create();
		for (const auto &p : pins) {
			OBSDataAutoRelease item = obs_data_create();
			obs_data_set_string(item, "source_name", p.name.toUtf8().constData());
			obs_data_set_string(item, "source_uuid", p.uuid.toUtf8().constData());
			obs_data_array_push_back(entries, item);
		}
		obs_data_set_array(data, "dock_sources", entries);
		if (auto *widget = dock()) {
			obs_data_set_bool(data, "dock_hidden", widget->isHidden());
			obs_data_set_bool(data, "dock_floating", widget->isFloating());
			obs_data_set_string(data, "dock_geometry", widget->saveGeometry().toBase64().constData());
			obs_data_set_int(data, "dock_area", main_widget()->dockWidgetArea(widget));
		}
		return data;
	}

private:
	Names shown;
};
QList<QPointer<Board>> boards;
QPointer<QObject> owner;
obs_hotkey_id key = OBS_INVALID_HOTKEY_ID;
bool add_board(obs_data_t *data)
{
	auto id = field(data, "dock_id"), title = field(data, "dock_name");
	if (!main_widget() || id.isEmpty() || title.isEmpty())
		return false;
	for (const auto &old : boards)
		if (old && old->id == id)
			return false;
	auto *board = new Board(data);
	if (!obs_frontend_add_dock_by_id(board->dock_key().constData(), title.toUtf8().constData(), board)) {
		delete board;
		return false;
	}
	boards.push_back(board);
	if (auto *widget = board->dock()) {
		auto area = static_cast<Qt::DockWidgetArea>(obs_data_get_int(data, "dock_area"));
		if (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea ||
		    area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea)
			main_widget()->addDockWidget(area, widget);
		widget->setFloating(obs_data_get_bool(data, "dock_floating"));
		auto bytes = QByteArray::fromBase64(field(data, "dock_geometry").toLatin1());
		if (!bytes.isEmpty())
			widget->restoreGeometry(bytes);
		widget->setVisible(!obs_data_get_bool(data, "dock_hidden"));
	}
	return true;
}
void clear_boards()
{
	const auto old = boards;
	boards.clear();
	for (const auto &b : old)
		if (b)
			obs_frontend_remove_dock(b->dock_key().constData());
}
OBSDataAutoRelease fresh(const QString &title, const QString &mode)
{
	OBSDataAutoRelease d = obs_data_create();
	obs_data_set_string(d, "dock_name", title.toUtf8().constData());
	obs_data_set_string(d, "dock_type", mode.toUtf8().constData());
	obs_data_set_string(d, "dock_id", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8().constData());
	for (const auto *field :
	     {"show_properties", "show_filters", "show_scenes", "clickable_scenes", "dock_floating"})
		obs_data_set_bool(d, field, true);
	return d;
}
void search_window()
{
	auto config = fresh(text("Native.SearchSources"), "Source Search");
	QDialog dialog(main_widget());
	dialog.setWindowTitle(text("Native.SearchSources"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *board = new Board(config, &dialog);
	layout->addWidget(board);
	board->search->setFocus();
	dialog.resize(610, 460);
	dialog.exec();
}
void manager()
{
	QDialog dialog(main_widget());
	dialog.setWindowTitle(text("Ports.Tool.quick"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *list = new QListWidget(&dialog);
	list->setObjectName("nativeQuickBoards");
	layout->addWidget(list);
	auto redraw = [&] {
		list->clear();
		for (const auto &board : boards) {
			if (!board)
				continue;
			auto *row = new QListWidgetItem(board->title, list);
			row->setData(Qt::UserRole, board->id);
		}
	};
	redraw();
	auto *form = new QFormLayout;
	layout->addLayout(form);
	auto *name = new QLineEdit(&dialog);
	name->setObjectName("nativeQuickTitle");
	form->addRow(text("Native.DockName"), name);
	auto *mode = new QComboBox(&dialog);
	mode->addItem(text("Native.AllSources"), "Source Search");
	mode->addItem(text("Native.CurrentScene"), "Dynamic");
	mode->addItem(text("Native.PinnedSources"), "Manual");
	form->addRow(text("Native.SourceList"), mode);
	auto *options = new QHBoxLayout;
	layout->addLayout(options);
	auto check = [&](const char *key) {
		auto *box = new QCheckBox(text(key), &dialog);
		box->setChecked(true);
		options->addWidget(box);
		return box;
	};
	auto *props = check("Native.Properties"), *filters = check("Native.Filters"),
	     *scenes = check("Native.InScenes"), *switching = check("Native.SwitchScene");
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	layout->addWidget(buttons);
	auto button = [&](const char *key, auto fn) {
		auto *b = buttons->addButton(text(key), QDialogButtonBox::ActionRole);
		QObject::connect(b, &QPushButton::clicked, &dialog, fn);
		return b;
	};
	button("Native.SearchSources", search_window);
	button("Native.CreateDock", [&] {
		if (name->text().trimmed().isEmpty())
			return;
		auto config = fresh(name->text().trimmed(), mode->currentData().toString());
		obs_data_set_bool(config, "show_properties", props->isChecked());
		obs_data_set_bool(config, "show_filters", filters->isChecked());
		obs_data_set_bool(config, "show_scenes", scenes->isChecked());
		obs_data_set_bool(config, "clickable_scenes", switching->isChecked());
		if (add_board(config)) {
			name->clear();
			redraw();
			obs_frontend_save();
		}
	})->setObjectName("nativeCreateQuickDock");
	auto selected = [&]() -> Board * {
		if (!list->currentItem())
			return nullptr;
		for (const auto &b : boards)
			if (b && b->id == list->currentItem()->data(Qt::UserRole).toString())
				return b.data();
		return nullptr;
	};
	button("Native.ShowDock", [&] {
		auto *b = selected();
		if (b && b->dock()) {
			b->dock()->show();
			b->dock()->raise();
		}
	});
	button("Native.Remove", [&] {
		auto *b = selected();
		if (!b || QMessageBox::question(&dialog, dialog.windowTitle(),
						QString(text("Native.RemoveDock")).arg(b->title)) != QMessageBox::Yes)
			return;
		boards.removeAll(b);
		obs_frontend_remove_dock(b->dock_key().constData());
		redraw();
		obs_frontend_save();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	dialog.resize(600, 380);
	dialog.exec();
}
void persist(obs_data_t *root, bool saving, void *)
{
	if (saving) {
		OBSDataAutoRelease data = obs_data_create();
		OBSDataArrayAutoRelease array = obs_data_array_create();
		for (const auto &b : boards)
			if (b) {
				auto item = b->save();
				obs_data_array_push_back(array, item);
			}
		obs_data_set_bool(data, "first_run", false);
		obs_data_set_array(data, "docks", array);
		OBSDataArrayAutoRelease binding = obs_hotkey_save(key);
		if (binding)
			obs_data_set_array(data, "quick_search_hotkey", binding);
		obs_data_set_obj(root, "stagekit.quick_access", data);
	} else {
		clear_boards();
		OBSDataAutoRelease data = obs_data_get_obj(root, "stagekit.quick_access");
		OBSDataArrayAutoRelease binding = obs_data_get_array(data, "quick_search_hotkey");
		obs_hotkey_load(key, binding);
		OBSDataArrayAutoRelease entries = obs_data_get_array(data, "docks");
		for (size_t i = 0; i < obs_data_array_count(entries); ++i) {
			OBSDataAutoRelease entry = obs_data_array_item(entries, i);
			add_board(entry);
		}
	}
}
void event(obs_frontend_event which, void *)
{
	if (which == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || which == OBS_FRONTEND_EVENT_EXIT)
		clear_boards();
}
void load()
{
	owner = new QObject(main_widget());
	auto *menu = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(text("Ports.Tool.quick")));
	if (menu)
		QObject::connect(menu, &QAction::triggered, owner, manager);
	key = obs_hotkey_register_frontend(
		"Quick Search", text("Native.SearchSources"),
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool down) {
			if (down && owner)
				QMetaObject::invokeMethod(owner, search_window, Qt::QueuedConnection);
		},
		nullptr);
	obs_frontend_add_save_callback(persist, nullptr);
	obs_frontend_add_event_callback(event, nullptr);
}
void unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	obs_frontend_remove_save_callback(persist, nullptr);
	obs_hotkey_unregister(key);
	clear_boards();
	delete owner;
	owner = nullptr;
}
} // namespace
const Component component{load, unload};
} // namespace stagekit::native
