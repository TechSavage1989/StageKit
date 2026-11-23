/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "views-dock.hpp"
#include "scene-controller.hpp"
#include "control-catalog.hpp"
#include "../common/item-transition.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <obs.hpp>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QStringList>
#include <QShowEvent>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

namespace stagekit {
namespace {

constexpr char kDockId[] = "stagekit-views-dock";

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString fresh_id()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject object(obs_data_t *data)
{
	const char *json = data ? obs_data_get_json(data) : nullptr;
	return json ? QJsonDocument::fromJson(QByteArray(json)).object() : QJsonObject{};
}

QJsonObject snapshot(const QString &id, QString *error = nullptr)
{
	OBSDataAutoRelease data = obs_data_create();
	std::string why;
	if (!scene_controller_get(id.toUtf8().constData(), data, why)) {
		if (error)
			*error = QString::fromStdString(why);
		return {};
	}
	return object(data);
}

QJsonObject find_id(const QJsonArray &array, const QString &id)
{
	for (const QJsonValue value : array)
		if (value.toObject()["id"].toString() == id)
			return value.toObject();
	return {};
}

QStringList values(const QJsonValue &value)
{
	QStringList result;
	for (const QJsonValue entry : value.toArray()) {
		const QString name = entry.isObject() ? entry.toObject()["value"].toString() : entry.toString();
		if (!name.isEmpty() && !result.contains(name))
			result.append(name);
	}
	return result;
}

QJsonArray value_list(const QStringList &names)
{
	QJsonArray result;
	for (const auto &name : names)
		result.append(QJsonObject{{"value", name}});
	return result;
}

struct Scene {
	QString uuid, name;
};

QList<Scene> scenes()
{
	QList<Scene> result;
	obs_frontend_source_list list{};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; ++i) {
		auto *source = list.sources.array[i];
		if (!obs_source_removed(source))
			result.append({QString::fromUtf8(obs_source_get_uuid(source)),
				       QString::fromUtf8(obs_source_get_name(source))});
	}
	obs_frontend_source_list_free(&list);
	return result;
}

bool collect_source_names(obs_scene_t *, obs_sceneitem_t *item, void *data)
{
	auto &names = *static_cast<QStringList *>(data);
	auto *source = obs_sceneitem_get_source(item);
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	if (!name.isEmpty() && !names.contains(name))
		names.append(name);
	if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_sceneitem_group_get_scene(item), collect_source_names, data);
	return true;
}

QStringList source_names(const QString &scene_uuid, const QStringList &additional)
{
	QStringList result;
	auto visit = [&](obs_source_t *source) {
		if (auto *scene = source ? obs_scene_from_source(source) : nullptr)
			obs_scene_enum_items(scene, collect_source_names, &result);
	};
	OBSSourceAutoRelease owner = obs_get_source_by_uuid(scene_uuid.toUtf8().constData());
	visit(owner);
	for (const auto &name : additional) {
		OBSSourceAutoRelease other = obs_get_source_by_name(name.toUtf8().constData());
		visit(other);
	}
	result.sort(Qt::CaseInsensitive);
	return result;
}

QStringList all_source_names()
{
	QStringList names;
	const auto add = [](void *data, obs_source_t *source) {
		auto &out = *static_cast<QStringList *>(data);
		if (!obs_obj_is_private(source) && !obs_source_removed(source) &&
		    (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) {
			const QString name = QString::fromUtf8(obs_source_get_name(source));
			if (!out.contains(name))
				out.append(name);
		}
		return true;
	};
	obs_enum_sources(add, &names);
	obs_enum_scenes(add, &names);
	names.sort(Qt::CaseInsensitive);
	return names;
}

QStringList checked_names(QListWidget *list)
{
	QStringList result;
	for (int i = 0; i < list->count(); ++i)
		if (list->item(i)->checkState() == Qt::Checked)
			result.append(list->item(i)->data(Qt::UserRole).toString());
	return result;
}

void fill_checks(QListWidget *list, const QStringList &available, const QStringList &chosen)
{
	const QSignalBlocker block(list);
	list->clear();
	QStringList ordered = chosen;
	for (const auto &name : available)
		if (!ordered.contains(name))
			ordered.append(name);
	for (const auto &name : ordered) {
		const bool missing = !available.contains(name);
		auto *item = new QListWidgetItem(missing ? text("Views.MissingItem").arg(name) : name, list);
		item->setData(Qt::UserRole, name);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(chosen.contains(name) ? Qt::Checked : Qt::Unchecked);
		if (missing)
			item->setToolTip(text("Views.MissingItemTip"));
	}
}

QListWidget *checks(QWidget *parent, const char *name, const QStringList &available, const QStringList &chosen)
{
	auto *list = new QListWidget(parent);
	list->setObjectName(name);
	list->setMinimumHeight(110);
	fill_checks(list, available, chosen);
	return list;
}

QLabel *note(QWidget *parent, const QString &value, const char *name = nullptr)
{
	auto *label = new QLabel(value, parent);
	label->setTextFormat(Qt::PlainText);
	label->setWordWrap(true);
	if (name)
		label->setObjectName(name);
	return label;
}

QString kind_label(const QString &kind)
{
	if (kind == "one")
		return text("Views.KindOne");
	if (kind == "together")
		return text("Views.KindTogether");
	if (kind == "follow")
		return text("Views.KindFollow");
	return text("Views.KindModes");
}

QComboBox *transition_box(QWidget *parent, const char *name, const QString &current, bool hiding)
{
	auto *box = new QComboBox(parent);
	box->setObjectName(name);
	if (hiding)
		box->addItem(text("Views.SameAsShow"), "same");
	box->addItem(text("Views.SceneTransition"), kTransitionScene);
	box->addItem(text("Views.KeepTransition"), kTransitionKeep);
	box->addItem(text("Views.Cut"), kTransitionCut);
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_transition_types(i, &id); ++i) {
		if (!id || std::strcmp(id, "cut_transition") == 0 || std::strcmp(id, "obs_stinger_transition") == 0)
			continue;
		const char *name_text = obs_source_get_display_name(id);
		if (name_text && box->findData(QString::fromUtf8(id)) < 0)
			box->addItem(QString::fromUtf8(name_text), QString::fromUtf8(id));
	}
	if (box->findData(current) < 0)
		box->addItem(current.isEmpty() ? text("Views.SavedDefault") : current, current);
	box->setCurrentIndex(std::max(0, box->findData(current)));
	QObject::connect(box, &QComboBox::currentIndexChanged, box, [box] { box->setProperty("viewsEdited", true); });
	return box;
}

void edit_group(QWidget *parent, const QString &scene_uuid, QJsonObject group, std::function<void(QJsonObject)> accept)
{
	const bool creating = group.isEmpty();
	if (creating)
		group = {{"id", fresh_id()}, {"kind", "modes"}, {"enabled", true}, {"settings", QJsonObject{}}};
	const QJsonObject original = group["settings"].toObject();
	auto *dialog = new QDialog(parent);
	dialog->setObjectName("viewsGroupEditor");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(text(creating ? "Views.AddGroup" : "Views.EditGroup"));
	auto *layout = new QVBoxLayout(dialog);
	auto *form = new QFormLayout;
	layout->addLayout(form);
	auto *name = new QLineEdit(group["name"].toString(), dialog);
	name->setObjectName("viewsGroupName");
	form->addRow(text("Views.Name"), name);
	auto *kind = new QComboBox(dialog);
	kind->setObjectName("viewsGroupKind");
	for (const char *value : {"modes", "one", "together", "follow"})
		kind->addItem(kind_label(value), value);
	kind->setCurrentIndex(std::max(0, kind->findData(group["kind"].toString())));
	kind->setEnabled(creating);
	form->addRow(text("Views.GroupKind"), kind);
	auto *enabled = new QCheckBox(text("Views.GroupEnabled"), dialog);
	enabled->setObjectName("viewsGroupEnabled");
	enabled->setChecked(group["enabled"].toBool(true));
	form->addRow(enabled);
	const QJsonObject original_origin = group["origin"].toObject();
	QCheckBox *ownership = nullptr;
	if (!original_origin["filterUuid"].toString().isEmpty()) {
		ownership = new QCheckBox(text("Views.ImportActivate"), dialog);
		ownership->setObjectName("viewsImportOwnership");
		const bool active_import = original_origin["activate"].toBool();
		ownership->setChecked(active_import);
		ownership->setEnabled(!active_import);
		enabled->setEnabled(active_import);
		if (!active_import)
			enabled->setChecked(false);
		form->addRow(ownership);
		form->addRow(note(dialog, text("Views.ImportOwnershipHelp")));
		QObject::connect(ownership, &QCheckBox::toggled, dialog, [enabled](bool active) {
			enabled->setEnabled(active);
			enabled->setChecked(active);
		});
	}
	auto *tabs = new QTabWidget(dialog);
	tabs->setObjectName("viewsGroupTabs");
	layout->addWidget(tabs, 1);
	auto *rules = new QWidget(tabs);
	auto *rules_layout = new QVBoxLayout(rules);
	auto *allow_none = new QCheckBox(text("Views.AllowNone"), rules);
	allow_none->setObjectName("viewsAllowNone");
	allow_none->setChecked(original["allow_none"].toBool());
	rules_layout->addWidget(allow_none);
	auto *mode_help = note(rules, text("Views.ModeGroupHelp"));
	rules_layout->addWidget(mode_help);
	auto *members_label = note(rules, text("Views.Members"));
	rules_layout->addWidget(members_label);
	auto *members = checks(rules, "viewsGroupItems", source_names(scene_uuid, values(group["scope"])),
			       values(original["items"]));
	members->setDragDropMode(QAbstractItemView::InternalMove);
	rules_layout->addWidget(members, 1);
	auto *leaders_label = note(rules, text("Views.Leaders"));
	rules_layout->addWidget(leaders_label);
	auto *leaders = checks(rules, "viewsGroupLeads", source_names(scene_uuid, values(group["scope"])),
			       values(original["leads"]));
	rules_layout->addWidget(leaders, 1);
	auto *lead_rule = new QComboBox(rules);
	lead_rule->setObjectName("viewsLeadRule");
	lead_rule->addItem(text("Views.AnyLead"), 0);
	lead_rule->addItem(text("Views.AllLeads"), 1);
	lead_rule->setCurrentIndex(original["lead_rule"].toInt() == 1 ? 1 : 0);
	rules_layout->addWidget(lead_rule);
	auto *lock = new QCheckBox(text("Views.LockHidden"), rules);
	lock->setObjectName("viewsLockHidden");
	lock->setChecked(original["lock_hidden"].toBool());
	rules_layout->addWidget(lock);
	tabs->addTab(rules, text("Views.Members"));
	auto *scope_page = new QWidget(tabs);
	auto *scope_layout = new QVBoxLayout(scope_page);
	scope_layout->addWidget(note(scope_page, text("Views.ScopeHelp")));
	QStringList other_scenes;
	for (const auto &scene : scenes())
		if (scene.uuid != scene_uuid)
			other_scenes.append(scene.name);
	auto *scope = checks(scope_page, "viewsGroupScope", other_scenes, values(group["scope"]));
	scope_layout->addWidget(scope, 1);
	auto *elsewhere_label = note(scope_page, text("Views.Elsewhere"));
	scope_layout->addWidget(elsewhere_label);
	auto *elsewhere = checks(scope_page, "viewsGroupElsewhere", all_source_names(), values(original["elsewhere"]));
	scope_layout->addWidget(elsewhere, 1);
	tabs->addTab(scope_page, text("Views.OtherScenes"));
	auto *transition_page = new QWidget(tabs);
	auto *transition_form = new QFormLayout(transition_page);
	auto *show =
		transition_box(transition_page, "viewsShowTransition", original["transition"].toString("scene"), false);
	auto *hide = transition_box(transition_page, "viewsHideTransition",
				    original["hide_transition"].toString("same"), true);
	auto milliseconds = [&](const char *object_name, const char *key) {
		auto *spin = new QDoubleSpinBox(transition_page);
		spin->setObjectName(object_name);
		const double saved = original[key].toDouble(300);
		spin->setDecimals(0);
		spin->setRange(std::min(0.0, saved), std::max(10000.0, saved));
		spin->setSuffix(text("Views.Milliseconds"));
		spin->setValue(saved);
		QObject::connect(spin, &QDoubleSpinBox::valueChanged, dialog,
				 [spin] { spin->setProperty("viewsEdited", true); });
		return spin;
	};
	auto *show_ms = milliseconds("viewsShowDuration", "transition_ms");
	auto *hide_ms = milliseconds("viewsHideDuration", "hide_ms");
	transition_form->addRow(text("Views.ShowTransition"), show);
	transition_form->addRow(text("Views.Duration"), show_ms);
	transition_form->addRow(text("Views.HideTransition"), hide);
	transition_form->addRow(text("Views.Duration"), hide_ms);
	transition_form->addRow(note(transition_page, text("Views.TransitionHelp")));
	tabs->addTab(transition_page, text("Views.Transitions"));
	layout->addWidget(note(dialog, text("Views.DraftHelp"), "viewsNestedDraftHelp"));
	auto *error = note(dialog, {}, "viewsGroupError");
	layout->addWidget(error);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
	buttons->button(QDialogButtonBox::Save)->setObjectName("viewsGroupSave");
	buttons->button(QDialogButtonBox::Save)->setText(text("Views.Done"));
	layout->addWidget(buttons);
	auto update_kind = [=] {
		const QString selected = kind->currentData().toString();
		const bool modes = selected == "modes", follow = selected == "follow";
		allow_none->setVisible(modes);
		mode_help->setVisible(modes);
		members_label->setVisible(!modes);
		members->setVisible(!modes);
		leaders_label->setVisible(follow);
		leaders->setVisible(follow);
		lead_rule->setVisible(follow);
		elsewhere_label->setVisible(!modes);
		elsewhere->setVisible(!modes);
	};
	QObject::connect(kind, &QComboBox::currentIndexChanged, dialog, update_kind);
	QObject::connect(scope, &QListWidget::itemChanged, dialog, [=] {
		const auto available = source_names(scene_uuid, checked_names(scope));
		fill_checks(members, available, checked_names(members));
		fill_checks(leaders, available, checked_names(leaders));
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=]() mutable {
		const QString selected = kind->currentData().toString();
		if (name->text().trimmed().isEmpty()) {
			error->setText(text("Views.NameRequired"));
			return;
		}
		if (selected != "modes" && checked_names(members).isEmpty()) {
			error->setText(text("Views.MembersRequired"));
			return;
		}
		if (selected == "follow" && checked_names(leaders).isEmpty()) {
			error->setText(text("Views.LeadersRequired"));
			return;
		}
		QJsonObject settings = original;
		settings["lock_hidden"] = lock->isChecked();
		if (!original.contains("transition") || show->property("viewsEdited").toBool())
			settings["transition"] = show->currentData().toString();
		if (!original.contains("transition_ms") || show_ms->property("viewsEdited").toBool())
			settings["transition_ms"] = show_ms->value();
		if (!original.contains("hide_transition") || hide->property("viewsEdited").toBool())
			settings["hide_transition"] = hide->currentData().toString();
		if (!original.contains("hide_ms") || hide_ms->property("viewsEdited").toBool())
			settings["hide_ms"] = hide_ms->value();
		if (selected == "modes") {
			settings["allow_none"] = allow_none->isChecked();
		} else {
			settings["items"] = value_list(checked_names(members));
			settings["leads"] = value_list(checked_names(leaders));
			settings["elsewhere"] = value_list(checked_names(elsewhere));
			settings["lead_rule"] = lead_rule->currentData().toInt();
			settings["mode"] = selected == "one" ? 0 : selected == "together" ? 1 : 2;
		}
		if (selected == "one") {
			QJsonObject ids = group["choiceIds"].toObject();
			for (const auto &source : checked_names(members))
				if (ids[source].toString().isEmpty())
					ids[source] = fresh_id();
			group["choiceIds"] = ids;
		}
		group["name"] = name->text().trimmed();
		group["kind"] = selected;
		group["enabled"] = enabled->isChecked();
		if (ownership) {
			QJsonObject origin = original_origin;
			origin["activate"] = ownership->isChecked();
			group["origin"] = origin;
		}
		group["scope"] = value_list(checked_names(scope));
		group["settings"] = settings;
		accept(group);
		dialog->accept();
	});
	update_kind();
	dialog->resize(530, 610);
	dialog->open();
	name->setFocus();
}

