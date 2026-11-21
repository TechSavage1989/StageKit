#pragma once

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QWidgetAction>

namespace quick_test {
constexpr auto manual_title = "QA fixture pins";
constexpr auto renamed_title = "QA ordered sources";
constexpr auto dynamic_title = "QA current scene";
constexpr auto search_title = "QA collection search";
constexpr auto alpha = "QA Alpha card";
constexpr auto zulu = "QA Zulu card";
constexpr auto nested_name = "QA nested scene";

inline QMainWindow *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

template<class T> T *visible(const char *objectName)
{
	for (auto *widget : QApplication::allWidgets())
		if (widget->objectName() == objectName && widget->isVisible())
			if (auto *typed = qobject_cast<T *>(widget))
				return typed;
	return nullptr;
}

template<class F> bool wait_for(F condition)
{
	QElapsedTimer timer;
	timer.start();
	while (!condition() && timer.elapsed() < 3000) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
		QThread::msleep(10);
	}
	return condition();
}

inline QDockWidget *dock(const QString &name)
{
	for (auto *found : main_window()->findChildren<QDockWidget *>())
		if (found->windowTitle() == name)
			return found;
	return nullptr;
}

inline QDockWidget *create(QDialog *manager, const QString &name, const QString &mode)
{
	auto *open = manager->findChild<QPushButton *>("nativeCreateQuickDock");
	if (!open)
		return nullptr;
	open->click();
	auto *form = visible<QDialog>("nativeQuickOptions");
	if (!form)
		return nullptr;
	auto *title = form->findChild<QLineEdit *>("nativeQuickTitle");
	auto *types = form->findChild<QComboBox *>("nativeQuickType");
	auto *clickable = form->findChild<QCheckBox *>("quickClickable");
	auto *buttons = form->findChild<QDialogButtonBox *>();
	if (!title || !types || !clickable || !buttons || clickable->isChecked() ||
	    types->count() != 3 || types->currentData().toString() != "Manual" || types->findData(mode) < 0) {
		form->reject();
		return nullptr;
	}
	title->setText(name);
	types->setCurrentIndex(types->findData(mode));
	if (mode == "Dynamic")
		clickable->setChecked(true);
	buttons->button(QDialogButtonBox::Ok)->click();
	return dock(name);
}

inline QTreeWidgetItem *result(QTreeWidget *list, const QString &name, const QString &section = {})
{
	for (auto *row : list->findItems(name, Qt::MatchExactly | Qt::MatchRecursive, 0))
		if (!row->data(0, Qt::UserRole).toString().isEmpty() && (!row->parent() || row->parent()->isExpanded()) &&
		    (section.isEmpty() || (row->parent() && row->parent()->text(0) == section)))
			return row;
	return nullptr;
}

