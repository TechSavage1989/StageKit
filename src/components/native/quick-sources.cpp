#include "quick-sources.hpp"
#include "component.hpp"

#include <obs-frontend-api.h>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSet>
#include <QStyle>
#include <QToolButton>
#include <QWidgetAction>
#include <algorithm>

namespace stagekit::native::quick {
Source describe(obs_source_t *source)
{
	return {QString::fromUtf8(obs_source_get_name(source)), QString::fromUtf8(obs_source_get_uuid(source))};
}

OBSSourceAutoRelease resolve(const Source &source)
{
	if (source.uuid.isEmpty() && source.name.isEmpty())
		return {};
	OBSSourceAutoRelease found = source.uuid.isEmpty() ? obs_get_source_by_name(source.name.toUtf8().constData())
							 : obs_get_source_by_uuid(source.uuid.toUtf8().constData());
	if (found && !obs_source_removed(found))
		return found;
	return {};
}

QList<Source> available_sources()
{
	QList<Source> sources;
	auto collect = [](void *data, obs_source_t *source) {
		if (!obs_obj_is_private(source) && !obs_source_removed(source))
			static_cast<QList<Source> *>(data)->append(describe(source));
		return true;
	};
	obs_enum_sources(collect, &sources);
	obs_enum_scenes(collect, &sources);
	std::sort(sources.begin(), sources.end(), [](const Source &a, const Source &b) {
		return QString::localeAwareCompare(a.name, b.name) < 0;
	});
	return sources;
}

namespace {
struct SceneWalk {
	QList<Source> sources;
	QList<Source> &containers;
	QSet<QString> seen;
	void visit(obs_scene_t *scene)
	{
		if (!scene)
			return;
		containers.append(describe(obs_scene_get_source(scene)));
		obs_scene_enum_items(scene, [](obs_scene_t *, obs_sceneitem_t *item, void *data) {
			auto &walk = *static_cast<SceneWalk *>(data);
			auto *child = obs_sceneitem_get_source(item);
			if (!child || obs_source_removed(child) || obs_obj_is_private(child))
				return true;
			auto entry = describe(child);
			if (!walk.seen.contains(entry.uuid)) {
				walk.seen.insert(entry.uuid);
				walk.sources.append(entry);
				walk.visit(obs_group_or_scene_from_source(child));
			}
			return true;
		}, this);
	}
};
}

QList<Source> program_sources(QList<Source> &containers)
{
	OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
	SceneWalk walk{{}, containers, {}};
	walk.visit(obs_scene_from_source(scene));
	return walk.sources;
}

unsigned search_matches(obs_source_t *source, const QString &query)
{
	if (!source || query.isEmpty())
		return 0;
	auto contains = [&](const char *value) { return QString::fromUtf8(value).contains(query, Qt::CaseInsensitive); };
	unsigned result = 0;
	if (contains(obs_source_get_name(source)) || contains(obs_source_get_uuid(source)))
		result |= Name;
	const char *type = obs_source_get_id(source);
	if (contains(type) || contains(obs_source_get_display_name(type)))
		result |= Type;
	struct FilterQuery {
		const QString &query;
		bool found = false;
	} filters{query};
	obs_source_enum_filters(source, [](obs_source_t *, obs_source_t *filter, void *data) {
		auto &q = *static_cast<FilterQuery *>(data);
		const char *id = obs_source_get_id(filter);
		for (const char *field : {obs_source_get_name(filter), id, obs_source_get_display_name(id)})
			if (QString::fromUtf8(field).contains(q.query, Qt::CaseInsensitive))
				q.found = true;
	}, &filters);
	if (filters.found)
		result |= Filter;
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	if (qstrcmp(obs_source_get_unversioned_id(source), "ffmpeg_source") == 0) {
		const bool local = obs_data_get_bool(settings, "is_local_file");
		if (contains(obs_data_get_string(settings, local ? "local_file" : "input")))
			result |= local ? File : Url;
	} else {
		for (const char *key : {"file", "local_file", "text_file"})
			if (contains(obs_data_get_string(settings, key)))
				result |= File;
		if (contains(obs_data_get_string(settings, "url")))
			result |= Url;
	}
	for (const char *key : {"window", "capture_window"})
		if (contains(obs_data_get_string(settings, key)))
			result |= Name;
	return result;
}

QIcon source_icon(obs_source_t *source)
{
	if (!source)
		return {};
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	const char *property = "defaultIcon";
	if (obs_source_is_group(source))
		property = "groupIcon";
	else if (obs_source_is_scene(source))
		property = "sceneIcon";
	else {
		switch (obs_source_get_icon_type(obs_source_get_id(source))) {
		case OBS_ICON_TYPE_IMAGE: property = "imageIcon"; break;
		case OBS_ICON_TYPE_COLOR: property = "colorIcon"; break;
		case OBS_ICON_TYPE_SLIDESHOW: property = "slideshowIcon"; break;
		case OBS_ICON_TYPE_AUDIO_INPUT: property = "audioInputIcon"; break;
		case OBS_ICON_TYPE_AUDIO_OUTPUT: property = "audioOutputIcon"; break;
		case OBS_ICON_TYPE_DESKTOP_CAPTURE: property = "desktopCapIcon"; break;
		case OBS_ICON_TYPE_WINDOW_CAPTURE: property = "windowCapIcon"; break;
		case OBS_ICON_TYPE_GAME_CAPTURE: property = "gameCapIcon"; break;
		case OBS_ICON_TYPE_CAMERA: property = "cameraIcon"; break;
		case OBS_ICON_TYPE_TEXT: property = "textIcon"; break;
		case OBS_ICON_TYPE_MEDIA: property = "mediaIcon"; break;
		case OBS_ICON_TYPE_BROWSER: property = "browserIcon"; break;
		case OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT: property = "audioProcessOutputIcon"; break;
		default: break;
		}
	}
	return main ? main->property(property).value<QIcon>() : QIcon();
}

bool can_open_properties(obs_source_t *source)
{
	return !obs_source_is_scene(source) && !obs_source_is_group(source) && obs_source_configurable(source);
}

void activate_scene(obs_source_t *source)
{
	if (!source || !obs_source_is_scene(source) || obs_source_is_group(source))
		return;
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(source);
	else
		obs_frontend_set_current_scene(source);
}

QToolButton *tool_button(QWidget *parent, const char *name, const char *label, const char *theme, const QIcon &icon)
{
	auto *button = new QToolButton(parent);
	button->setObjectName(name);
	button->setAutoRaise(true);
	button->setText(text(label));
	button->setToolTip(button->text());
	button->setAccessibleName(button->text());
	button->setIcon(icon);
	button->setProperty("class", theme);
	return button;
}

namespace {
struct ParentItem {
	Source root, owner;
	int64_t id;
	QString label() const { return root.uuid == owner.uuid ? root.name : root.name + " / " + owner.name; }
};

struct ParentSearch {
	QString target;
	Source root;
	QList<ParentItem> items;
	void visit(obs_scene_t *scene)
	{
		if (!scene)
			return;
		obs_scene_enum_items(scene, [](obs_scene_t *owner, obs_sceneitem_t *item, void *data) {
			auto &search = *static_cast<ParentSearch *>(data);
			auto *child = obs_sceneitem_get_source(item);
			if (QString::fromUtf8(obs_source_get_uuid(child)) == search.target)
				search.items.append({search.root, describe(obs_scene_get_source(owner)), obs_sceneitem_get_id(item)});
			if (obs_sceneitem_is_group(item))
				search.visit(obs_sceneitem_group_get_scene(item));
			return true;
		}, this);
	}
};

OBSSceneItem get_item(const ParentItem &parent, const Source &source)
{
	auto owner = resolve(parent.owner);
	auto *scene = owner ? obs_group_or_scene_from_source(owner) : nullptr;
	auto *item = scene ? obs_scene_find_sceneitem_by_id(scene, parent.id) : nullptr;
	if (!item || source.uuid != QString::fromUtf8(obs_source_get_uuid(obs_sceneitem_get_source(item))))
		return {};
	return item;
}

QMenu *popup(QWidget *parent, const char *name)
{
	auto *menu = new QMenu(parent);
	menu->setObjectName(name);
	QObject::connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
	return menu;
}

const char *clone_type()
{
	for (const char *wanted : {"stagekit_port_source-clone", "source-clone"}) {
		const char *id, *unversioned;
		for (size_t i = 0; obs_enum_input_types2(i, &id, &unversioned); ++i)
			if (qstrcmp(unversioned, wanted) == 0)
				return obs_get_latest_input_type_id(wanted);
	}
	return nullptr;
}

bool add_to_scene(QWidget *parent, const Source &entry, bool clone)
{
	OBSSourceAutoRelease current = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
										 : obs_frontend_get_current_scene();
	auto source = resolve(entry);
	auto *scene = obs_scene_from_source(current);
	if (!scene || !source || source.Get() == current.Get())
		return false;
	if (!clone)
		return obs_scene_add(scene, source) != nullptr;
	const char *id = clone_type();
	if (!id)
		return false;
	bool ok = false;
	auto name = QInputDialog::getText(parent, text("Quick.AddClone"), text("Quick.CloneName"),
					  QLineEdit::Normal, entry.name + " CLONE", &ok).trimmed();
	if (!ok || name.isEmpty() || obs_source_removed(source) || obs_source_removed(current))
		return false;
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "clone", obs_source_get_name(source));
	OBSSourceAutoRelease copy = obs_source_create(id, name.toUtf8().constData(), settings, nullptr);
	return copy && obs_scene_add(scene, copy);
}
}