void edit_mode(QWidget *parent, const QString &scene_uuid, const QJsonObject &group, QString mode_id,
	       std::function<void(QString, QJsonObject)> accept)
{
	const bool creating = mode_id.isEmpty();
	if (creating)
		mode_id = fresh_id();
	const QString prefix = "mode|" + mode_id + "|";
	const QJsonObject original = group["settings"].toObject();
	auto *dialog = new QDialog(parent);
	dialog->setObjectName("viewsModeEditor");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(text(creating ? "Views.AddMode" : "Views.EditMode"));
	auto *layout = new QVBoxLayout(dialog);
	auto *form = new QFormLayout;
	auto *name = new QLineEdit(original[prefix + "name"].toString(), dialog);
	name->setObjectName("viewsModeName");
	form->addRow(text("Views.Name"), name);
	layout->addLayout(form);
	layout->addWidget(note(dialog, text("Views.ModeHelp")));
	auto *members = checks(dialog, "viewsModeItems", source_names(scene_uuid, values(group["scope"])),
			       values(original[prefix + "sources"]));
	layout->addWidget(members, 1);
	auto *hide_label = note(dialog, text("Views.ModeHide"));
	layout->addWidget(hide_label);
	auto *hide = checks(dialog, "viewsModeHide", source_names(scene_uuid, values(group["scope"])),
			    values(original[prefix + "hide"]));
	layout->addWidget(hide, 1);
	auto *change_scene = new QCheckBox(text("Views.ChangeScene"), dialog);
	change_scene->setObjectName("viewsModeChangeScene");
	change_scene->setChecked(!original[prefix + "go_to"].toString().isEmpty());
	layout->addWidget(change_scene);
	auto *destination = new QComboBox(dialog);
	destination->setObjectName("viewsModeDestination");
	for (const auto &scene : scenes())
		destination->addItem(scene.name, scene.name);
	const QString saved_scene = original[prefix + "go_to"].toString();
	if (!saved_scene.isEmpty() && destination->findData(saved_scene) < 0)
		destination->addItem(text("Views.MissingItem").arg(saved_scene), saved_scene);
	destination->setCurrentIndex(std::max(0, destination->findData(saved_scene)));
	destination->setEnabled(change_scene->isChecked());
	layout->addWidget(destination);
	QObject::connect(change_scene, &QCheckBox::toggled, destination, &QWidget::setEnabled);
	layout->addWidget(note(dialog, text("Views.DraftHelp"), "viewsNestedDraftHelp"));
	auto *error = note(dialog, {}, "viewsModeError");
	layout->addWidget(error);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
	buttons->button(QDialogButtonBox::Save)->setObjectName("viewsModeSave");
	buttons->button(QDialogButtonBox::Save)->setText(text("Views.Done"));
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
		if (name->text().trimmed().isEmpty()) {
			error->setText(text("Views.NameRequired"));
			return;
		}
		if (change_scene->isChecked() && destination->currentData().toString().isEmpty()) {
			error->setText(text("Views.DestinationRequired"));
			return;
		}
		QJsonObject settings = original;
		settings[prefix + "name"] = name->text().trimmed();
		settings[prefix + "sources"] = value_list(checked_names(members));
		settings[prefix + "hide"] = value_list(checked_names(hide));
		settings[prefix + "go_to"] = change_scene->isChecked() ? destination->currentData().toString() : "";
		QStringList ids = values(settings["modes"]);
		if (!ids.contains(mode_id))
			ids.append(mode_id);
		settings["modes"] = value_list(ids);
		accept(mode_id, settings);
		dialog->accept();
	});
	dialog->resize(510, 570);
	dialog->open();
	name->setFocus();
}

QList<QPair<QString, QString>> group_choices(const QJsonObject &group)
{
	QList<QPair<QString, QString>> result;
	const QString kind = group["kind"].toString();
	const auto settings = group["settings"].toObject();
	if (kind == "modes") {
		for (const auto &id : values(settings["modes"]))
			result.append({id, settings["mode|" + id + "|name"].toString(id)});
		if (settings["allow_none"].toBool())
			result.append({QString(), text("Views.None")});
	} else if (kind == "one") {
		const auto ids = group["choiceIds"].toObject();
		for (const auto &source : values(settings["items"]))
			result.append({ids[source].toString(), source});
	} else if (kind == "together") {
		result.append({"on", text("Views.On")});
		result.append({"off", text("Views.Off")});
	}
	return result;
}

void edit_view(QWidget *parent, const QJsonArray &groups, QJsonObject view, std::function<void(QJsonObject)> accept)
{
	const bool creating = view.isEmpty();
	if (creating)
		view["id"] = fresh_id();
	auto *dialog = new QDialog(parent);
	dialog->setObjectName("viewsViewEditor");
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(text(creating ? "Views.AddView" : "Views.EditView"));
	auto *layout = new QVBoxLayout(dialog);
	auto *form = new QFormLayout;
	auto *name = new QLineEdit(view["name"].toString(), dialog);
	name->setObjectName("viewsViewName");
	form->addRow(text("Views.Name"), name);
	layout->addLayout(form);
	layout->addWidget(note(dialog, text("Views.ViewHelp")));
	auto *table = new QTableWidget(dialog);
	table->setObjectName("viewsViewGroups");
	table->setColumnCount(2);
	table->setHorizontalHeaderLabels({text("Views.ViewControl"), text("Views.ViewSetTo")});
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	table->verticalHeader()->hide();
	layout->addWidget(table, 1);
	for (const QJsonValue entry : groups) {
		const auto group = entry.toObject();
		if (group["kind"].toString() == "follow")
			continue;
		const int row = table->rowCount();
		table->insertRow(row);
		auto *label = new QTableWidgetItem(group["name"].toString());
		label->setFlags(Qt::ItemIsEnabled);
		label->setData(Qt::UserRole, group["id"].toString());
		table->setItem(row, 0, label);
		auto *choice = new QComboBox(table);
		choice->setObjectName("viewsViewChoice-" + group["id"].toString());
		choice->addItem(text("Views.KeepCurrent"), QVariant());
		for (const auto &[id, title] : group_choices(group))
			choice->addItem(title, id);
		for (const QJsonValue saved : view["selections"].toArray()) {
			const auto selection = saved.toObject();
			if (selection["groupId"].toString() != group["id"].toString())
				continue;
			const QString id = selection["choiceId"].toString();
			int index = -1;
			for (int i = 1; i < choice->count(); ++i)
				if (choice->itemData(i).toString() == id) {
					index = i;
					break;
				}
			if (index < 0) {
				choice->addItem(text("Views.MissingItem").arg(id), id);
				index = choice->count() - 1;
			}
			choice->setCurrentIndex(index);
		}
		table->setCellWidget(row, 1, choice);
	}
	auto *change_scene = new QCheckBox(text("Views.ChangeScene"), dialog);
	change_scene->setObjectName("viewsChangeScene");
	change_scene->setChecked(!view["scene"].toString().isEmpty());
	layout->addWidget(change_scene);
	auto *destination = new QComboBox(dialog);
	destination->setObjectName("viewsDestinationScene");
	for (const auto &scene : scenes())
		destination->addItem(scene.name, scene.name);
	const QString saved_scene = view["scene"].toString();
	if (!saved_scene.isEmpty() && destination->findData(saved_scene) < 0)
		destination->addItem(text("Views.MissingItem").arg(saved_scene), saved_scene);
	destination->setCurrentIndex(std::max(0, destination->findData(saved_scene)));
	destination->setEnabled(change_scene->isChecked());
	layout->addWidget(destination);
	QObject::connect(change_scene, &QCheckBox::toggled, destination, &QWidget::setEnabled);
	layout->addWidget(note(dialog, text("Views.DraftHelp"), "viewsNestedDraftHelp"));
	auto *error = note(dialog, {}, "viewsViewError");
	layout->addWidget(error);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
	buttons->button(QDialogButtonBox::Save)->setObjectName("viewsViewSave");
	buttons->button(QDialogButtonBox::Save)->setText(text("Views.Done"));
	layout->addWidget(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=]() mutable {
		if (name->text().trimmed().isEmpty()) {
			error->setText(text("Views.NameRequired"));
			return;
		}
		QJsonArray selections;
		for (int row = 0; row < table->rowCount(); ++row) {
			auto *choice = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
			if (choice && choice->currentData().isValid())
				selections.append(
					QJsonObject{{"groupId", table->item(row, 0)->data(Qt::UserRole).toString()},
						    {"choiceId", choice->currentData().toString()}});
		}
		if (selections.isEmpty() && !change_scene->isChecked()) {
			error->setText(text("Views.SelectionRequired"));
			return;
		}
		if (change_scene->isChecked() && destination->currentData().toString().isEmpty()) {
			error->setText(text("Views.DestinationRequired"));
			return;
		}
		view["name"] = name->text().trimmed();
		view["selections"] = selections;
		view["scene"] = change_scene->isChecked() ? destination->currentData().toString() : "";
		accept(view);
		dialog->accept();
	});
	dialog->resize(540, 420);
	dialog->open();
	name->setFocus();
}

QJsonObject editable_definition(QJsonObject config)
{
	QJsonArray groups = config["groups"].toArray();
	for (int i = 0; i < groups.size(); ++i) {
		auto group = groups[i].toObject();
		if (group["kind"].toString() == "modes") {
			auto settings = group["settings"].toObject();
			settings.remove("mode_now");
			settings.remove("mode_before");
			group["settings"] = settings;
			groups[i] = group;
		}
	}
	config["groups"] = groups;
	return config;
}

class ViewsEditor final : public QDialog {
public:
	QString controller_id() const { return id_; }

	ViewsEditor(QWidget *parent, const QJsonObject &state)
		: QDialog(parent),
		  id_(state["controllerId"].toString()),
		  scene_uuid_(state["sceneUuid"].toString()),
		  config_(state["config"].toObject()),
		  original_(editable_definition(config_))
	{
		setObjectName("viewsEditor");
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(text("Views.ConfigureTitle").arg(state["name"].toString()));
		config_["schema"] = 1;
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(note(this, text("Views.EditorHelp")));
		auto *tabs = new QTabWidget(this);
		tabs->setObjectName("viewsEditorTabs");
		layout->addWidget(tabs, 1);
		auto *group_page = new QWidget(tabs);
		auto *group_layout = new QHBoxLayout(group_page);
		auto *left = new QVBoxLayout;
		group_layout->addLayout(left, 1);
		left->addWidget(note(group_page, text("Views.Groups")));
		groups_ = new QListWidget(group_page);
		groups_->setObjectName("viewsGroups");
		left->addWidget(groups_, 1);
		tools(
			left, "Group", [this] { group_edit(true); }, [this] { group_edit(false); },
			[this] { group_remove(); }, [this](int direction) { move("groups", groups_, direction); });
		auto *right = new QVBoxLayout;
		group_layout->addLayout(right, 1);
		right->addWidget(note(group_page, text("Views.Modes")));
		modes_ = new QListWidget(group_page);
		modes_->setObjectName("viewsModes");
		right->addWidget(modes_, 1);
		tools(
			right, "Mode", [this] { mode_edit(true); }, [this] { mode_edit(false); },
			[this] { mode_remove(); }, [this](int direction) { mode_move(direction); });
		mode_help_ = note(group_page, text("Views.SelectModeGroup"));
		right->addWidget(mode_help_);
		tabs->addTab(group_page, text("Views.GroupsAndModes"));
		auto *view_page = new QWidget(tabs);
		auto *view_layout = new QVBoxLayout(view_page);
		view_layout->addWidget(note(view_page, text("Views.ViewHelp")));
		views_ = new QListWidget(view_page);
		views_->setObjectName("viewsCombined");
		view_layout->addWidget(views_, 1);
		tools(
			view_layout, "View", [this] { view_edit(true); }, [this] { view_edit(false); },
			[this] { view_remove(); }, [this](int direction) { move("views", views_, direction); });
		tabs->addTab(view_page, text("Views.CombinedViews"));
		pending_ = note(this, text("Views.PendingChanges"), "viewsEditorPending");
		pending_->setVisible(false);
		layout->addWidget(pending_);
		error_ = note(this, {}, "viewsEditorError");
		layout->addWidget(error_);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
		buttons->button(QDialogButtonBox::Save)->setObjectName("viewsEditorSave");
		buttons->button(QDialogButtonBox::Save)->setText(text("Views.ApplyChanges"));
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] { save(); });
		connect(groups_, &QListWidget::currentRowChanged, this, [this] { fill_modes(); });
		connect(groups_, &QListWidget::itemDoubleClicked, this, [this] { group_edit(false); });
		connect(modes_, &QListWidget::itemDoubleClicked, this, [this] { mode_edit(false); });
		connect(views_, &QListWidget::itemDoubleClicked, this, [this] { view_edit(false); });
		redraw();
		resize(760, 530);
	}