inline bool check_manual(QDockWidget *board, obs_sceneitem_t *nested, obs_sceneitem_t *direct)
{
	auto *list = board->findChild<QTreeWidget *>("nativeQuickSources");
	auto *pin = board->findChild<QToolButton *>("nativeQuickPin");
	auto *up = board->findChild<QToolButton *>("nativeQuickUp");
	if (!list || !pin || !up || board->findChild<QLineEdit *>("nativeQuickSearch"))
		return false;
	pin->click();
	auto *picker = visible<QDialog>("nativeQuickPicker");
	if (!picker)
		return false;
	auto *available = picker->findChild<QListWidget *>("nativeQuickAvailable");
	auto *include = picker->findChild<QToolButton *>("nativeQuickInclude");
	auto *commit = picker->findChild<QPushButton *>("nativeQuickAddSources");
	if (!available || !include || !commit)
		return false;
	for (const char *name : {alpha, zulu}) {
		auto rows = available->findItems(name, Qt::MatchExactly);
		if (rows.size() != 1)
			return false;
		rows.front()->setSelected(true);
	}
	include->click();
	commit->click();
	if (list->topLevelItemCount() != 2 || list->topLevelItem(1)->text(0) != zulu)
		return false;
	list->setCurrentItem(list->topLevelItem(1));
	up->click();
	if (list->topLevelItem(0)->text(0) != zulu || list->topLevelItem(1)->text(0) != alpha)
		return false;
	auto *row = list->topLevelItem(1);
	auto *parents = qobject_cast<QToolButton *>(list->itemWidget(row, 3));
	if (!parents || !list->itemWidget(row, 1) || !list->itemWidget(row, 2))
		return false;
	parents->click();
	auto *menu = visible<QMenu>("nativeQuickParentsMenu");
	if (!menu)
		return false;
	bool isolated = false;
	for (auto *action : menu->actions()) {
		auto *entry = qobject_cast<QWidgetAction *>(action);
		if (!entry || entry->data().toString() != nested_name)
			continue;
		auto *eye = entry->defaultWidget()->findChild<QCheckBox *>("nativeQuickVisibility");
		auto *transform = entry->defaultWidget()->findChild<QToolButton *>("nativeQuickTransform");
		if (eye && transform) {
			eye->click();
			isolated = !obs_sceneitem_visible(nested) && obs_sceneitem_visible(direct);
			eye->click();
		}
	}
	menu->close();
	if (!isolated || !obs_sceneitem_visible(nested))
		return false;
	list->customContextMenuRequested(list->visualItemRect(row).center());
	auto *context = visible<QMenu>("nativeQuickSourceMenu");
	if (!context)
		return false;
	QStringList commands;
	for (auto *command : context->actions())
		commands << command->text();
	context->close();
	return commands.contains("Add to Current Scene") && commands.contains("Add Clone to Current Scene") &&
	       commands.contains("Rename") && commands.contains("Filters") && commands.contains("Open Windowed Projector");
}

inline bool edit_manual(QDockWidget *board)
{
	auto *edit = board->findChild<QToolButton *>("nativeQuickSettings");
	if (!edit)
		return false;
	edit->click();
	auto *form = visible<QDialog>("nativeQuickOptions");
	if (!form)
		return false;
	auto *name = form->findChild<QLineEdit *>("nativeQuickTitle");
	auto *filters = form->findChild<QCheckBox *>("quickShowFilters");
	auto *ok = form->findChild<QDialogButtonBox *>();
	if (!name || !filters || !ok)
		return false;
	name->setText(renamed_title);
	filters->setChecked(false);
	ok->button(QDialogButtonBox::Ok)->click();
	auto *list = board->findChild<QTreeWidget *>("nativeQuickSources");
	return board->windowTitle() == renamed_title && list && list->isColumnHidden(2);
}

inline bool check_dynamic(QDockWidget *board, obs_scene_t *nested, obs_sceneitem_t *group, obs_source_t *other)
{
	auto *list = board->findChild<QTreeWidget *>("nativeQuickSources");
	if (!list || board->findChild<QLineEdit *>("nativeQuickSearch"))
		return false;
	auto *inside = obs_scene_add(obs_sceneitem_group_get_scene(group), other);
	if (!inside || !wait_for([&] { return result(list, obs_source_get_name(other)) != nullptr; }))
		return false;
	obs_sceneitem_remove(inside);
	if (!wait_for([&] { return !result(list, obs_source_get_name(other)); }))
		return false;
	OBSSourceAutoRelease before = obs_frontend_get_current_scene();
	if (!before || !result(list, nested_name) || !result(list, alpha) || !result(list, zulu))
		return false;
	const bool wasStudio = obs_frontend_preview_program_mode_active();
	obs_frontend_set_preview_program_mode(true);
	if (!wait_for([] { return obs_frontend_preview_program_mode_active(); }))
		return false;
	OBSSourceAutoRelease previousPreview = obs_frontend_get_current_preview_scene();
	if (auto *nestedRow = result(list, nested_name))
		list->itemClicked(nestedRow, 0);
	bool preview = wait_for([&] {
		OBSSourceAutoRelease active = obs_frontend_get_current_preview_scene();
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		return active.Get() == obs_scene_get_source(nested) && program.Get() == before.Get();
	});
	if (previousPreview)
		obs_frontend_set_current_preview_scene(previousPreview);
	obs_frontend_set_preview_program_mode(false);
	wait_for([] { return !obs_frontend_preview_program_mode_active(); });
	obs_frontend_set_current_scene(obs_scene_get_source(nested));
	bool changed = wait_for([&] { return result(list, alpha) && !result(list, zulu) && !result(list, nested_name); });
	obs_frontend_set_current_scene(before);
	bool restored = wait_for([&] { return result(list, zulu) && result(list, nested_name); });
	if (wasStudio)
		obs_frontend_set_preview_program_mode(true);
	return preview && changed && restored;
}