void open_parents(QWidget *parent, const QPoint &position, const Source &source)
{
	ParentSearch search{source.uuid, {}, {}};
	obs_enum_scenes([](void *data, obs_source_t *scene) {
		auto &parents = *static_cast<ParentSearch *>(data);
		if (!obs_obj_is_private(scene) && !obs_source_removed(scene)) {
			parents.root = describe(scene);
			parents.visit(obs_scene_from_source(scene));
		}
		return true;
	}, &search);
	std::stable_sort(search.items.begin(), search.items.end(), [](const ParentItem &a, const ParentItem &b) {
		return QString::localeAwareCompare(a.label(), b.label()) < 0;
	});
	auto *menu = popup(parent, "nativeQuickParentsMenu");
	menu->addSection(text("Quick.ParentScenes"));
	if (search.items.isEmpty())
		menu->addAction(text("Quick.NoParents"))->setEnabled(false);
	if (search.items.size() > 1) {
		auto *action = new QWidgetAction(menu);
		auto *filter = new QLineEdit;
		filter->setPlaceholderText(text("Quick.Search"));
		action->setDefaultWidget(filter);
		menu->addAction(action);
		QObject::connect(filter, &QLineEdit::textChanged, menu, [menu](const QString &term) {
			for (auto *row : menu->actions())
				if (row->data().isValid())
					row->setVisible(row->data().toString().contains(term, Qt::CaseInsensitive));
		});
	}
	for (const auto &parentItem : search.items) {
		auto *action = new QWidgetAction(menu);
		auto *row = new QWidget;
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(4, 0, 4, 0);
		auto *name = new QPushButton(parentItem.label(), row);
		name->setFlat(true);
		layout->addWidget(name, 1);
		QObject::connect(name, &QPushButton::clicked, menu, [menu, parentItem] {
			activate_scene(resolve(parentItem.root));
			menu->close();
		});
		auto item = get_item(parentItem, source);
		auto *visible = new QCheckBox(row);
		visible->setObjectName("nativeQuickVisibility");
		visible->setAccessibleName(text("Quick.Visibility"));
		visible->setToolTip(visible->accessibleName());
		visible->setProperty("visibilityCheckBox", true);
		visible->setProperty("class", "indicator-visibility");
		visible->setChecked(item && obs_sceneitem_visible(item));
		layout->addWidget(visible);
		QObject::connect(visible, &QCheckBox::clicked, menu, [parentItem, source](bool value) {
			auto target = get_item(parentItem, source);
			if (target)
				obs_sceneitem_set_visible(target, value);
		});
		auto *transform = tool_button(row, "nativeQuickTransform", "Quick.Transform", "icon-transform",
					      row->style()->standardIcon(QStyle::SP_TitleBarMaxButton));
		layout->addWidget(transform);
		QObject::connect(transform, &QToolButton::clicked, menu, [menu, parentItem, source] {
			auto target = get_item(parentItem, source);
			menu->close();
			if (target)
				obs_frontend_open_sceneitem_edit_transform(target);
		});
		action->setData(parentItem.label());
		action->setDefaultWidget(row);
		menu->addAction(action);
	}
	menu->popup(position);
}