private:
	void tools(QVBoxLayout *layout, const char *entity, std::function<void()> add, std::function<void()> edit,
		   std::function<void()> remove, std::function<void(int)> reorder)
	{
		auto *row = new QHBoxLayout;
		layout->addLayout(row);
		auto button = [&](const char *operation, const char *label, std::function<void()> action) {
			auto *button = new QPushButton(text(label), this);
			button->setObjectName(QStringLiteral("views") + operation + entity);
			button->setAutoDefault(false);
			connect(button, &QPushButton::clicked, this, std::move(action));
			row->addWidget(button);
		};
		button("Add", "Views.Add", std::move(add));
		button("Edit", "Views.Edit", std::move(edit));
		button("Remove", "Views.Remove", std::move(remove));
		for (const int direction : {-1, 1}) {
			auto *arrow = new QToolButton(this);
			arrow->setObjectName(QStringLiteral("views") + entity + (direction < 0 ? "Up" : "Down"));
			arrow->setArrowType(direction < 0 ? Qt::UpArrow : Qt::DownArrow);
			arrow->setAccessibleName(text(direction < 0 ? "Views.MoveUp" : "Views.MoveDown"));
			arrow->setToolTip(arrow->accessibleName());
			connect(arrow, &QToolButton::clicked, this, [reorder, direction] { reorder(direction); });
			row->addWidget(arrow);
		}
	}

	QString selected(QListWidget *list) const
	{
		return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
	}

	void fill(QListWidget *list, const QJsonArray &array, const QString &select = {})
	{
		const QString keep = select.isEmpty() ? selected(list) : select;
		const QSignalBlocker block(list);
		list->clear();
		int index = 0;
		for (const QJsonValue entry : array) {
			const auto item = entry.toObject();
			auto *row = new QListWidgetItem(item["name"].toString(), list);
			row->setData(Qt::UserRole, item["id"].toString());
			if (item["id"].toString() == keep)
				index = list->count() - 1;
		}
		list->setCurrentRow(list->count() ? index : -1);
	}

	void redraw(const QString &group = {}, const QString &view = {})
	{
		fill(groups_, config_["groups"].toArray(), group);
		fill(views_, config_["views"].toArray(), view);
		fill_modes();
	}

	void fill_modes(const QString &select = {})
	{
		const auto group = find_id(config_["groups"].toArray(), selected(groups_));
		const bool modes = group["kind"].toString() == "modes";
		QJsonArray entries;
		if (modes) {
			const auto settings = group["settings"].toObject();
			for (const auto &id : values(settings["modes"]))
				entries.append(QJsonObject{{"id", id},
							   {"name", settings["mode|" + id + "|name"].toString(id)}});
		}
		fill(modes_, entries, select);
		mode_help_->setVisible(!modes || entries.isEmpty());
		mode_help_->setText(text(modes ? "Views.EmptyModes" : "Views.SelectModeGroup"));
		for (const char *name :
		     {"viewsAddMode", "viewsEditMode", "viewsRemoveMode", "viewsModeUp", "viewsModeDown"})
			if (auto *button = findChild<QAbstractButton *>(name))
				button->setEnabled(modes);
	}

	void update_pending() { pending_->setVisible(editable_definition(config_) != original_); }

	void put(const char *field, const QJsonObject &entry)
	{
		QJsonArray array = config_[field].toArray();
		bool replaced = false;
		for (int i = 0; i < array.size(); ++i)
			if (array[i].toObject()["id"] == entry["id"]) {
				array[i] = entry;
				replaced = true;
				break;
			}
		if (!replaced)
			array.append(entry);
		config_[field] = array;
		update_pending();
	}

	bool referenced(const QString &group, const QString &choice, bool whole_group)
	{
		for (const QJsonValue entry : config_["views"].toArray()) {
			const auto view = entry.toObject();
			for (const QJsonValue value : view["selections"].toArray()) {
				const auto selection = value.toObject();
				if (selection["groupId"].toString() == group &&
				    (whole_group || selection["choiceId"].toString() == choice)) {
					error_->setText(text("Views.UsedByView").arg(view["name"].toString()));
					return true;
				}
			}
		}
		return false;
	}

	void erase(const char *field, const QString &id)
	{
		QJsonArray array = config_[field].toArray();
		for (int i = 0; i < array.size(); ++i)
			if (array[i].toObject()["id"].toString() == id) {
				array.removeAt(i);
				break;
			}
		config_[field] = array;
		update_pending();
		redraw();
	}

	void move(const char *field, QListWidget *list, int direction)
	{
		QJsonArray array = config_[field].toArray();
		const int from = list->currentRow(), to = from + direction;
		if (from < 0 || to < 0 || to >= array.size())
			return;
		const auto entry = array.takeAt(from);
		array.insert(to, entry);
		config_[field] = array;
		update_pending();
		redraw();
	}

	void group_edit(bool creating)
	{
		const auto group = creating ? QJsonObject{} : find_id(config_["groups"].toArray(), selected(groups_));
		if (!creating && group.isEmpty())
			return;
		edit_group(this, scene_uuid_, group, [this](QJsonObject value) {
			put("groups", value);
			redraw(value["id"].toString());
		});
	}

	void group_remove()
	{
		const QString id = selected(groups_);
		if (!id.isEmpty() && !referenced(id, {}, true))
			erase("groups", id);
	}

	void mode_edit(bool creating)
	{
		auto group = find_id(config_["groups"].toArray(), selected(groups_));
		if (group["kind"].toString() != "modes")
			return;
		const QString id = creating ? QString() : selected(modes_);
		if (!creating && id.isEmpty())
			return;
		edit_mode(this, scene_uuid_, group, id, [this, group](QString mode, QJsonObject settings) mutable {
			group["settings"] = settings;
			put("groups", group);
			fill_modes(mode);
		});
	}

	void mode_remove()
	{
		auto group = find_id(config_["groups"].toArray(), selected(groups_));
		const QString id = selected(modes_);
		if (id.isEmpty() || referenced(group["id"].toString(), id, false))
			return;
		auto settings = group["settings"].toObject();
		QStringList modes = values(settings["modes"]);
		modes.removeAll(id);
		settings["modes"] = value_list(modes);
		const QString prefix = "mode|" + id + "|";
		for (const auto &key : settings.keys())
			if (key.startsWith(prefix))
				settings.remove(key);
		for (const auto &other : modes)
			if (settings["mode|" + other + "|leave_to"].toString() == id)
				settings["mode|" + other + "|leave_to"] = "";
		for (const char *key : {"mode_now", "mode_before", "mode_for"})
			if (settings[key].toString() == id)
				settings[key] = "";
		group["settings"] = settings;
		put("groups", group);
		fill_modes();
	}

	void mode_move(int direction)
	{
		auto group = find_id(config_["groups"].toArray(), selected(groups_));
		auto settings = group["settings"].toObject();
		QStringList modes = values(settings["modes"]);
		const int from = modes_->currentRow(), to = from + direction;
		if (from < 0 || to < 0 || to >= modes.size())
			return;
		modes.move(from, to);
		settings["modes"] = value_list(modes);
		group["settings"] = settings;
		put("groups", group);
		fill_modes();
	}

	void view_edit(bool creating)
	{
		const auto view = creating ? QJsonObject{} : find_id(config_["views"].toArray(), selected(views_));
		if (!creating && view.isEmpty())
			return;
		edit_view(this, config_["groups"].toArray(), view, [this](QJsonObject value) {
			put("views", value);
			redraw({}, value["id"].toString());
		});
	}

	void view_remove()
	{
		if (!selected(views_).isEmpty())
			erase("views", selected(views_));
	}

	void save()
	{
		QString error;
		const auto current = snapshot(id_, &error);
		if (current.isEmpty()) {
			error_->setText(error);
			return;
		}
		// Key presses change the two runtime mode fields. Structural edits or
		// source renames must not be overwritten by an older open draft.
		if (editable_definition(current["config"].toObject()) != original_) {
			error_->setText(text("Views.DraftChanged"));
			return;
		}
		const QByteArray json = QJsonDocument(config_).toJson(QJsonDocument::Compact);
		OBSDataAutoRelease settings = obs_data_create_from_json(json.constData());
		std::string why;
		if (!scene_controller_configure(id_.toUtf8().constData(), settings, why)) {
			error_->setText(QString::fromStdString(why));
			return;
		}
		obs_frontend_save();
		accept();
	}

	QString id_, scene_uuid_;
	QJsonObject config_, original_;
	QListWidget *groups_ = nullptr, *modes_ = nullptr, *views_ = nullptr;
	QLabel *mode_help_ = nullptr, *error_ = nullptr, *pending_ = nullptr;
};

obs_source_t *controller_source(const QJsonObject &state)
{
	const QByteArray id = state["controllerId"].toString().toUtf8();
	OBSSourceAutoRelease scene = obs_get_source_by_uuid(state["sceneUuid"].toString().toUtf8().constData());
	struct Find {
		const char *id;
		obs_source_t *found = nullptr;
	} find{id.constData(), nullptr};
	if (scene)
		obs_source_enum_filters(
			scene,
			[](obs_source_t *, obs_source_t *filter, void *data) {
				auto &find = *static_cast<Find *>(data);
				if (!find.found && std::strcmp(obs_source_get_uuid(filter), find.id) == 0)
					find.found = obs_source_get_ref(filter);
			},
			&find);
	return find.found;
}

void name_dialog(QWidget *parent, const char *object_name, const QString &title, const QString &initial,
		 std::function<bool(const QString &, QString &)> apply)
{
	auto *dialog = new QDialog(parent);
	dialog->setObjectName(object_name);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(title);
	auto *layout = new QFormLayout(dialog);
	auto *name = new QLineEdit(initial, dialog);
	name->setObjectName("viewsControllerName");
	layout->addRow(text("Views.Name"), name);
	auto *error = note(dialog, {}, "viewsControllerNameError");
	layout->addRow(error);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, dialog);
	buttons->button(QDialogButtonBox::Save)->setObjectName("viewsControllerNameSave");
	layout->addRow(buttons);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, [=] {
		QString why;
		if (name->text().trimmed().isEmpty())
			why = text("Views.NameRequired");
		else if (apply(name->text().trimmed(), why)) {
			dialog->accept();
			return;
		}
		error->setText(why);
	});
	dialog->resize(380, dialog->sizeHint().height());
	dialog->open();
	name->selectAll();
	name->setFocus();
}

void combo_items(QComboBox *box, const QList<QPair<QString, QString>> &items, const QString &selected)
{
	bool same = box->count() == items.size();
	for (int i = 0; same && i < items.size(); ++i)
		same = box->itemText(i) == items[i].first && box->itemData(i).toString() == items[i].second;
	if (!same) {
		box->clear();
		for (const auto &[name, id] : items)
			box->addItem(name, id);
	}
	const int found = box->findData(selected);
	box->setCurrentIndex(found >= 0 ? found : box->count() ? 0 : -1);
}

// A native button keeps the OBS theme while wrapping long, user-provided names.
class ViewControlButton final : public QPushButton {
public:
	ViewControlButton(const QString &label, QWidget *parent) : QPushButton(label, parent)
	{
		// These wrapped size hints describe the full widget rectangle.
		setAttribute(Qt::WA_LayoutUsesWidgetRect);
	}
	void set_display_size(int value)
	{
		size_ = std::clamp(value, 40, 112);
		setMinimumHeight(size_);
		setIconSize(QSize(icon_side(), icon_side()));
		updateGeometry();
		update();
	}
	int preferred_width() const { return size_ * 3 + 6; }
	QSize sizeHint() const override { return {preferred_width(), heightForWidth(preferred_width())}; }
	QSize minimumSizeHint() const override { return {80, size_}; }
	int heightForWidth(int width) const override
	{
		const int label_height = fontMetrics()
						 .boundingRect(QRect(0, 0, std::max(40, width - padding() * 2), 1000),
							       Qt::TextWordWrap | Qt::AlignCenter, text())
						 .height();
		return std::max(size_, label_height + padding() * 2 + (icon().isNull() ? 0 : icon_side() + 4));
	}

protected:
	bool event(QEvent *event) override
	{
		const bool handled = QPushButton::event(event);
		if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) {
			// OBS themes can cap ordinary buttons below our wrapped minimum.
			// Clear that cap after polish so the grid can use heightForWidth()
			// without centering a smaller maximum rectangle inside the row.
			setMaximumHeight(QWIDGETSIZE_MAX);
		}
		return handled;
	}

	void paintEvent(QPaintEvent *) override
	{
		QStylePainter painter(this);
		QStyleOptionButton option;
		initStyleOption(&option);
		option.text.clear();
		option.icon = QIcon();
		painter.drawControl(QStyle::CE_PushButton, option);
		const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
		painter.setPen(palette().color(group, isChecked() && isEnabled() ? QPalette::HighlightedText
										 : QPalette::ButtonText));
		QRect label = rect().adjusted(padding(), padding(), -padding(), -padding());
		if (!icon().isNull()) {
			const QRect image((width() - icon_side()) / 2, padding(), icon_side(), icon_side());
			icon().paint(&painter, image, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled,
				     isChecked() ? QIcon::On : QIcon::Off);
			label.setTop(image.bottom() + 5);
		}
		painter.drawText(label, Qt::TextWordWrap | Qt::AlignCenter, text());
	}

private:
	int padding() const { return std::max(6, size_ / 8); }
	int icon_side() const { return std::clamp(size_ / 2, 20, 56); }
	int size_ = 48;
};

QJsonObject panel_pin(const QJsonObject &control)
{
	return {{"id", control["id"]},
		{"target", control["target"]},
		{"name", control["name"]},
		{"context", control["section"]}};
}

QString panel_title(const QJsonObject &pin, const QJsonObject &control)
{
	const QString custom = pin["label"].toString().trimmed();
	return !custom.isEmpty() ? custom : control["name"].toString(pin["name"].toString());
}

QString panel_section(const QJsonObject &pin, const QJsonObject &control)
{
	const QString custom = pin["section"].toString().trimmed();
	const QString section = !custom.isEmpty() ? custom : control["section"].toString(pin["context"].toString());
	return section.isEmpty() ? text("Controls.General") : section;
}

QIcon panel_icon(const QJsonObject &pin)
{
	const QString encoded = pin["iconPng"].toString();
	if (encoded.isEmpty() || encoded.size() > 128 * 1024)
		return {};
	QByteArray bytes = QByteArray::fromBase64(encoded.toLatin1());
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer, "PNG");
	const QSize size = reader.size();
	if (!size.isValid() || size.width() > 128 || size.height() > 128)
		return {};
	return QIcon(QPixmap::fromImage(reader.read()));
}

QString embedded_panel_icon(QImage image)
{
	if (image.isNull())
		return {};
	image = image.scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation)
			.convertToFormat(QImage::Format_ARGB32);
	QByteArray png;
	QBuffer buffer(&png);
	buffer.open(QIODevice::WriteOnly);
	if (!image.save(&buffer, "PNG"))
		return {};
	const auto encoded = QString::fromLatin1(png.toBase64());
	return encoded.size() <= 128 * 1024 ? encoded : QString();
}

class ViewIconLibrary final : public QDialog {
public:
	ViewIconLibrary(QWidget *parent, std::function<void(const QString &)> apply)
		: QDialog(parent),
		  apply_(std::move(apply))
	{
		setObjectName("viewsPanelIconLibraryDialog");
		setWindowTitle(text("Controls.IconLibraryTitle"));
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowModality(Qt::WindowModal);
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(note(this, text("Controls.IconLibraryHelp")));
		search_ = new QLineEdit(this);
		search_->setObjectName("viewsPanelIconLibrarySearch");
		search_->setPlaceholderText(text("Controls.IconSearch"));
		search_->setClearButtonEnabled(true);
		layout->addWidget(search_);
		auto *filters = new QHBoxLayout;
		category_ = new QComboBox(this);
		category_->setObjectName("viewsPanelIconLibraryCategory");
		category_->setAccessibleName(text("Controls.IconCategory"));
		category_->addItem(text("Controls.IconAllCategories"), QString());
		auto *category_label = new QLabel(text("Controls.IconCategory"), this);
		category_label->setBuddy(category_);
		filters->addWidget(category_label);
		filters->addWidget(category_, 1);
		palette_ = new QComboBox(this);
		palette_->setObjectName("viewsPanelIconLibraryPalette");
		palette_->setAccessibleName(text("Controls.IconColour"));
		auto *palette_label = new QLabel(text("Controls.IconColour"), this);
		palette_label->setBuddy(palette_);
		filters->addWidget(palette_label);
		filters->addWidget(palette_, 1);
		layout->addLayout(filters);
		icons_ = new QListWidget(this);
		icons_->setObjectName("viewsPanelIconLibraryItems");
		icons_->setViewMode(QListView::IconMode);
		icons_->setMovement(QListView::Static);
		icons_->setResizeMode(QListView::Adjust);
		icons_->setSelectionMode(QAbstractItemView::SingleSelection);
		icons_->setIconSize(QSize(64, 64));
		icons_->ensurePolished();
		icons_->setGridSize(QSize(144, 64 + icons_->fontMetrics().lineSpacing() * 3 + 12));
		icons_->setSpacing(4);
		icons_->setWordWrap(true);
		icons_->setTextElideMode(Qt::ElideRight);
		icons_->setUniformItemSizes(true);
		icons_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		layout->addWidget(icons_, 1);
		status_ = note(this, {}, "viewsPanelIconLibraryStatus");
		layout->addWidget(status_);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
		use_ = buttons->addButton(text("Controls.UseIcon"), QDialogButtonBox::AcceptRole);
		use_->setObjectName("viewsPanelIconLibraryUse");
		use_->setEnabled(false);
		buttons->button(QDialogButtonBox::Cancel)->setObjectName("viewsPanelIconLibraryCancel");
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] { use_selection(); });
		connect(icons_, &QListWidget::itemActivated, this, [this] { use_selection(); });
		connect(icons_, &QListWidget::itemSelectionChanged, this, [this] { show_selection(); });
		connect(search_, &QLineEdit::textChanged, this, [this] { filter_icons(); });
		connect(category_, &QComboBox::currentIndexChanged, this, [this] { filter_icons(); });
		connect(palette_, &QComboBox::currentIndexChanged, this, [this] { update_palette(); });
		load_icons();
		resize(680, 560);
	}