inline bool check_search(QWidget *board, obs_source_t *source)
{
	auto *edit = board->findChild<QLineEdit *>("nativeQuickSearch");
	auto *list = board->findChild<QTreeWidget *>("nativeQuickSources");
	if (!edit || !list || list->topLevelItemCount())
		return false;
	auto match = [&](const QString &term, const char *header) {
		edit->setText(term);
		return result(list, alpha, header) != nullptr;
	};
	bool passed = match("qa alpha", "Source") && match(obs_source_get_uuid(source), "Source") &&
		      match("image_source", "Source Type") && match("qa-filter-phrase", "Filters") &&
		      match("qa-file-phrase", "File Path");
	edit->setText("qa-url-phrase");
	passed = passed && result(list, "QA remote media", "URL");
	edit->setText("no-such-qa-result");
	return passed && list->topLevelItemCount() == 0;
}

inline bool check_popup(obs_source_t *source)
{
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
	obs_enum_hotkeys([](void *data, obs_hotkey_id id, obs_hotkey_t *key) {
		if (qstrcmp(obs_hotkey_get_name(key), "Quick Search") != 0)
			return true;
		*static_cast<obs_hotkey_id *>(data) = id;
		return false;
	}, &hotkey);
	if (hotkey == OBS_INVALID_HOTKEY_ID)
		return false;
	obs_hotkey_trigger_routed_callback(hotkey, true);
	obs_hotkey_trigger_routed_callback(hotkey, false);
	if (!wait_for([] { return visible<QDialog>("nativeQuickPopup") != nullptr; }))
		return false;
	auto *popup = visible<QDialog>("nativeQuickPopup");
	const bool search = check_search(popup, source);
	obs_hotkey_trigger_routed_callback(hotkey, true);
	obs_hotkey_trigger_routed_callback(hotkey, false);
	const bool closed = wait_for([] { return !visible<QDialog>("nativeQuickPopup"); });
	if (!search || !closed)
		return false;
	obs_hotkey_trigger_routed_callback(hotkey, true);
	obs_hotkey_trigger_routed_callback(hotkey, false);
	if (!wait_for([] { return visible<QDialog>("nativeQuickPopup") != nullptr; }))
		return false;
	popup = visible<QDialog>("nativeQuickPopup");
	auto *edit = popup->findChild<QLineEdit *>("nativeQuickSearch");
	auto *list = popup->findChild<QTreeWidget *>("nativeQuickSources");
	if (!edit || !list)
		return false;
	edit->setText("QA group child");
	auto *row = result(list, "QA group child", "Source");
	if (!row)
		return false;
	list->customContextMenuRequested(list->visualItemRect(row).center());
	auto *menu = visible<QMenu>("nativeQuickSourceMenu");
	if (!menu)
		return false;
	OBSSourceAutoRelease destination = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
										     : obs_frontend_get_current_scene();
	for (auto *action : menu->actions())
		if (action->text() == "Add to Current Scene") {
			action->trigger();
			break;
		}
	auto *scene = obs_scene_from_source(destination);
	return scene && obs_scene_find_source(scene, "QA group child") && !visible<QDialog>("nativeQuickPopup");
}

inline void check_restart(QStringList &issues)
{
	auto *manual = dock(renamed_title);
	auto *list = manual ? manual->findChild<QTreeWidget *>("nativeQuickSources") : nullptr;
	if (!list || list->topLevelItemCount() != 2 || list->topLevelItem(0)->text(0) != zulu ||
	    list->topLevelItem(1)->text(0) != alpha || !list->isColumnHidden(2) || !dock(dynamic_title) || !dock(search_title))
		issues.append("Quick Access lost a dock, its options or the manual source order after restarting OBS");
	else
		blog(LOG_INFO, "[Quick Access test] Restored all three docks, row options and manual order on restart");
}