void open_source_menu(QWidget *parent, const QPoint &position, const Source &entry)
{
	auto source = resolve(entry);
	if (!source)
		return;
	auto *menu = popup(parent, "nativeQuickSourceMenu");
	auto action = [&](const char *label, auto fn) {
		menu->addAction(text(label), parent, [entry, fn] {
			auto target = resolve(entry);
			if (target)
				fn(target.Get());
		});
	};
	auto add = [parent, entry](bool clone) {
		QPointer<QWidget> window = parent->window();
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (add_to_scene(main, entry, clone) && window && window->windowType() == Qt::Popup)
			window->close();
	};
	menu->addAction(text("Quick.AddCurrent"), parent, [add] { add(false); });
	if (clone_type())
		menu->addAction(text("Quick.AddClone"), parent, [add] { add(true); });
	menu->addSeparator();
	if (can_open_properties(source))
		action("Native.Properties", obs_frontend_open_source_properties);
	action("Native.Filters", obs_frontend_open_source_filters);
	action("Native.Rename", [](obs_source_t *target) {
		auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
		bool ok = false;
		const QString name = QInputDialog::getText(main, text("Native.Rename"), text("Quick.NewName"),
							QLineEdit::Normal, obs_source_get_name(target), &ok).trimmed();
		if (ok && !name.isEmpty() && !obs_source_removed(target))
			obs_source_set_name(target, name.toUtf8().constData());
	});
	if (obs_source_get_output_flags(source) & OBS_SOURCE_INTERACTION)
		action("Quick.Interact", obs_frontend_open_source_interaction);
	const auto id = QByteArray(obs_source_get_unversioned_id(source));
	if (id == "browser_source")
		action("Quick.Refresh", [](obs_source_t *target) {
			auto *properties = obs_source_properties(target);
			if (auto *refresh = obs_properties_get(properties, "refreshnocache"))
				obs_property_button_clicked(refresh, target);
			obs_properties_destroy(properties);
		});
	if (id == "dshow_input") {
		OBSDataAutoRelease settings = obs_source_get_settings(source);
		action(obs_data_get_bool(settings, "active") ? "Quick.Deactivate" : "Quick.Activate", [](obs_source_t *target) {
			OBSDataAutoRelease live = obs_source_get_settings(target);
			calldata_t data{};
			calldata_set_bool(&data, "active", !obs_data_get_bool(live, "active"));
			proc_handler_call(obs_source_get_proc_handler(target), "activate", &data);
			calldata_free(&data);
		});
	}
	menu->addSeparator();
	auto project = [entry](int monitor) {
		auto target = resolve(entry);
		if (target)
			obs_frontend_open_projector(obs_source_is_scene(target) ? "Scene" : "Source", monitor, "",
						    obs_source_get_name(target));
	};
	menu->addAction(text("Quick.WindowProjector"), parent, [project] { project(-1); });
	auto *screens = menu->addMenu(text("Quick.FullProjector"));
	int index = 0;
	for (auto *screen : QApplication::screens()) {
		screens->addAction(QString("%1. %2").arg(index + 1).arg(screen->name()), parent, [project, index] { project(index); });
		++index;
	}
	menu->popup(position);
}
} // namespace stagekit::native::quick