private:
	void load_icons()
	{
		QFile file(":/stagekit/view-icons/catalog.json");
		if (!file.open(QIODevice::ReadOnly)) {
			status_->setText(text("Controls.IconLibraryError"));
			return;
		}
		const auto catalog = QJsonDocument::fromJson(file.readAll()).object();
		const QSignalBlocker category_block(category_), palette_block(palette_);
		for (const QJsonValue value : catalog["groups"].toArray()) {
			const auto group = value.toObject();
			const auto label = text(group["labelKey"].toString().toUtf8().constData());
			groups_.insert(group["id"].toString(), label);
			category_->addItem(label, group["id"].toString());
		}
		for (const QJsonValue value : catalog["palettes"].toArray()) {
			const auto palette = value.toObject();
			palette_->addItem(text(palette["labelKey"].toString().toUtf8().constData()),
					  palette["id"].toString());
		}
		for (const QJsonValue value : catalog["icons"].toArray()) {
			const auto icon = value.toObject();
			const auto label = text(icon["labelKey"].toString().toUtf8().constData());
			auto *item = new QListWidgetItem(label, icons_);
			// IconMode's grid controls placement, not the item's painting rect.
			// Give the native delegate the full tile for wrapped labels.
			item->setSizeHint(icons_->gridSize());
			item->setData(Qt::UserRole, icon["id"].toString());
			item->setData(Qt::UserRole + 1, icon.toVariantMap());
			item->setToolTip(label + "\n" + groups_.value(icon["group"].toString()));
			item->setTextAlignment(Qt::AlignHCenter);
		}
		update_palette();
		filter_icons();
	}

	QString image_path(const QListWidgetItem *item) const
	{
		if (!item)
			return {};
		const auto icon = QJsonObject::fromVariantMap(item->data(Qt::UserRole + 1).toMap());
		const auto path = icon["paths"].toObject()[palette_->currentData().toString()].toString();
		return path.startsWith(":/stagekit/view-icons/") ? path : QString();
	}

	void update_palette()
	{
		for (int i = 0; i < icons_->count(); ++i) {
			auto *item = icons_->item(i);
			item->setIcon(QIcon(image_path(item)));
		}
		show_selection();
	}

	void filter_icons()
	{
		const auto query = search_->text().simplified();
		const auto words = query.split(' ', Qt::SkipEmptyParts);
		const auto group = category_->currentData().toString();
		int visible = 0;
		for (int i = 0; i < icons_->count(); ++i) {
			auto *item = icons_->item(i);
			const auto icon = QJsonObject::fromVariantMap(item->data(Qt::UserRole + 1).toMap());
			QString searchable = item->text() + " " + groups_.value(icon["group"].toString());
			for (const QJsonValue keyword : icon["keywords"].toArray())
				searchable += " " + keyword.toString();
			bool matches = group.isEmpty() || group == icon["group"].toString();
			for (const auto &word : words)
				matches = matches && searchable.contains(word, Qt::CaseInsensitive);
			item->setHidden(!matches);
			if (matches)
				++visible;
		}
		if (icons_->currentItem() && icons_->currentItem()->isHidden()) {
			icons_->clearSelection();
			icons_->setCurrentItem(nullptr);
		}
		visible_ = visible;
		show_selection();
	}

	void show_selection()
	{
		auto *item = icons_->currentItem();
		const bool selected = item && item->isSelected() && !item->isHidden() && !item->icon().isNull();
		use_->setEnabled(selected);
		status_->setText(selected   ? item->toolTip().replace('\n', " - ")
				 : visible_ ? text("Controls.IconCount").arg(visible_)
					    : text("Controls.IconNoMatches"));
	}

	void use_selection()
	{
		auto *item = icons_->currentItem();
		if (!item || !item->isSelected() || item->isHidden() || !use_->isEnabled())
			return;
		const auto encoded = embedded_panel_icon(QImage(image_path(item)));
		if (encoded.isEmpty()) {
			status_->setText(text("Controls.IconError"));
			return;
		}
		apply_(encoded);
		accept();
	}

	std::function<void(const QString &)> apply_;
	QMap<QString, QString> groups_;
	QLineEdit *search_ = nullptr;
	QComboBox *category_ = nullptr, *palette_ = nullptr;
	QListWidget *icons_ = nullptr;
	QLabel *status_ = nullptr;
	QPushButton *use_ = nullptr;
	int visible_ = 0;
};

// A click and a keyboard activation have the same meaning, including when the
// current row already represents an active native selection. A double-click is
// one command, not two visibility toggles.
class ViewControlList final : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;
	std::function<void(QTreeWidgetItem *)> activate;

protected:
	void keyPressEvent(QKeyEvent *event) override
	{
		if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) {
			if (!event->isAutoRepeat()) {
				auto *item = currentItem();
				if (item && item->childCount())
					item->setExpanded(!item->isExpanded());
				else if (item && activate && (item->flags() & Qt::ItemIsEnabled))
					activate(item);
			}
			event->accept();
			return;
		}
		QTreeWidget::keyPressEvent(event);
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		suppress_release_ = true;
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override
	{
		if (suppress_release_) {
			suppress_release_ = false;
			event->accept();
			return;
		}
		QTreeWidget::mouseReleaseEvent(event);
	}

private:
	bool suppress_release_ = false;
};

class ViewPanelEditor final : public QDialog {
public:
	ViewPanelEditor(QWidget *parent, const QJsonArray &inventory, const QJsonArray &pins,
			const QJsonArray &custom_sections, bool show_scene,
			std::function<void(QJsonArray, QJsonArray, bool, bool)> apply)
		: QDialog(parent),
		  inventory_(inventory),
		  apply_(std::move(apply))
	{
		setObjectName("viewsPanelEditor");
		setAttribute(Qt::WA_DeleteOnClose);
		setWindowTitle(text("Controls.EditPanel"));
		auto *layout = new QVBoxLayout(this);
		layout->addWidget(note(this, text("Controls.EditorHelp")));
		auto *columns = new QHBoxLayout;
		layout->addLayout(columns, 1);
		auto *available_column = new QVBoxLayout;
		columns->addLayout(available_column, 1);
		available_column->addWidget(note(this, text("Controls.Available")));
		search_ = new QLineEdit(this);
		search_->setObjectName("viewsPanelSearch");
		search_->setPlaceholderText(text("Controls.Search"));
		available_column->addWidget(search_);
		available_ = new QTreeWidget(this);
		available_->setObjectName("viewsPanelAvailable");
		available_->setHeaderHidden(true);
		available_->setUniformRowHeights(true);
		available_->setWordWrap(false);
		available_->setTextElideMode(Qt::ElideRight);
		available_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		available_->header()->setSectionResizeMode(QHeaderView::Stretch);
		available_->setIndentation(16);
		available_column->addWidget(available_, 1);
		auto *selected_column = new QVBoxLayout;
		columns->addLayout(selected_column, 1);
		selected_heading_ = note(this, {});
		selected_column->addWidget(selected_heading_);
		pins_ = new QListWidget(this);
		pins_->setObjectName("viewsPanelPins");
		pins_->setSelectionMode(QAbstractItemView::ExtendedSelection);
		pins_->setDragDropMode(QAbstractItemView::InternalMove);
		pins_->setDefaultDropAction(Qt::MoveAction);
		pins_->setWordWrap(false);
		pins_->setTextElideMode(Qt::ElideRight);
		pins_->setIconSize(QSize(20, 20));
		selected_column->addWidget(pins_, 1);
		auto *tools = new QHBoxLayout;
		selected_column->addLayout(tools);
		remove_ = new QPushButton(text("Controls.RemoveSelected"), this);
		remove_->setObjectName("viewsPanelRemove");
		tools->addWidget(remove_, 1);
		clear_ = new QPushButton(text("Controls.RemoveAll"), this);
		clear_->setObjectName("viewsPanelRemoveAll");
		tools->addWidget(clear_);
		for (const int direction : {-1, 1}) {
			auto *move = new QToolButton(this);
			move->setObjectName(direction < 0 ? "viewsPanelUp" : "viewsPanelDown");
			move->setArrowType(direction < 0 ? Qt::UpArrow : Qt::DownArrow);
			move->setAccessibleName(text(direction < 0 ? "Views.MoveUp" : "Views.MoveDown"));
			move->setToolTip(move->accessibleName());
			tools->addWidget(move);
			connect(move, &QToolButton::clicked, this, [this, direction] { move_pins(direction); });
		}
		auto *form = new QFormLayout;
		selected_column->addLayout(form);
		label_ = new QLineEdit(this);
		label_->setObjectName("viewsPanelLabel");
		label_->setPlaceholderText(text("Controls.UseName"));
		form->addRow(text("Controls.Label"), label_);
		section_choice_ = new QComboBox(this);
		section_choice_->setObjectName("viewsPanelSectionChoice");
		section_choice_->setEditable(true);
		section_choice_->setInsertPolicy(QComboBox::NoInsert);
		section_choice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		section_choice_->setMinimumContentsLength(12);
		section_choice_->setToolTip(text("Controls.SectionHelp"));
		section_ = section_choice_->lineEdit();
		section_->setObjectName("viewsPanelSection");
		for (const char *key :
		     {"Controls.Scenes", "Controls.SectionViews", "Controls.SectionCameras", "Controls.SectionCaptures",
		      "Controls.SectionBackgrounds", "Controls.SectionOverlays", "Controls.SectionAudio"})
			section_presets_.append(text(key));
		for (const QJsonValue value : custom_sections)
			remember_section(value.toString());
		for (const QJsonValue value : pins)
			remember_section(value.toObject()["section"].toString());
		fill_sections();
		auto *section_row = new QHBoxLayout;
		section_row->addWidget(section_choice_, 1);
		add_section_ = new QPushButton(text("Controls.AddSection"), this);
		add_section_->setObjectName("viewsPanelAddSection");
		section_row->addWidget(add_section_);
		reset_section_ = new QToolButton(this);
		reset_section_->setObjectName("viewsPanelResetSection");
		reset_section_->setText(text("Controls.ResetSection"));
		reset_section_->setToolTip(text("Controls.ResetSectionHelp"));
		section_row->addWidget(reset_section_);
		form->addRow(text("Controls.Section"), section_row);
		auto *icons = new QHBoxLayout;
		icon_preview_ = new QLabel(this);
		icon_preview_->setObjectName("viewsPanelIconPreview");
		icon_preview_->setFixedSize(32, 32);
		icon_preview_->setAlignment(Qt::AlignCenter);
		icons->addWidget(icon_preview_);
		icon_library_ = new QPushButton(text("Controls.IconLibrary"), this);
		icon_library_->setObjectName("viewsPanelIconLibrary");
		icons->addWidget(icon_library_, 1);
		choose_icon_ = new QPushButton(text("Controls.ChooseIcon"), this);
		choose_icon_->setObjectName("viewsPanelChooseIcon");
		icons->addWidget(choose_icon_, 1);
		remove_icon_ = new QPushButton(text("Controls.RemoveIcon"), this);
		remove_icon_->setObjectName("viewsPanelRemoveIcon");
		icons->addWidget(remove_icon_);
		form->addRow(text("Controls.Icon"), icons);
		context_ = note(this, {}, "viewsPanelContext");
		selected_column->addWidget(context_);
		switch_scene_ = new QCheckBox(text("Controls.SwitchScene"), this);
		switch_scene_->setObjectName("viewsPanelSwitchScene");
		switch_scene_->setToolTip(text("Controls.SwitchSceneHelp"));
		switch_scene_->setChecked(show_scene);
		layout->addWidget(switch_scene_);
		auto *automatic_button = new QPushButton(text("Controls.Automatic"), this);
		automatic_button->setObjectName("viewsPanelAutomatic");
		layout->addWidget(automatic_button, 0, Qt::AlignLeft);
		status_ = note(this, {}, "viewsPanelEditorStatus");
		layout->addWidget(status_);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
		buttons->button(QDialogButtonBox::Save)->setObjectName("viewsPanelSave");
		layout->addWidget(buttons);
		for (const QJsonValue value : pins)
			append_pin(value.toObject());
		fill_available();
		connect(search_, &QLineEdit::textChanged, this, [this] { filter_available(); });
		connect(available_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
			if (column != 0)
				return;
			remember_pin_sections();
			mark_custom();
			const bool checked = item->checkState(0) == Qt::Checked;
			if (item->childCount()) {
				for (int i = 0; i < item->childCount(); ++i)
					include_control(item->child(i), checked);
			} else {
				include_control(item, checked);
			}
			sync_checks();
			if (pins_->selectedItems().isEmpty() && pins_->count())
				pins_->setCurrentRow(pins_->count() - 1);
			show_pin();
		});
		connect(remove_, &QPushButton::clicked, this, [this] {
			remember_pin_sections();
			mark_custom();
			{
				const QSignalBlocker blocker(pins_);
				for (auto *item : pins_->selectedItems())
					delete pins_->takeItem(pins_->row(item));
			}
			sync_checks();
			show_pin();
		});
		connect(clear_, &QPushButton::clicked, this, [this] {
			remember_pin_sections();
			mark_custom();
			pins_->clear();
			sync_checks();
			show_pin();
		});
		connect(pins_, &QListWidget::itemSelectionChanged, this, [this] { show_pin(); });
		connect(pins_->model(), &QAbstractItemModel::rowsMoved, this, [this] { mark_custom(); });
		connect(label_, &QLineEdit::textChanged, this, [this] { edit_labels(); });
		connect(section_, &QLineEdit::textChanged, this, [this] {
			// The editable combo can publish its label before activated. The
			// original-section placeholder must never become a saved section.
			const bool original = section_->text() == section_choice_->itemText(0);
			edit_sections(original ? QString() : section_->text());
		});
		connect(section_, &QLineEdit::editingFinished, this, [this] { remember_pin_sections(); });
		connect(section_choice_, &QComboBox::activated, this, [this](int index) {
			edit_sections(section_choice_->itemData(index).toString());
			show_pin();
		});
		connect(add_section_, &QPushButton::clicked, this, [this] { add_section(); });
		connect(reset_section_, &QToolButton::clicked, this, [this] {
			remember_pin_sections();
			edit_sections({});
			show_pin();
		});
		connect(icon_library_, &QPushButton::clicked, this, [this] { choose_library_icon(); });
		connect(choose_icon_, &QPushButton::clicked, this, [this] { choose_icon(); });
		connect(remove_icon_, &QPushButton::clicked, this, [this] {
			const auto selected = pins_->selectedItems();
			if (selected.size() != 1)
				return;
			mark_custom();
			auto pin = pin_data(selected.front());
			pin.remove("iconPng");
			update_pin(selected.front(), pin);
			show_pin();
		});
		connect(automatic_button, &QPushButton::clicked, this, [this] {
			remember_pin_sections();
			automatic_ = true;
			pins_->clear();
			remembered_.clear();
			for (const QJsonValue value : inventory_)
				if (value.toObject()["defaultVisible"].toBool())
					append_pin(panel_pin(value.toObject()));
			sync_checks();
			show_pin();
			status_->setText(text("Controls.AutomaticPending"));
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, this, [this] {
			remember_pin_sections();
			QJsonArray pins, sections;
			for (int i = 0; i < pins_->count(); ++i)
				pins.append(pin_data(pins_->item(i)));
			for (const auto &name : custom_sections_)
				sections.append(name);
			apply_(pins, sections, !automatic_, switch_scene_->isChecked());
			accept();
		});
		if (pins_->count())
			pins_->setCurrentRow(0);
		show_pin();
		resize(860, 650);
	}

private:
	QJsonObject pin_data(const QListWidgetItem *item) const
	{
		return item ? QJsonObject::fromVariantMap(item->data(Qt::UserRole + 1).toMap()) : QJsonObject{};
	}