inline void check(QStringList &issues, QAction *toolsAction)
{
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	auto *scene = obs_scene_from_source(current);
	if (!toolsAction || !scene) {
		issues.append("Quick Access Tools entry or fixture scene unavailable");
		return;
	}
	OBSDataAutoRelease options = obs_data_create();
	const auto path = QFileInfo(qEnvironmentVariable("STAGEKIT_TEST_REPORT")).dir().filePath("qa-file-phrase.png");
	QImage image(4, 4, QImage::Format_RGB32);
	image.fill(Qt::green);
	if (!image.save(path)) {
		issues.append("Could not write the Quick Access image fixture");
		return;
	}
	obs_data_set_string(options, "file", path.toUtf8().constData());
	OBSSourceAutoRelease first = obs_source_create("image_source", alpha, options, nullptr);
	OBSSourceAutoRelease second = obs_source_create("image_source", zulu, nullptr, nullptr);
	OBSSourceAutoRelease third = obs_source_create("image_source", "QA group child", nullptr, nullptr);
	OBSDataAutoRelease mediaSettings = obs_data_create();
	QUrl url = QUrl::fromLocalFile(path);
	url.setFragment("qa-url-phrase");
	obs_data_set_bool(mediaSettings, "is_local_file", false);
	obs_data_set_string(mediaSettings, "input", url.toEncoded().constData());
	OBSSourceAutoRelease media = obs_source_create("ffmpeg_source", "QA remote media", mediaSettings, nullptr);
	OBSSceneAutoRelease nested = obs_scene_create(nested_name);
	OBSSourceAutoRelease filter = obs_source_create_private("stagekit_gaussian_blur", "QA-filter-phrase", nullptr);
	if (!first || !second || !third || !media || !nested || !filter) {
		issues.append("Quick Access could not create its test sources");
		return;
	}
	obs_source_filter_add(first, filter);
	auto *direct = obs_scene_add(scene, first);
	auto *nestedItem = obs_scene_add(nested, first);
	auto *group = obs_scene_add_group(scene, "QA source group");
	obs_scene_add(scene, second);
	obs_scene_add(scene, obs_scene_get_source(nested));
	toolsAction->trigger();
	auto *manager = visible<QDialog>("nativeQuickManager");
	if (!manager || !direct || !nestedItem || !group) {
		issues.append("Quick Access could not open its dock manager or build its scenes");
		return;
	}
	auto *manual = create(manager, manual_title, "Manual");
	auto *dynamic = create(manager, dynamic_title, "Dynamic");
	auto *search = create(manager, search_title, "Source Search");
	manager->close();
	if (!manual || !check_manual(manual, nestedItem, direct) || !edit_manual(manual))
		issues.append("Quick Access manual picker, order, row actions, parent controls or dock editing failed");
	else
		blog(LOG_INFO, "[Quick Access test] Manual multi-select, ordering, row menus, parent visibility and dock editing passed");
	if (!dynamic || !check_dynamic(dynamic, nested, group, third))
		issues.append("Quick Access dynamic dock missed a nested source, group edit or scene or changed Program on a Preview click");
	else
		blog(LOG_INFO, "[Quick Access test] Dynamic followed Program, nested scenes and group edits; clickable scenes selected Preview");
	if (!search || !check_search(search, first))
		issues.append("Quick Access dock did not group source, UUID, type, file and filter matches or clear empty results");
	else
		blog(LOG_INFO, "[Quick Access test] Source Search matched name, UUID, type, file, URL and filter");
	if (!check_popup(first))
		issues.append("Quick Search hotkey did not open, search and dismiss a single popup");
	else
		blog(LOG_INFO, "[Quick Access test] The Quick Search hotkey opened, searched and dismissed the popup");
	obs_frontend_save();
}
} // namespace quick_test