	QListWidgetItem *pin_item(const QString &id) const
	{
		for (int i = 0; i < pins_->count(); ++i)
			if (pins_->item(i)->data(Qt::UserRole).toString() == id)
				return pins_->item(i);
		return nullptr;
	}

	void mark_custom()
	{
		automatic_ = false;
		status_->clear();
	}

	void update_pin(QListWidgetItem *item, const QJsonObject &pin)
	{
		const auto control = find_id(inventory_, pin["id"].toString());
		QString name = panel_title(pin, control);
		if (control.isEmpty())
			name = text("Views.MissingItem").arg(name);
		item->setData(Qt::UserRole + 1, pin.toVariantMap());
		item->setText(name);
		item->setIcon(panel_icon(pin));
		item->setToolTip(panel_section(pin, control) + "\n" + control["section"].toString());
		remembered_.insert(pin["id"].toString(), pin);
	}

	void append_pin(const QJsonObject &pin)
	{
		const QString id = pin["id"].toString();
		if (id.isEmpty() || pin_item(id))
			return;
		auto *item = new QListWidgetItem(pins_);
		item->setData(Qt::UserRole, id);
		update_pin(item, remembered_.value(id, pin));
	}

	void include_control(QTreeWidgetItem *item, bool include)
	{
		const QString id = item->data(0, Qt::UserRole).toString();
		if (id.isEmpty())
			return;
		if (include)
			append_pin(panel_pin(QJsonObject::fromVariantMap(item->data(0, Qt::UserRole + 1).toMap())));
		else if (auto *pin = pin_item(id))
			delete pins_->takeItem(pins_->row(pin));
	}

	void fill_available()
	{
		const QSignalBlocker blocker(available_);
		available_->clear();
		QMap<QString, QTreeWidgetItem *> groups;
		for (const QJsonValue value : inventory_) {
			const auto control = value.toObject();
			const QString section = panel_section({}, control);
			auto *group = groups.value(section);
			if (!group) {
				group = new QTreeWidgetItem(available_, {section});
				group->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
				group->setToolTip(0, section);
				groups.insert(section, group);
			}
			auto *item = new QTreeWidgetItem(group, {control["name"].toString()});
			item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
			item->setData(0, Qt::UserRole, control["id"].toString());
			item->setData(0, Qt::UserRole + 1, control.toVariantMap());
			item->setToolTip(0, section + "\n" + control["name"].toString() + "\n" +
						    control["reason"].toString());
		}
		sync_checks();
		filter_available();
	}

	void sync_checks()
	{
		const QSignalBlocker blocker(available_);
		for (int i = 0; i < available_->topLevelItemCount(); ++i) {
			auto *group = available_->topLevelItem(i);
			int checked = 0;
			for (int j = 0; j < group->childCount(); ++j) {
				auto *item = group->child(j);
				const bool present = pin_item(item->data(0, Qt::UserRole).toString());
				item->setCheckState(0, present ? Qt::Checked : Qt::Unchecked);
				checked += present ? 1 : 0;
			}
			group->setCheckState(0, checked == 0                     ? Qt::Unchecked
						: checked == group->childCount() ? Qt::Checked
										 : Qt::PartiallyChecked);
		}
		selected_heading_->setText(text("Controls.OnPanelCount").arg(pins_->count()));
	}

	void filter_available()
	{
		const QString query = search_->text().trimmed();
		if (!query.isEmpty() && !searching_) {
			expanded_before_search_.clear();
			for (int i = 0; i < available_->topLevelItemCount(); ++i)
				if (available_->topLevelItem(i)->isExpanded())
					expanded_before_search_.insert(available_->topLevelItem(i)->text(0));
		}
		for (int i = 0; i < available_->topLevelItemCount(); ++i) {
			auto *group = available_->topLevelItem(i);
			bool any = false;
			for (int j = 0; j < group->childCount(); ++j) {
				auto *item = group->child(j);
				const bool match =
					(group->text(0) + "\n" + item->text(0)).contains(query, Qt::CaseInsensitive);
				item->setHidden(!match);
				any = any || match;
			}
			group->setHidden(!any);
			if (!query.isEmpty())
				group->setExpanded(any);
			else if (searching_)
				group->setExpanded(expanded_before_search_.contains(group->text(0)));
		}
		searching_ = !query.isEmpty();
	}

	void show_pin()
	{
		remember_pin_sections();
		const QSignalBlocker label_block(label_), section_block(section_),
			section_choice_block(section_choice_);
		const auto selected = pins_->selectedItems();
		const bool one = selected.size() == 1;
		const auto pin = one ? pin_data(selected.front()) : QJsonObject{};
		label_->setEnabled(one);
		label_->setText(pin["label"].toString());
		section_choice_->setEnabled(!selected.isEmpty());
		reset_section_->setEnabled(!selected.isEmpty());
		QString section = selected.isEmpty() ? QString() : pin_data(selected.front())["section"].toString();
		bool same_section = true;
		for (auto *item : selected)
			same_section = same_section && pin_data(item)["section"].toString() == section;
		section_->setPlaceholderText(text(same_section ? "Controls.UseSection" : "Controls.MixedSections"));
		section_choice_->setCurrentIndex(same_section ? section_choice_->findData(section) : -1);
		section_->setText(same_section ? section : QString());
		const auto control = find_id(inventory_, pin["id"].toString());
		context_->setText(one ? control["section"].toString(pin["context"].toString()) + "\n" +
						  control["name"].toString(pin["name"].toString())
				  : selected.isEmpty() ? text("Controls.SelectToEdit")
						       : text("Controls.EditSelection").arg(selected.size()));
		remove_->setEnabled(!selected.isEmpty());
		choose_icon_->setEnabled(one);
		icon_library_->setEnabled(one);
		remove_icon_->setEnabled(one && !pin["iconPng"].toString().isEmpty());
		icon_preview_->clear();
		if (one)
			icon_preview_->setPixmap(panel_icon(pin).pixmap(28, 28));
	}

	void edit_labels()
	{
		const auto selected = pins_->selectedItems();
		if (selected.size() != 1)
			return;
		mark_custom();
		auto pin = pin_data(selected.front());
		pin["label"] = label_->text();
		update_pin(selected.front(), pin);
	}

	bool remember_section(const QString &value)
	{
		const auto name = value.trimmed();
		if (name.isEmpty() || section_presets_.contains(name) || custom_sections_.contains(name))
			return false;
		custom_sections_.append(name);
		return true;
	}

	void fill_sections()
	{
		const QSignalBlocker section_block(section_), choice_block(section_choice_);
		const auto current = section_->text();
		section_choice_->clear();
		section_choice_->addItem(text("Controls.UseSection"), QString());
		for (const auto &name : section_presets_)
			section_choice_->addItem(name, name);
		for (const auto &name : custom_sections_)
			section_choice_->addItem(name, name);
		section_choice_->setCurrentIndex(section_choice_->findData(current));
		section_->setText(current);
	}

	void remember_pin_sections()
	{
		bool added = false;
		for (int i = 0; i < pins_->count(); ++i)
			added = remember_section(pin_data(pins_->item(i))["section"].toString()) || added;
		if (added)
			fill_sections();
	}

	void add_section()
	{
		remember_pin_sections();
		auto *dialog = new QDialog(this);
		dialog->setObjectName("viewsPanelNewSectionDialog");
		dialog->setWindowTitle(text("Controls.AddSectionTitle"));
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowModality(Qt::WindowModal);
		auto *layout = new QVBoxLayout(dialog);
		layout->addWidget(note(dialog, text("Controls.AddSectionHelp")));
		auto *name = new QLineEdit(dialog);
		name->setObjectName("viewsPanelNewSectionName");
		name->setPlaceholderText(text("Controls.SectionName"));
		name->setAccessibleName(text("Controls.SectionName"));
		layout->addWidget(name);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
		auto *save = buttons->addButton(text("Controls.AddSectionTitle"), QDialogButtonBox::AcceptRole);
		save->setObjectName("viewsPanelNewSectionSave");
		save->setEnabled(false);
		buttons->button(QDialogButtonBox::Cancel)->setObjectName("viewsPanelNewSectionCancel");
		layout->addWidget(buttons);
		connect(name, &QLineEdit::textChanged, dialog,
			[name, save] { save->setEnabled(!name->text().trimmed().isEmpty()); });
		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
		connect(buttons, &QDialogButtonBox::accepted, dialog, [this, dialog, name] {
			const auto value = name->text().trimmed();
			if (value.isEmpty())
				return;
			remember_section(value);
			fill_sections();
			edit_sections(value);
			show_pin();
			dialog->accept();
		});
		dialog->resize(400, 150);
		dialog->open();
		name->setFocus();
	}

	void edit_sections(const QString &section)
	{
		const auto selected = pins_->selectedItems();
		if (selected.isEmpty())
			return;
		mark_custom();
		for (auto *item : selected) {
			auto pin = pin_data(item);
			pin["section"] = section;
			update_pin(item, pin);
		}
	}

	void move_pins(int direction)
	{
		if (pins_->selectedItems().isEmpty())
			return;
		mark_custom();
		const QSignalBlocker blocker(pins_);
		auto *current = pins_->currentItem();
		const int start = direction < 0 ? 0 : pins_->count() - 1;
		for (int i = start; i >= 0 && i < pins_->count(); i -= direction) {
			const int next = i + direction;
			if (!pins_->item(i)->isSelected() || next < 0 || next >= pins_->count() ||
			    pins_->item(next)->isSelected())
				continue;
			auto *item = pins_->takeItem(i);
			pins_->insertItem(next, item);
			item->setSelected(true);
		}
		pins_->setCurrentItem(current, QItemSelectionModel::NoUpdate);
	}

	void set_icon(const QString &id, const QString &encoded)
	{
		auto *item = pin_item(id);
		if (!item || encoded.isEmpty())
			return;
		mark_custom();
		auto pin = pin_data(item);
		pin["iconPng"] = encoded;
		update_pin(item, pin);
		show_pin();
	}

	void choose_library_icon()
	{
		const auto selected = pins_->selectedItems();
		if (selected.size() != 1)
			return;
		const QString id = selected.front()->data(Qt::UserRole).toString();
		auto *picker = new ViewIconLibrary(this, [this, id](const QString &encoded) { set_icon(id, encoded); });
		picker->open();
	}

	void choose_icon()
	{
		const auto selected = pins_->selectedItems();
		if (selected.size() != 1)
			return;
		const QString id = selected.front()->data(Qt::UserRole).toString();
		auto *picker = new QFileDialog(this, text("Controls.ChooseIcon"));
		picker->setObjectName("viewsPanelIconDialog");
		picker->setAttribute(Qt::WA_DeleteOnClose);
		picker->setOption(QFileDialog::DontUseNativeDialog);
		picker->setFileMode(QFileDialog::ExistingFile);
		picker->setNameFilter(text("Controls.IconFiles"));
		picker->setWindowModality(Qt::WindowModal);
		connect(picker, &QFileDialog::accepted, this, [this, picker, id] {
			const auto files = picker->selectedFiles();
			auto *item = pin_item(id);
			if (!item || files.isEmpty())
				return;
			if (QFileInfo(files.front()).size() > 16 * 1024 * 1024) {
				status_->setText(text("Controls.IconError"));
				return;
			}
			QImageReader reader(files.front());
			reader.setAutoTransform(true);
			const QSize size = reader.size();
			if (!size.isValid() || size.width() > 16384 || size.height() > 16384 ||
			    int64_t(size.width()) * size.height() > 64 * 1024 * 1024) {
				status_->setText(text("Controls.IconError"));
				return;
			}
			reader.setScaledSize(size.scaled(128, 128, Qt::KeepAspectRatio));
			const auto encoded = embedded_panel_icon(reader.read());
			if (encoded.isEmpty()) {
				status_->setText(text("Controls.IconError"));
				return;
			}
			set_icon(id, encoded);
		});
		picker->open();
	}

	QJsonArray inventory_;
	QMap<QString, QJsonObject> remembered_;
	std::function<void(QJsonArray, QJsonArray, bool, bool)> apply_;
	QStringList section_presets_, custom_sections_;
	QComboBox *section_choice_ = nullptr;
	QLineEdit *search_ = nullptr, *label_ = nullptr, *section_ = nullptr;
	QTreeWidget *available_ = nullptr;
	QListWidget *pins_ = nullptr;
	QLabel *context_ = nullptr, *status_ = nullptr, *selected_heading_ = nullptr, *icon_preview_ = nullptr;
	QPushButton *remove_ = nullptr, *clear_ = nullptr, *choose_icon_ = nullptr, *remove_icon_ = nullptr,
		    *icon_library_ = nullptr, *add_section_ = nullptr;
	QCheckBox *switch_scene_ = nullptr;
	QToolButton *reset_section_ = nullptr;
	QSet<QString> expanded_before_search_;
	bool automatic_ = false, searching_ = false;
};

class ViewsControlPanel final : public QWidget {
public:
	ViewsControlPanel(QWidget *parent, std::function<void()> manage, std::function<void()> changed)
		: QWidget(parent),
		  changed_(std::move(changed))
	{
		setObjectName("viewsOperatorBoard");
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *tools = new QHBoxLayout;
		layout->addLayout(tools);
		edit_ = new QPushButton(text("Controls.EditPanel"), this);
		edit_->setObjectName("viewsPanelEdit");
		tools->addWidget(edit_);
		auto *configure = new QPushButton(text("Controls.Manage"), this);
		configure->setObjectName("viewsManageControllers");
		tools->addWidget(configure);
		connect(configure, &QPushButton::clicked, this, std::move(manage));
		connect(edit_, &QPushButton::clicked, this, [this] { edit(); });
		auto *display = new QHBoxLayout;
		layout->addLayout(display);
		scene_label_ = new QLabel(text("Views.Scene"), this);
		scene_label_->setObjectName("viewsPanelSceneLabel");
		display->addWidget(scene_label_);
		scene_choice_ = new QComboBox(this);
		scene_choice_->setObjectName("viewsPanelScene");
		scene_choice_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		scene_choice_->setMinimumContentsLength(6);
		scene_choice_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		scene_choice_->setToolTip(text("Controls.SceneTip"));
		scene_label_->setBuddy(scene_choice_);
		display->addWidget(scene_choice_, 1);
		auto *display_label = new QLabel(text("Controls.Layout"), this);
		display->addWidget(display_label);
		layout_choice_ = new QComboBox(this);
		layout_choice_->setObjectName("viewsPanelLayout");
		layout_choice_->addItem(text("Controls.List"), "list");
		layout_choice_->addItem(text("Controls.Buttons"), "buttons");
		display_label->setBuddy(layout_choice_);
		display->addWidget(layout_choice_);
		pages_ = new QStackedWidget(this);
		pages_->setObjectName("viewsPanelLayouts");
		layout->addWidget(pages_, 1);
		list_ = new ViewControlList(pages_);
		list_->setObjectName("viewsPanelList");
		list_->setHeaderHidden(true);
		list_->setUniformRowHeights(true);
		list_->setWordWrap(false);
		list_->setTextElideMode(Qt::ElideRight);
		list_->setSelectionMode(QAbstractItemView::NoSelection);
		list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		list_->header()->setSectionResizeMode(QHeaderView::Stretch);
		list_->setIndentation(16);
		list_->setIconSize(QSize(16, 16));
		list_->setAccessibleName(text("Controls.List"));
		pages_->addWidget(list_);
		scroll_ = new QScrollArea(pages_);
		scroll_->setObjectName("viewsPanelScroll");
		scroll_->setWidgetResizable(true);
		scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll_->setFrameShape(QFrame::NoFrame);
		scroll_->viewport()->installEventFilter(this);
		content_ = new QWidget(scroll_);
		content_->setObjectName("viewsPanelContent");
		content_layout_ = new QVBoxLayout(content_);
		content_layout_->setContentsMargins(0, 0, 0, 0);
		content_layout_->setSpacing(12);
		scroll_->setWidget(content_);
		pages_->addWidget(scroll_);
		size_controls_ = new QWidget(this);
		size_controls_->setObjectName("viewsPanelSizeControls");
		auto *sizes = new QHBoxLayout(size_controls_);
		sizes->setContentsMargins(0, 0, 0, 0);
		auto *size_label = new QLabel(text("Controls.ButtonSize"), size_controls_);
		sizes->addWidget(size_label);
		button_size_ = new QSlider(Qt::Horizontal, size_controls_);
		button_size_->setObjectName("viewsPanelButtonSize");
		button_size_->setRange(40, 112);
		button_size_->setSingleStep(4);
		button_size_->setPageStep(12);
		button_size_->setValue(48);
		button_size_->setAccessibleName(text("Controls.ButtonSize"));
		button_size_->setToolTip(text("Controls.ButtonSizeHelp"));
		size_label->setBuddy(button_size_);
		sizes->addWidget(button_size_, 1);
		layout->addWidget(size_controls_);
		status_ = note(this, {}, "viewsPanelStatus");
		layout->addWidget(status_);
		connect(scene_choice_, &QComboBox::activated, this, [this](int index) {
			const QString id = scene_choice_->itemData(index).toString();
			if (!id.isEmpty())
				press(id);
		});
		connect(layout_choice_, &QComboBox::currentIndexChanged, this, [this] {
			config_["layout"] = layout_choice_->currentData().toString();
			apply_display();
			if (loaded_)
				obs_frontend_save();
		});
		connect(button_size_, &QSlider::valueChanged, this, [this](int value) {
			config_["buttonSize"] = value;
			for (auto *button : buttons_)
				button->set_display_size(value);
			reflow();
			if (loaded_ && !button_size_->isSliderDown())
				obs_frontend_save();
		});
		connect(button_size_, &QSlider::sliderReleased, this, [this] {
			if (loaded_)
				obs_frontend_save();
		});
		list_->activate = [this](QTreeWidgetItem *item) {
			const QString id = item->data(0, Qt::UserRole).toString();
			if (!id.isEmpty())
				press(id);
		};
		connect(list_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
			if (item->flags() & Qt::ItemIsEnabled)
				list_->activate(item);
		});
		connect(list_, &QTreeWidget::itemExpanded, this, [this] { remember_collapsed(); });
		connect(list_, &QTreeWidget::itemCollapsed, this, [this] { remember_collapsed(); });
		apply_display();
	}

	void clear_collection()
	{
		if (editor_)
			editor_->close();
		config_ = {};
		apply_display();
		set_inventory({}, false);
	}

	void load(obs_data_t *root)
	{
		OBSDataAutoRelease data = obs_data_get_obj(root, "stagekit.views.panel");
		config_ = object(data);
		apply_display();
	}

	void save(obs_data_t *root)
	{
		if (config_["customized"].toBool()) {
			// Refresh fallback names without changing targets, icons or custom labels.
			auto pins = config_["pins"].toArray();
			for (int i = 0; i < pins.size(); ++i) {
				auto pin = pins[i].toObject();
				const auto control = find_id(inventory_, pin["id"].toString());
				if (!control.isEmpty()) {
					pin["name"] = control["name"];
					pin["context"] = control["section"];
					pins[i] = pin;
				}
			}
			config_["pins"] = pins;
		}
		const QByteArray json = QJsonDocument(config_).toJson(QJsonDocument::Compact);
		OBSDataAutoRelease data = obs_data_create_from_json(json.constData());
		obs_data_set_obj(root, "stagekit.views.panel", data);
	}

	void set_inventory(const QJsonArray &inventory, bool loaded)
	{
		inventory_ = inventory;
		loaded_ = loaded;
		refresh_scenes();
		edit_->setEnabled(loaded && !editor_);
		layout_choice_->setEnabled(loaded);
		button_size_->setEnabled(loaded);
		QJsonArray appearance;
		for (const QJsonValue value : effective_pins()) {
			const auto pin = value.toObject();
			const auto control = find_id(inventory_, pin["id"].toString());
			appearance.append(QJsonObject{{"id", pin["id"]},
						      {"target", pin["target"]},
						      {"name", panel_title(pin, control)},
						      {"section", panel_section(pin, control)},
						      {"iconPng", pin["iconPng"]}});
		}
		if (appearance != appearance_) {
			appearance_ = appearance;
			rebuild();
		}
		for (auto *button : buttons_) {
			const auto control = find_id(inventory_, button->property("viewsControlId").toString());
			const bool available = loaded && control["available"].toBool();
			const bool active = available && control["active"].toBool();
			const QSignalBlocker blocker(button);
			button->setChecked(active);
			button->setEnabled(available);
			QFont font = button->font();
			font.setBold(active);
			button->setFont(font);
			const QString reason = unavailable_reason(control, available);
			const QString context = button->property("viewsControlContext").toString();
			button->setToolTip(context + (reason.isEmpty() ? QString() : "\n" + reason));
			button->setAccessibleDescription(context + "\n" + (active ? text("Controls.Active") : reason));
		}
		for (auto *item : list_items_) {
			const auto control = find_id(inventory_, item->data(0, Qt::UserRole).toString());
			const bool available = loaded && control["available"].toBool();
			const bool active = available && control["active"].toBool();
			item->setFlags(available ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags);
			item->setData(0, Qt::UserRole + 2, active);
			QFont font = item->font(0);
			font.setBold(active);
			item->setFont(0, font);
			item->setBackground(0, active ? list_->palette().highlight() : QBrush());
			item->setForeground(0, active ? list_->palette().highlightedText() : QBrush());
			const QString reason = unavailable_reason(control, available);
			const QString context = item->data(0, Qt::UserRole + 3).toString();
			item->setToolTip(0, context + "\n" + item->text(0) +
						    (reason.isEmpty() ? QString() : "\n" + reason));
			item->setData(0, Qt::AccessibleDescriptionRole,
				      context + "\n" + (active ? text("Controls.Active") : reason));
		}
		status_->setText(!loaded              ? text("Views.Loading")
				 : buttons_.isEmpty() ? text(config_["customized"].toBool() ? "Controls.EmptyCustom"
											    : "Controls.Empty")
						      : QString());
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == scroll_->viewport() && event->type() == QEvent::Resize)
			reflow();
		return QWidget::eventFilter(watched, event);
	}

private:
	void refresh_scenes()
	{
		QJsonArray appearance;
		QString active;
		for (const QJsonValue value : inventory_) {
			const auto control = value.toObject();
			if (control["target"].toObject()["kind"].toString() != "scene")
				continue;
			appearance.append(QJsonObject{{"id", control["id"]},
						      {"name", control["name"]},
						      {"target", control["target"]}});
			if (control["active"].toBool())
				active = control["id"].toString();
		}
		const QSignalBlocker blocker(scene_choice_);
		if (appearance != scene_appearance_) {
			scene_appearance_ = appearance;
			scene_choice_->clear();
			for (const QJsonValue value : appearance) {
				const auto entry = value.toObject();
				scene_choice_->addItem(entry["name"].toString(), entry["id"].toString());
				scene_choice_->setItemData(scene_choice_->count() - 1,
							   entry["target"].toObject().toVariantMap(), Qt::UserRole + 1);
			}
		}
		scene_choice_->setCurrentIndex(active.isEmpty() ? -1 : scene_choice_->findData(active));
		scene_choice_->setEnabled(loaded_ && scene_choice_->count() > 0);
		const QString label =
			text(obs_frontend_preview_program_mode_active() ? "Controls.Preview" : "Views.Scene");
		scene_label_->setText(label);
		scene_choice_->setAccessibleName(label);
	}

	QString unavailable_reason(const QJsonObject &control, bool available) const
	{
		if (available)
			return {};
		const QString reason = control.isEmpty() ? text("Controls.Missing") : control["reason"].toString();
		return reason.isEmpty() ? text("Controls.NotReady") : reason;
	}

	void apply_display()
	{
		const bool buttons = config_["layout"].toString() == "buttons";
		const QSignalBlocker layout_block(layout_choice_), size_block(button_size_);
		layout_choice_->setCurrentIndex(buttons ? 1 : 0);
		button_size_->setValue(std::clamp(config_["buttonSize"].toInt(48), 40, 112));
		pages_->setCurrentWidget(buttons ? static_cast<QWidget *>(scroll_) : list_);
		size_controls_->setVisible(buttons);
		for (auto *button : buttons_)
			button->set_display_size(button_size_->value());
		reflow();
	}

	QJsonArray effective_pins() const
	{
		if (config_["customized"].toBool())
			return config_["pins"].toArray();
		QJsonArray pins;
		for (const QJsonValue value : inventory_)
			if (value.toObject()["defaultVisible"].toBool())
				pins.append(panel_pin(value.toObject()));
		return pins;
	}

	void rebuild()
	{
		const QSignalBlocker tree_block(list_);
		const QString current = list_->currentItem() ? list_->currentItem()->data(0, Qt::UserRole).toString()
							     : QString();
		const int scroll = list_->verticalScrollBar()->value();
		buttons_.clear();
		sections_.clear();
		list_items_.clear();
		list_->clear();
		while (auto *item = content_layout_->takeAt(0)) {
			if (auto *widget = item->widget())
				delete widget;
			delete item;
		}
		QSet<QString> collapsed;
		for (const QJsonValue value : config_["collapsedSections"].toArray())
			collapsed.insert(value.toString());
		for (const QJsonValue value : appearance_) {
			const auto entry = value.toObject();
			const QString title = entry["section"].toString();
			int at = 0;
			while (at < sections_.size() && sections_[at].title != title)
				++at;
			if (at == sections_.size()) {
				auto *section = new QWidget(content_);
				auto *layout = new QVBoxLayout(section);
				layout->setContentsMargins(0, 0, 0, 0);
				auto *label = new QLabel(title, section);
				label->setTextFormat(Qt::PlainText);
				label->setWordWrap(true);
				label->setObjectName("viewsPanelSectionHeading");
				QFont font = label->font();
				font.setBold(true);
				label->setFont(font);
				layout->addWidget(label);
				auto *grid = new QGridLayout;
				grid->setSpacing(6);
				layout->addLayout(grid);
				content_layout_->addWidget(section);
				auto *heading = new QTreeWidgetItem(list_, {title});
				heading->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
				heading->setFont(0, font);
				heading->setToolTip(0, title);
				sections_.append({title, grid, {}, heading});
			}
			const QIcon icon = panel_icon(entry);
			auto *button = new ViewControlButton(entry["name"].toString(), content_);
			button->setObjectName("viewsPanelButton-" + entry["id"].toString());
			button->setProperty("viewsControlId", entry["id"].toString());
			button->setProperty("viewsControlTarget", entry["target"].toObject().toVariantMap());
			button->setProperty("viewsControlContext", entry["section"].toString());
			button->setAccessibleName(entry["name"].toString());
			button->setCheckable(true);
			button->setIcon(icon);
			button->set_display_size(button_size_->value());
			button->setStyleSheet(
				"QPushButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }");
			auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
			policy.setHeightForWidth(true);
			button->setSizePolicy(policy);
			sections_[at].buttons.append(button);
			buttons_.append(button);
			connect(button, &QPushButton::clicked, this, [this, button] {
				const QString id = button->property("viewsControlId").toString();
				const auto control = find_id(inventory_, id);
				button->setChecked(control["available"].toBool() && control["active"].toBool());
				press(id);
			});
			auto *item = new QTreeWidgetItem(sections_[at].heading, {entry["name"].toString()});
			item->setData(0, Qt::UserRole, entry["id"].toString());
			item->setData(0, Qt::UserRole + 1, entry["target"].toObject().toVariantMap());
			item->setData(0, Qt::UserRole + 3, title);
			item->setIcon(0, icon);
			list_items_.append(item);
			if (entry["id"].toString() == current)
				list_->setCurrentItem(item);
		}
		for (const auto &section : sections_)
			section.heading->setExpanded(!collapsed.contains(section.title));
		content_layout_->addStretch(1);
		list_->verticalScrollBar()->setValue(scroll);
		reflow();
	}

	void remember_collapsed()
	{
		QJsonArray collapsed;
		for (const auto &section : sections_)
			if (!section.heading->isExpanded())
				collapsed.append(section.title);
		config_["collapsedSections"] = collapsed;
		if (loaded_)
			obs_frontend_save();
	}

	void press(const QString &id)
	{
		// Resolve again in the native catalog. UI focus/selection never decides
		// whether a command is needed, and native state remains authoritative.
		const auto control = find_id(inventory_, id);
		const QByteArray json = QJsonDocument(control["target"].toObject()).toJson(QJsonDocument::Compact);
		OBSDataAutoRelease data = obs_data_create_from_json(json.constData());
		std::string why;
		if (!view_control_press(data, why, config_["showScene"].toBool(true)))
			status_->setText(QString::fromStdString(why));
		else {
			status_->setText(text("Views.Applying"));
			changed_();
		}
	}

	void reflow()
	{
		const int width = button_size_ ? button_size_->value() * 3 + 6 : 150;
		const int columns = std::clamp((scroll_->viewport()->width() + 6) / width, 1, 6);
		for (auto &section : sections_) {
			for (auto *button : section.buttons)
				section.grid->removeWidget(button);
			for (int i = 0; i < 6; ++i)
				section.grid->setColumnStretch(i, i < columns ? 1 : 0);
			for (int i = 0; i < section.buttons.size(); ++i)
				section.grid->addWidget(section.buttons[i], i / columns, i % columns);
		}
	}

	void edit()
	{
		if (editor_) {
			editor_->raise();
			editor_->activateWindow();
			return;
		}
		OBSDataAutoRelease response = obs_data_create();
		view_controls_list(response);
		const bool ready = loaded_ && obs_data_get_bool(response, "available");
		set_inventory(object(response)["controls"].toArray(), ready);
		if (!ready)
			return;
		editor_ =
			new ViewPanelEditor(this, inventory_, effective_pins(), config_["customSections"].toArray(),
					    config_["showScene"].toBool(true),
					    [this](QJsonArray pins, QJsonArray sections, bool custom, bool show_scene) {
						    config_["schema"] = 1;
						    config_["customized"] = custom;
						    config_["pins"] = pins;
						    config_["customSections"] = sections;
						    config_["showScene"] = show_scene;
						    set_inventory(inventory_, loaded_);
						    obs_frontend_save();
					    });
		connect(editor_, &QDialog::finished, this, [this] {
			editor_ = nullptr;
			edit_->setEnabled(loaded_);
		});
		edit_->setEnabled(false);
		editor_->open();
	}

	struct Section {
		QString title;
		QGridLayout *grid;
		QList<ViewControlButton *> buttons;
		QTreeWidgetItem *heading;
	};
	QList<Section> sections_;
	QList<ViewControlButton *> buttons_;
	QList<QTreeWidgetItem *> list_items_;
	QJsonArray inventory_, appearance_, scene_appearance_;
	QJsonObject config_;
	std::function<void()> changed_;
	QPushButton *edit_ = nullptr;
	QComboBox *layout_choice_ = nullptr, *scene_choice_ = nullptr;
	QStackedWidget *pages_ = nullptr;
	ViewControlList *list_ = nullptr;
	QScrollArea *scroll_ = nullptr;
	QWidget *content_ = nullptr, *size_controls_ = nullptr;
	QSlider *button_size_ = nullptr;
	QVBoxLayout *content_layout_ = nullptr;
	QLabel *status_ = nullptr, *scene_label_ = nullptr;
	QPointer<ViewPanelEditor> editor_;
	bool loaded_ = false;
};

class ViewsDock final : public QWidget {
public:
	ViewsDock()
	{
		setObjectName("stagekitViewsDock");
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(8, 8, 8, 8);
		pages_ = new QStackedWidget(this);
		pages_->setObjectName("viewsPages");
		outer->addWidget(pages_);
		panel_ = new ViewsControlPanel(
			pages_,
			[this] {
				pages_->setCurrentWidget(manager_);
				refresh(true);
			},
			[this] {
				queue_refresh(true);
				QTimer::singleShot(150, this, [this] { queue_refresh(); });
			});
		pages_->addWidget(panel_);
		manager_ = new QWidget(pages_);
		manager_->setObjectName("viewsControllerManager");
		pages_->addWidget(manager_);
		auto *layout = new QVBoxLayout(manager_);
		layout->setContentsMargins(0, 0, 0, 0);
		auto *back = new QPushButton(text("Controls.Back"), manager_);
		back->setObjectName("viewsBackToControls");
		layout->addWidget(back);
		connect(back, &QPushButton::clicked, this, [this] {
			pages_->setCurrentWidget(panel_);
			queue_refresh(true);
		});
		auto *form = new QFormLayout;
		layout->addLayout(form);
		scenes_ = new QComboBox(this);
		scenes_->setObjectName("viewsScene");
		scenes_->setToolTip(text("Views.SceneTip"));
		form->addRow(text("Views.Scene"), scenes_);
		auto *controller_row = new QHBoxLayout;
		controllers_ = new QComboBox(this);
		controllers_->setObjectName("viewsController");
		controllers_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		controller_row->addWidget(controllers_, 1);
		create_ = new QToolButton(this);
		create_->setObjectName("viewsCreate");
		create_->setText(text("Views.Add"));
		create_->setToolTip(text("Views.CreateController"));
		create_->setAccessibleName(text("Views.CreateController"));
		controller_row->addWidget(create_);
		form->addRow(text("Views.Controller"), controller_row);
		enabled_ = new QCheckBox(text("Views.ControllerEnabled"), this);
		enabled_->setObjectName("viewsEnabled");
		form->addRow(enabled_);
		scope_ = new QComboBox(this);
		scope_->setObjectName("viewsScope");
		form->addRow(text("Views.Show"), scope_);
		list_ = new QListWidget(this);
		list_->setObjectName("viewsList");
		list_->setSelectionMode(QAbstractItemView::SingleSelection);
		list_->setMinimumHeight(120);
		list_->setAlternatingRowColors(true);
		layout->addWidget(list_, 1);
		status_ = note(this, text("Views.Loading"), "viewsStatus");
		layout->addWidget(status_);
		auto *actions = new QHBoxLayout;
		configure_ = new QPushButton(text("Views.Configure"), this);
		configure_->setObjectName("viewsConfigure");
		actions->addWidget(configure_, 1);
		import_ = new QPushButton(text("Views.Import"), this);
		import_->setObjectName("viewsImportLegacy");
		actions->addWidget(import_);
		auto *more = new QToolButton(this);
		more->setObjectName("viewsMore");
		more->setText(text("Views.More"));
		more->setPopupMode(QToolButton::InstantPopup);
		auto *menu = new QMenu(more);
		rename_ = menu->addAction(text("Views.RenameController"));
		rename_->setObjectName("viewsRenameController");
		remove_ = menu->addAction(text("Views.RemoveController"));
		remove_->setObjectName("viewsRemoveController");
		more->setMenu(menu);
		actions->addWidget(more);
		layout->addLayout(actions);
		connect(scenes_, &QComboBox::currentIndexChanged, this, [this] {
			selected_scene_ = scenes_->currentData().toString();
			selected_controller_.clear();
			selected_scope_.clear();
			refresh(true);
			persist_selection();
		});
		connect(controllers_, &QComboBox::currentIndexChanged, this, [this] {
			selected_controller_ = controllers_->currentData().toString();
			selected_scope_.clear();
			refresh(true);
			persist_selection();
		});
		connect(scope_, &QComboBox::currentIndexChanged, this, [this] {
			selected_scope_ = scope_->currentData().toString();
			scope_is_views_ = scope_->currentIndex() == 0;
			redraw_state();
			persist_selection();
		});
		connect(enabled_, &QCheckBox::toggled, this, [this](bool enabled) {
			OBSSourceAutoRelease source = controller_source(state_);
			if (source) {
				obs_source_set_enabled(source, enabled);
				obs_frontend_save();
				refresh(true);
			}
		});
		connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) { select(item); });
		connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { select(item); });
		connect(create_, &QToolButton::clicked, this, [this] { create(); });
		connect(configure_, &QPushButton::clicked, this, [this] { configure(); });
		connect(import_, &QPushButton::clicked, this, [this] { import_legacy(); });
		connect(rename_, &QAction::triggered, this, [this] { rename_controller(); });
		connect(remove_, &QAction::triggered, this, [this] { remove_controller(); });
		fallback_.setInterval(2000);
		connect(&fallback_, &QTimer::timeout, this, [this] {
			if (loaded_ && isVisible())
				refresh(false);
		});
		for (const char *signal : {"source_create", "source_remove"})
			signal_handler_connect(obs_get_signal_handler(), signal, structure_changed, this);
		for (const char *signal : {"source_rename", "stagekit_scene_controller_changed"})
			signal_handler_connect(obs_get_signal_handler(), signal, changed, this);
		refresh(false);
	}

	~ViewsDock() override { shutdown(); }

	void shutdown()
	{
		if (closed_)
			return;
		closed_ = true;
		fallback_.stop();
		for (const char *signal : {"source_create", "source_remove"})
			signal_handler_disconnect(obs_get_signal_handler(), signal, structure_changed, this);
		for (const char *signal : {"source_rename", "stagekit_scene_controller_changed"})
			signal_handler_disconnect(obs_get_signal_handler(), signal, changed, this);
		unwatch();
		panel_->clear_collection();
		if (editor_)
			editor_->close();
	}

	void clear_collection()
	{
		loaded_ = false;
		fallback_.stop();
		unwatch();
		watch_dirty_ = true;
		panel_->clear_collection();
		pages_->setCurrentWidget(panel_);
		if (editor_)
			editor_->close();
		for (auto *dialog : findChildren<QDialog *>())
			dialog->close();
		state_ = {};
		selected_scene_.clear();
		selected_controller_.clear();
		selected_scope_.clear();
		refresh(true);
	}

	void open_collection()
	{
		if (closed_)
			return;
		loaded_ = true;
		if (isVisible())
			fallback_.start();
		queue_refresh();
	}

	void save_load(obs_data_t *root, bool saving)
	{
		if (saving) {
			if (!loaded_)
				return;
			OBSDataAutoRelease settings = obs_data_create();
			obs_data_set_string(settings, "scene", selected_scene_.toUtf8().constData());
			obs_data_set_string(settings, "controller", selected_controller_.toUtf8().constData());
			obs_data_set_string(settings, "scope", selected_scope_.toUtf8().constData());
			obs_data_set_string(settings, "scope_kind", scope_is_views_ ? "views" : "group");
			obs_data_set_obj(root, "stagekit.views", settings);
			panel_->save(root);
		} else if (!closed_) {
			clear_collection();
			panel_->load(root);
			OBSDataAutoRelease settings = obs_data_get_obj(root, "stagekit.views");
			selected_scene_ = QString::fromUtf8(obs_data_get_string(settings, "scene"));
			selected_controller_ = QString::fromUtf8(obs_data_get_string(settings, "controller"));
			selected_scope_ = QString::fromUtf8(obs_data_get_string(settings, "scope"));
			scope_is_views_ = std::strcmp(obs_data_get_string(settings, "scope_kind"), "group") != 0;
			open_collection();
		}
	}

	void queue_refresh(bool force = false)
	{
		force_refresh_ = force_refresh_ || force;
		if (pending_ || closed_)
			return;
		pending_ = true;
		QTimer::singleShot(40, this, [this] {
			pending_ = false;
			const bool force = std::exchange(force_refresh_, false);
			refresh(force);
		});
	}

	void show_controller(const QString &id)
	{
		pages_->setCurrentWidget(manager_);
		if (raise_import())
			return;
		if (editor_ && editor_->controller_id() != id) {
			status_->setText(text("Views.FinishEditing"));
			editor_->raise();
			editor_->activateWindow();
			return;
		}
		const auto target = snapshot(id);
		if (target.isEmpty())
			return;
		selected_scene_ = target["sceneUuid"].toString();
		selected_controller_ = id;
		selected_scope_.clear();
		loaded_ = true;
		if (auto *container = qobject_cast<QDockWidget *>(parentWidget())) {
			container->show();
			container->raise();
		}
		refresh(true);
		persist_selection();
		configure();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (loaded_ && !closed_)
			fallback_.start();
		queue_refresh(true);
	}

	void hideEvent(QHideEvent *event) override
	{
		fallback_.stop();
		QWidget::hideEvent(event);
	}

private:
	static void structure_changed(void *data, calldata_t *)
	{
		auto *self = static_cast<ViewsDock *>(data);
		QMetaObject::invokeMethod(
			self,
			[self] {
				self->watch_dirty_ = true;
				self->queue_refresh();
			},
			Qt::QueuedConnection);
	}

	static void changed(void *data, calldata_t *)
	{
		auto *self = static_cast<ViewsDock *>(data);
		// Backend notifications can originate on the video thread. Snapshot
		// only after returning to the UI, and combine bursts into one refresh.
		QMetaObject::invokeMethod(self, [self] { self->queue_refresh(); }, Qt::QueuedConnection);
	}

	void unwatch()
	{
		std::vector<obs_weak_source_t *> watched;
		watched.swap(watched_);
		for (auto *weak : watched) {
			OBSSourceAutoRelease source = obs_weak_source_get_source(weak);
			if (source) {
				auto *handler = obs_source_get_signal_handler(source);
				if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
					for (const char *signal : {"item_add", "item_remove", "reorder", "refresh"})
						signal_handler_disconnect(handler, signal, structure_changed, this);
					signal_handler_disconnect(handler, "item_visible", changed, this);
				} else {
					signal_handler_disconnect(handler, "enable", changed, this);
				}
			}
			obs_weak_source_release(weak);
		}
	}

	void watch()
	{
		watch_dirty_ = false;
		unwatch();
		std::vector<obs_source_t *> held;
		const auto collect = [](void *data, obs_source_t *source) {
			auto &out = *static_cast<std::vector<obs_source_t *> *>(data);
			if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE)
				if (auto *ref = obs_source_get_ref(source))
					out.push_back(ref);
			obs_source_enum_filters(
				source,
				[](obs_source_t *, obs_source_t *filter, void *opaque) {
					const char *id = obs_source_get_unversioned_id(filter);
					if (!id || (std::strcmp(id, "stagekit_scene_controller") != 0 &&
						    std::strcmp(id, "stagekit_scene_modes") != 0 &&
						    std::strcmp(id, "stagekit_visibility_link") != 0 &&
						    std::strcmp(id, "stagekit_show_in_scenes") != 0))
						return;
					if (auto *ref = obs_source_get_ref(filter))
						static_cast<std::vector<obs_source_t *> *>(opaque)->push_back(ref);
				},
				data);
			return true;
		};
		obs_enum_sources(collect, &held);
		obs_enum_scenes(collect, &held);
		QSet<QString> seen;
		for (auto *source : held) {
			const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
			if (!seen.contains(uuid) && !obs_source_removed(source)) {
				seen.insert(uuid);
				auto *handler = obs_source_get_signal_handler(source);
				if (obs_source_get_type(source) == OBS_SOURCE_TYPE_SCENE) {
					for (const char *signal : {"item_add", "item_remove", "reorder", "refresh"})
						signal_handler_connect(handler, signal, structure_changed, this);
					signal_handler_connect(handler, "item_visible", changed, this);
				} else {
					signal_handler_connect(handler, "enable", changed, this);
				}
				watched_.push_back(obs_source_get_weak_source(source));
			}
			obs_source_release(source);
		}
	}

	void persist_selection()
	{
		if (loaded_ && !closed_)
			obs_frontend_save();
	}

	void refresh(bool force)
	{
		if (closed_ || (loaded_ && !isVisible()))
			return;
		if (loaded_ && watch_dirty_)
			watch();
		OBSDataAutoRelease controls = obs_data_create();
		if (loaded_)
			view_controls_list(controls);
		panel_->set_inventory(object(controls)["controls"].toArray(),
				      loaded_ && obs_data_get_bool(controls, "available"));
		if (loaded_ && pages_->currentWidget() != manager_)
			return;
		const QSignalBlocker scene_block(scenes_), controller_block(controllers_), scope_block(scope_),
			enabled_block(enabled_);
		const auto available_scenes = loaded_ ? scenes() : QList<Scene>{};
		if (selected_scene_.isEmpty()) {
			OBSSourceAutoRelease program = loaded_ ? obs_frontend_get_current_scene() : nullptr;
			if (program)
				selected_scene_ = QString::fromUtf8(obs_source_get_uuid(program));
		}
		QList<QPair<QString, QString>> scene_items;
		for (const auto &scene : available_scenes)
			scene_items.append({scene.name, scene.uuid});
		combo_items(scenes_, scene_items, selected_scene_);
		selected_scene_ = scenes_->currentData().toString();
		OBSDataAutoRelease response = obs_data_create();
		if (loaded_)
			scene_controller_list(response);
		QJsonObject next;
		QList<QPair<QString, QString>> controller_items;
		for (const QJsonValue entry : object(response)["controllers"].toArray()) {
			const auto controller = entry.toObject();
			if (controller["sceneUuid"].toString() != selected_scene_)
				continue;
			controller_items.append({controller["name"].toString(), controller["controllerId"].toString()});
			if (controller["controllerId"].toString() == selected_controller_ || next.isEmpty())
				next = controller;
		}
		if (!next.isEmpty())
			selected_controller_ = next["controllerId"].toString();
		else
			selected_controller_.clear();
		combo_items(controllers_, controller_items, selected_controller_);
		const bool changed_state = force || next != state_;
		state_ = next;
		enabled_->setChecked(state_["enabled"].toBool());
		enabled_->setEnabled(!state_.isEmpty());
		create_->setEnabled(loaded_ && !selected_scene_.isEmpty());
		configure_->setEnabled(!state_.isEmpty() && !editor_);
		import_->setEnabled(!state_.isEmpty() && !editor_);
		rename_->setEnabled(!state_.isEmpty() && !editor_);
		remove_->setEnabled(!state_.isEmpty() && !editor_);
		QList<QPair<QString, QString>> scope_items{{text("Views.CombinedViews"), "views"}};
		for (const QJsonValue entry : state_["groups"].toArray()) {
			const auto group = entry.toObject();
			scope_items.append({group["name"].toString(), group["id"].toString()});
		}
		if (selected_scope_.isEmpty()) {
			scope_is_views_ = !state_["views"].toArray().isEmpty() || scope_items.size() == 1;
			selected_scope_ = scope_is_views_ ? QStringLiteral("views") : scope_items[1].second;
		}
		combo_items(scope_, scope_items, selected_scope_);
		// Keep the target kind separate: a valid imported group ID can be
		// literally "views", the same string used for the combined-view row.
		int scope_index = 0;
		if (!scope_is_views_)
			for (int i = 1; i < scope_items.size(); ++i)
				if (scope_items[i].second == selected_scope_) {
					scope_index = i;
					break;
				}
		scope_->setCurrentIndex(scope_index);
		for (int i = 0; i < scope_->count(); ++i)
			scope_->setItemData(i, i == 0 ? "views" : "group", Qt::UserRole + 1);
		selected_scope_ = scope_->currentData().toString();
		scope_is_views_ = scope_index == 0;
		scope_->setEnabled(!state_.isEmpty());
		if (changed_state || !loaded_)
			redraw_state();
	}

	void redraw_state()
	{
		const QSignalBlocker block(list_);
		list_->clear();
		if (!loaded_ || state_.isEmpty()) {
			status_->setText(text(loaded_ ? "Views.NoController" : "Views.Loading"));
			return;
		}
		const bool enabled = state_["enabled"].toBool();
		const bool controller_available = state_["available"].toBool();
		const bool is_views = scope_is_views_;
		const auto group = is_views ? QJsonObject{} : find_id(state_["groups"].toArray(), selected_scope_);
		const bool group_enabled = is_views || group["enabled"].toBool();
		const bool group_available = is_views || group["available"].toBool();
		QJsonArray entries = is_views ? state_["views"].toArray() : group["choices"].toArray();
		if (!is_views && group["kind"].toString() == "modes" && group["allowNone"].toBool())
			entries.append(QJsonObject{{"id", ""},
						   {"name", text("Views.None")},
						   {"active", group["current"].toString().isEmpty()},
						   {"available", true}});
		for (const QJsonValue entry : entries) {
			const auto choice = entry.toObject();
			const bool available = enabled && controller_available && group_enabled && group_available &&
					       choice["available"].toBool();
			const bool active = available && choice["active"].toBool();
			const QString label = choice["name"].toString();
			auto *item = new QListWidgetItem(active ? text("Views.ActiveItem").arg(label) : label, list_);
			item->setData(Qt::UserRole, choice["id"].toString());
			item->setData(Qt::UserRole + 1, active);
			QFont font = item->font();
			font.setBold(active);
			item->setFont(font);
			if (active)
				item->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
			if (!available)
				item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
			QString reason = choice["reason"].toString();
			if (reason.isEmpty())
				reason = group["reason"].toString();
			if (reason.isEmpty())
				reason = state_["reason"].toString();
			if (!reason.isEmpty())
				item->setToolTip(reason);
			if (active)
				list_->setCurrentItem(item);
		}
		if (!enabled)
			status_->setText(text("Views.ControllerOff"));
		else if (!group_enabled)
			status_->setText(text("Views.GroupOff"));
		else if (!controller_available)
			status_->setText(state_["reason"].toString());
		else if (!group_available)
			status_->setText(group["reason"].toString());
		else if (!is_views && group["kind"].toString() == "follow")
			status_->setText(group["reason"].toString().isEmpty() ? text("Views.FollowHelp")
									      : group["reason"].toString());
		else if (entries.isEmpty())
			status_->setText(text(is_views ? "Views.EmptyViews" : "Views.EmptyModes"));
		else if (!state_["error"].toString().isEmpty())
			status_->setText(state_["error"].toString());
		else
			status_->setText(text("Views.SelectHelp"));
	}

	void select(QListWidgetItem *item)
	{
		if (!item || !(item->flags() & Qt::ItemIsEnabled))
			return;
		const QByteArray controller = selected_controller_.toUtf8();
		const QByteArray group = scope_is_views_ ? QByteArray() : selected_scope_.toUtf8();
		const QByteArray selected = item->data(Qt::UserRole).toString().toUtf8();
		const QByteArray choice = scope_is_views_ ? QByteArray() : selected;
		const QByteArray view = scope_is_views_ ? selected : QByteArray();
		std::string why;
		const char *command = !group.isEmpty() && choice.isEmpty() ? "none" : "select";
		if (!scene_controller_command(controller.constData(), group.constData(), choice.constData(),
					      view.constData(), command, why))
			status_->setText(QString::fromStdString(why));
		else {
			status_->setText(text("Views.Applying"));
			queue_refresh(true);
		}
	}

	void create()
	{
		const QString scene_id = selected_scene_;
		name_dialog(this, "viewsCreateDialog", text("Views.CreateController"), text("Views.DefaultController"),
			    [this, scene_id](const QString &name, QString &error) {
				    OBSDataAutoRelease response = obs_data_create();
				    std::string why;
				    if (!scene_controller_create(scene_id.toUtf8().constData(),
								 name.toUtf8().constData(), response, why)) {
					    error = QString::fromStdString(why);
					    return false;
				    }
				    selected_scene_ = scene_id;
				    selected_controller_ = object(response)["controllerId"].toString();
				    selected_scope_.clear();
				    refresh(true);
				    persist_selection();
				    return true;
			    });
	}

	void configure()
	{
		if (raise_import())
			return;
		if (editor_) {
			editor_->raise();
			editor_->activateWindow();
			return;
		}
		QString error;
		const auto current = snapshot(selected_controller_, &error);
		if (current.isEmpty()) {
			status_->setText(error);
			return;
		}
		editor_ = new ViewsEditor(this, current);
		connect(editor_, &QDialog::finished, this, [this] {
			editor_ = nullptr;
			refresh(true);
		});
		editor_->open();
		refresh(false);
	}

	void rename_controller()
	{
		const QJsonObject current = state_;
		name_dialog(this, "viewsRenameDialog", text("Views.RenameController"), current["name"].toString(),
			    [this, current](const QString &name, QString &error) {
				    OBSSourceAutoRelease source = controller_source(current);
				    if (!source) {
					    error = text("Views.ControllerMissing");
					    return false;
				    }
				    obs_source_set_name(source, name.toUtf8().constData());
				    obs_frontend_save();
				    refresh(true);
				    return true;
			    });
	}

	void remove_controller()
	{
		const auto current = state_;
		auto *confirm = new QMessageBox(QMessageBox::Question, text("Views.RemoveController"),
						text("Views.RemoveControllerPrompt").arg(current["name"].toString()),
						QMessageBox::Yes | QMessageBox::No, this);
		confirm->setObjectName("viewsRemoveControllerConfirm");
		confirm->setAttribute(Qt::WA_DeleteOnClose);
		confirm->setDefaultButton(QMessageBox::No);
		connect(confirm, &QMessageBox::finished, this, [this, current](int answer) {
			if (answer != QMessageBox::Yes)
				return;
			OBSSourceAutoRelease source = controller_source(current);
			OBSSourceAutoRelease parent =
				obs_get_source_by_uuid(current["sceneUuid"].toString().toUtf8().constData());
			if (source && parent) {
				obs_source_filter_remove(parent, source);
				obs_frontend_save();
			}
			selected_controller_.clear();
			selected_scope_.clear();
			refresh(true);
		});
		confirm->open();
	}

	void import_legacy()
	{
		if (editor_) {
			editor_->raise();
			editor_->activateWindow();
			return;
		}
		if (raise_import())
			return;
		const QString controller_id = selected_controller_;
		auto *dialog = new QDialog(this);
		dialog->setObjectName("viewsImportDialog");
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(text("Views.ImportTitle"));
		auto *layout = new QVBoxLayout(dialog);
		layout->addWidget(note(dialog, text("Views.ImportHelp")));
		auto *filters = new QComboBox(dialog);
		filters->setObjectName("viewsImportFilter");
		OBSSourceAutoRelease scene = obs_get_source_by_uuid(selected_scene_.toUtf8().constData());
		if (scene)
			obs_source_enum_filters(
				scene,
				[](obs_source_t *, obs_source_t *source, void *data) {
					const char *id = obs_source_get_id(source);
					if (std::strcmp(id, "stagekit_scene_modes") == 0 ||
					    std::strcmp(id, "stagekit_visibility_link") == 0)
						static_cast<QComboBox *>(data)->addItem(
							QString::fromUtf8(obs_source_get_name(source)),
							QString::fromUtf8(obs_source_get_uuid(source)));
				},
				filters);
		layout->addWidget(filters);
		auto *activate = new QCheckBox(text("Views.ImportActivate"), dialog);
		activate->setObjectName("viewsImportActivate");
		activate->setChecked(true);
		layout->addWidget(activate);
		layout->addWidget(note(dialog, text("Views.ImportActivateHelp")));
		auto *error = note(dialog, filters->count() ? QString() : text("Views.NoLegacy"), "viewsImportError");
		layout->addWidget(error);
		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
		auto *import_button = buttons->addButton(text("Views.Import"), QDialogButtonBox::ActionRole);
		import_button->setObjectName("viewsImportConfirm");
		import_button->setEnabled(filters->count() > 0);
		auto *import_all = buttons->addButton(text("Views.ImportAll"), QDialogButtonBox::ActionRole);
		import_all->setObjectName("viewsImportAll");
		import_all->setToolTip(text("Views.ImportAllHelp"));
		import_all->setEnabled(filters->count() > 0);
		layout->addWidget(buttons);
		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
		connect(import_all, &QPushButton::clicked, dialog, [=] {
			OBSDataAutoRelease response = obs_data_create();
			std::string why;
			if (!scene_controller_import_all(controller_id.toUtf8().constData(), activate->isChecked(),
							 response, why)) {
				error->setText(QString::fromStdString(why));
				return;
			}
			selected_scope_.clear();
			obs_frontend_save();
			queue_refresh();
			error->setText(text("Views.ImportAllResult")
					       .arg(obs_data_get_int(response, "importedCount"))
					       .arg(obs_data_get_int(response, "skippedCount")));
			buttons->button(QDialogButtonBox::Cancel)->setText(text("Views.Close"));
		});
		connect(import_button, &QPushButton::clicked, dialog, [=] {
			OBSDataAutoRelease response = obs_data_create();
			std::string why;
			if (!scene_controller_import(controller_id.toUtf8().constData(),
						     filters->currentData().toString().toUtf8().constData(),
						     activate->isChecked(), response, why)) {
				error->setText(QString::fromStdString(why));
				return;
			}
			selected_scope_.clear();
			obs_frontend_save();
			queue_refresh();
			dialog->accept();
		});
		dialog->resize(450, dialog->sizeHint().height());
		dialog->open();
	}

	bool raise_import()
	{
		for (auto *dialog : findChildren<QDialog *>("viewsImportDialog"))
			if (dialog->isVisible()) {
				dialog->raise();
				dialog->activateWindow();
				return true;
			}
		return false;
	}

	QStackedWidget *pages_ = nullptr;
	QWidget *manager_ = nullptr;
	ViewsControlPanel *panel_ = nullptr;
	std::vector<obs_weak_source_t *> watched_;
	bool watch_dirty_ = true;
	QComboBox *scenes_ = nullptr, *controllers_ = nullptr, *scope_ = nullptr;
	QCheckBox *enabled_ = nullptr;
	QListWidget *list_ = nullptr;
	QLabel *status_ = nullptr;
	QToolButton *create_ = nullptr;
	QPushButton *configure_ = nullptr, *import_ = nullptr;
	QAction *rename_ = nullptr, *remove_ = nullptr;
	QPointer<ViewsEditor> editor_;
	QTimer fallback_;
	QJsonObject state_;
	QString selected_scene_, selected_controller_, selected_scope_;
	bool loaded_ = false, closed_ = false, pending_ = false, force_refresh_ = false, scope_is_views_ = true;
};

QPointer<ViewsDock> dock;
bool registered = false;

void collection_data(obs_data_t *root, bool saving, void *)
{
	if (dock)
		dock->save_load(root, saving);
}

void frontend_event(obs_frontend_event event, void *)
{
	if (!dock)
		return;
	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
		dock->shutdown();
	else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP)
		dock->clear_collection();
	else if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		dock->open_collection();
	else if (event == OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED || event == OBS_FRONTEND_EVENT_SCENE_CHANGED ||
		 event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED ||
		 event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED)
		dock->queue_refresh();
}

} // namespace

void register_views_dock()
{
	if (registered || !obs_frontend_get_main_window())
		return;
	dock = new ViewsDock;
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("Views.Dock"), dock.data())) {
		delete dock.data();
		return;
	}
	obs_frontend_add_save_callback(collection_data, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	registered = true;
}

void unregister_views_dock()
{
	if (!registered)
		return;
	registered = false;
	obs_frontend_remove_event_callback(frontend_event, nullptr);
	obs_frontend_remove_save_callback(collection_data, nullptr);
	if (dock) {
		dock->shutdown();
		obs_frontend_remove_dock(kDockId);
	}
}

void show_views_controller(const char *controller_id)
{
	if (!qApp || !controller_id || !*controller_id)
		return;
	const QString id = QString::fromUtf8(controller_id);
	QMetaObject::invokeMethod(
		qApp,
		[id] {
			if (dock)
				dock->show_controller(id);
		},
		Qt::QueuedConnection);
}

} // namespace stagekit
