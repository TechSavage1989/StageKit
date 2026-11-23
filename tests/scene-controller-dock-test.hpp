#pragma once

#include "scene-controller-test.hpp"
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>

namespace scene_controller_dock_test {

using namespace scene_controller_test;

inline QDialog *dialog(const char *name)
{
	for (auto *widget : QApplication::allWidgets())
		if (widget->objectName() == name && widget->isVisible())
			if (auto *found = qobject_cast<QDialog *>(widget))
				return found;
	return nullptr;
}

inline bool click(QWidget *parent, const char *name)
{
	if (auto *button = parent ? parent->findChild<QAbstractButton *>(name) : nullptr) {
		if (!button->isEnabled())
			return false;
		button->click();
		QCoreApplication::processEvents();
		return true;
	}
	return false;
}

inline bool check_item(QListWidget *list, const QString &name)
{
	if (!list)
		return false;
	for (int i = 0; i < list->count(); ++i)
		if (list->item(i)->data(Qt::UserRole).toString() == name) {
			list->item(i)->setCheckState(Qt::Checked);
			return true;
		}
	return false;
}

inline bool click_row(QListWidget *list, const QString &id)
{
	if (!list)
		return false;
	for (int i = 0; i < list->count(); ++i) {
		auto *item = list->item(i);
		if (item->data(Qt::UserRole).toString() != id)
			continue;
		list->scrollToItem(item);
		const QPointF at = list->visualItemRect(item).center();
		QMouseEvent press(QEvent::MouseButtonPress, at, list->viewport()->mapToGlobal(at), Qt::LeftButton,
				  Qt::LeftButton, Qt::NoModifier);
		QMouseEvent release(QEvent::MouseButtonRelease, at, list->viewport()->mapToGlobal(at), Qt::LeftButton,
				    Qt::NoButton, Qt::NoModifier);
		QApplication::sendEvent(list->viewport(), &press);
		QApplication::sendEvent(list->viewport(), &release);
		return true;
	}
	return false;
}

inline OBSDataAutoRelease find_controller(const char *name)
{
	OBSDataAutoRelease all = request("ListSceneControllers", nullptr);
	OBSDataArrayAutoRelease controllers = obs_data_get_array(all, "controllers");
	for (size_t i = 0; i < obs_data_array_count(controllers); ++i) {
		OBSDataAutoRelease found = obs_data_array_item(controllers, i);
		if (std::strcmp(obs_data_get_string(found, "name"), name) == 0)
			return found;
	}
	return nullptr;
}

inline void check(QStringList &issues, bool restart)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit Views: ") + QString::fromUtf8(message));
	};
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = main ? main->findChild<QWidget *>("stagekitViewsDock") : nullptr;
	if (!panel) {
		expect(false, "dock was not registered");
		return;
	}
	auto *scenes = panel->findChild<QComboBox *>("viewsScene");
	auto *controllers = panel->findChild<QComboBox *>("viewsController");
	auto *scope = panel->findChild<QComboBox *>("viewsScope");
	auto *choices = panel->findChild<QListWidget *>("viewsList");
	if (!scenes || !controllers || !scope || !choices) {
		expect(false, "dock selectors were missing");
		return;
	}
	QDockWidget *floating = qobject_cast<QDockWidget *>(panel);
	for (auto *parent = panel->parentWidget(); !floating && parent; parent = parent->parentWidget())
		floating = qobject_cast<QDockWidget *>(parent);
	if (floating) {
		floating->setFloating(true);
		floating->resize(350, 450);
		floating->show();
	}
	constexpr const char *controller_name = "Stream views";
	if (restart) {
		OBSDataAutoRelease state = find_controller(controller_name);
		const QString id = QString::fromUtf8(obs_data_get_string(state, "controllerId"));
		expect(state && !id.isEmpty() && until([&] {
			       return controllers->currentData().toString() == id &&
				      scope->currentData().toString() == "views" && choices->count() == 3;
		       }),
		       "controller, scope or saved views were not restored after restarting OBS");
		OBSDataAutoRelease config = obs_data_get_obj(state, "config");
		OBSDataArrayAutoRelease groups = obs_data_get_array(config, "groups");
		expect(obs_data_array_count(groups) == 2, "edited independent groups did not survive restart");
		if (issues.size() == before)
			blog(LOG_INFO,
			     "[StageKit Views test] PASS: controller, group/view definitions and dock selection restored after OBS restart");
		return;
	}

	const bool studio = obs_frontend_preview_program_mode_active();
	OBSSourceAutoRelease old_program = obs_frontend_get_current_scene();
	OBSSourceAutoRelease old_preview = studio ? obs_frontend_get_current_preview_scene() : nullptr;
	struct Restore {
		bool studio;
		obs_source_t *program, *preview;
		~Restore()
		{
			obs_frontend_set_preview_program_mode(false);
			if (program)
				obs_frontend_set_current_scene(program);
			if (studio) {
				obs_frontend_set_preview_program_mode(true);
				if (preview)
					obs_frontend_set_current_preview_scene(preview);
			}
			obs_frontend_save();
		}
	} restore{studio, old_program, old_preview};
	obs_frontend_set_preview_program_mode(false);
	OBSSceneAutoRelease layout = obs_scene_create("Views demo layout");
	OBSSceneAutoRelease destination = obs_scene_create("Views demo second layout");
	auto *layout_source = obs_scene_get_source(layout);
	for (const char *name :
	     {"Views demo game", "Views demo browser", "Views demo camera", "Views demo close camera"}) {
		OBSSourceAutoRelease source =
			obs_source_create("stagekit_test_controller_item", name, nullptr, nullptr);
		obs_scene_add(layout, source);
	}
	const QString scene_id = QString::fromUtf8(obs_source_get_uuid(layout_source));
	expect(until([&] { return scenes->findData(scene_id) >= 0; }), "new scene did not reach the dock");
	scenes->setCurrentIndex(scenes->findData(scene_id));
	settle();
	OBSSourceAutoRelease after_browse = obs_frontend_get_current_scene();
	expect(after_browse.Get() == old_program.Get(), "browsing the dock changed Program");
	OBSDataAutoRelease create = obs_data_create();
	obs_data_set_string(create, "sceneUuid", scene_id.toUtf8().constData());
	obs_data_set_string(create, "name", controller_name);
	OBSDataAutoRelease created = request("CreateSceneController", create);
	const std::string id = obs_data_get_string(created, "controllerId");
	if (!obs_data_get_bool(created, "success") || id.empty()) {
		expect(false, "controller creation failed");
		return;
	}
	expect(until([&] { return controllers->findData(QString::fromStdString(id)) >= 0; }),
	       "new controller did not reach the dock");
	controllers->setCurrentIndex(controllers->findData(QString::fromStdString(id)));
	expect(click(panel, "viewsConfigure") && until([&] { return dialog("viewsEditor"); }),
	       "configuration editor did not open");
	QPointer<QDialog> editor = dialog("viewsEditor");
	if (!editor)
		return;
	auto *groups = editor->findChild<QListWidget *>("viewsGroups");
	auto *modes_list = editor->findChild<QListWidget *>("viewsModes");
	auto add_group_ui = [&](const char *name, const char *kind, const std::vector<QString> &members) {
		if (!click(editor, "viewsAddGroup") || !until([&] { return dialog("viewsGroupEditor"); }))
			return false;
		QPointer<QDialog> form = dialog("viewsGroupEditor");
		auto *title = form->findChild<QLineEdit *>("viewsGroupName");
		auto *type = form->findChild<QComboBox *>("viewsGroupKind");
		if (!title || !type)
			return false;
		title->setText(name);
		type->setCurrentIndex(type->findData(kind));
		for (const auto &member : members)
			if (!check_item(form->findChild<QListWidget *>("viewsGroupItems"), member))
				return false;
		return click(form, "viewsGroupSave") && until([&] { return !form || !form->isVisible(); });
	};
	auto add_mode_ui = [&](const char *name, const char *member) {
		if (!click(editor, "viewsAddMode") || !until([&] { return dialog("viewsModeEditor"); }))
			return false;
		QPointer<QDialog> form = dialog("viewsModeEditor");
		auto *title = form->findChild<QLineEdit *>("viewsModeName");
		auto *scene_switch = form->findChild<QCheckBox *>("viewsModeChangeScene");
		if (!title || !scene_switch || scene_switch->isChecked())
			return false;
		title->setText(name);
		return check_item(form->findChild<QListWidget *>("viewsModeItems"), member) &&
		       click(form, "viewsModeSave") && until([&] { return !form || !form->isVisible(); });
	};
	expect(add_group_ui("Main content", "modes", {}), "could not add a mode group from the editor");
	if (!groups || !modes_list || groups->count() != 1) {
		editor->reject();
		return;
	}
	groups->setCurrentRow(0);
	const QString content_id = groups->item(0)->data(Qt::UserRole).toString();
	expect(add_mode_ui("Game", "Views demo game") && add_mode_ui("Browser", "Views demo browser"),
	       "could not add the content modes from actual scene items");
	expect(modes_list->count() == 2, "mode list did not reflect the draft edits");
	expect(add_group_ui("Camera", "one", {"Views demo camera", "Views demo close camera"}),
	       "could not add independent source choices");
	if (groups->count() != 2) {
		editor->reject();
		return;
	}
	groups->setCurrentRow(1);
	expect(click(editor, "viewsGroupUp") && groups->item(1)->data(Qt::UserRole).toString() == content_id,
	       "group reordering lost its stable identity");
	groups->setCurrentRow(1);
	const QString capture = qEnvironmentVariable("STAGEKIT_VIEWS_CAPTURE");
	if (!capture.isEmpty()) {
		const QString path = QFileInfo(capture).dir().filePath("stagekit-views-editor.png");
		expect(editor->grab().save(path), "could not capture the real configuration editor");
	}
	auto *tabs = editor->findChild<QTabWidget *>("viewsEditorTabs");
	if (tabs)
		tabs->setCurrentIndex(1);
	auto add_view_ui = [&](const char *name, const char *content, const char *camera, bool change_scene) {
		if (!click(editor, "viewsAddView") || !until([&] { return dialog("viewsViewEditor"); }))
			return false;
		QPointer<QDialog> form = dialog("viewsViewEditor");
		auto *title = form->findChild<QLineEdit *>("viewsViewName");
		auto *table = form->findChild<QTableWidget *>("viewsViewGroups");
		auto *switch_scene = form->findChild<QCheckBox *>("viewsChangeScene");
		auto *target = form->findChild<QComboBox *>("viewsDestinationScene");
		if (!title || !table || !switch_scene || !target || switch_scene->isChecked())
			return false;
		title->setText(name);
		for (int row = 0; row < table->rowCount(); ++row) {
			auto *select = qobject_cast<QComboBox *>(table->cellWidget(row, 1));
			const QString desired = table->item(row, 0)->text() == "Main content" ? content : camera;
			if (!select || select->findText(desired) < 0)
				return false;
			select->setCurrentIndex(select->findText(desired));
		}
		if (change_scene) {
			switch_scene->setChecked(true);
			target->setCurrentIndex(target->findData("Views demo second layout"));
		}
		return click(form, "viewsViewSave") && until([&] { return !form || !form->isVisible(); });
	};
	expect(add_view_ui("Game and camera", "Game", "Views demo camera", false) &&
		       add_view_ui("Browser and close camera", "Browser", "Views demo close camera", false) &&
		       add_view_ui("Second layout", "Game", "Views demo camera", true),
	       "could not configure combined views or an explicit destination");
	expect(click(editor, "viewsEditorSave") && until([&] { return !editor || !editor->isVisible(); }),
	       "configuration did not save");
	if (editor && editor->isVisible()) {
		editor->reject();
		return;
	}
	expect(until([&] { return scope->findData("views") >= 0; }), "combined view selector was missing");
	scope->setCurrentIndex(scope->findData("views"));
	expect(until([&] { return choices->count() == 3; }), "dock did not show the saved views");
	OBSDataAutoRelease state = get(id);
	const std::string browser_mode = choice(id, content_id.toUtf8().constData(), "Browser");
	OBSDataAutoRelease config = obs_data_get_obj(state, "config");
	OBSDataAutoRelease content_group = row(config, "groups", content_id.toStdString());
	OBSDataAutoRelease content_settings = obs_data_get_obj(content_group, "settings");
	obs_data_set_string(content_settings, ("mode|" + browser_mode + "|go_to").c_str(), "Views demo second layout");
	expect(!browser_mode.empty() && configure(id, config), "could not configure a direct mode destination");
	settle();
	OBSDataArrayAutoRelease views = obs_data_get_array(state, "views");
	std::string game_view, browser_view, switch_view;
	for (size_t i = 0; i < obs_data_array_count(views); ++i) {
		OBSDataAutoRelease item = obs_data_array_item(views, i);
		const std::string name = obs_data_get_string(item, "name");
		if (name == "Game and camera")
			game_view = obs_data_get_string(item, "id");
		if (name == "Browser and close camera")
			browser_view = obs_data_get_string(item, "id");
		if (name == "Second layout")
			switch_view = obs_data_get_string(item, "id");
	}
	auto active = [&](const std::string &view) {
		OBSDataAutoRelease now = get(id);
		OBSDataAutoRelease value = row(now, "views", view);
		return obs_data_get_bool(value, "active");
	};
	expect(!browser_view.empty() && click_row(choices, QString::fromStdString(browser_view)) &&
		       until([&] { return active(browser_view); }),
	       "single-click did not activate a saved view");
	expect(until([&] {
		       for (int i = 0; i < choices->count(); ++i)
			       if (choices->item(i)->data(Qt::UserRole).toString() ==
				   QString::fromStdString(browser_view))
				       return choices->item(i)->data(Qt::UserRole + 1).toBool();
		       return false;
	       }),
	       "dock did not display the applied active state");
	OBSSourceAutoRelease after_select = obs_frontend_get_current_scene();
	expect(after_select.Get() == old_program.Get(), "ordinary view selection changed Program");
	if (!capture.isEmpty())
		expect(panel->grab().save(capture), "could not capture the real Views dock");
	expect(press(id, content_id.toUtf8().constData(), browser_mode.c_str()) && until([&] {
		       OBSSourceAutoRelease current_scene = obs_frontend_get_current_scene();
		       return current_scene.Get() == obs_scene_get_source(destination);
	       }),
	       "direct mode selection lost its configured scene destination");
	if (old_program) {
		obs_frontend_set_current_scene(old_program);
		until([&] {
			OBSSourceAutoRelease current_scene = obs_frontend_get_current_scene();
			return current_scene.Get() == old_program.Get();
		});
	}
	expect(!switch_view.empty() && click_row(choices, QString::fromStdString(switch_view)) && until([&] {
		OBSSourceAutoRelease current_scene = obs_frontend_get_current_scene();
		return current_scene.Get() == obs_scene_get_source(destination);
	}),
	       "explicit destination did not change the scene");
	if (old_program) {
		obs_frontend_set_current_scene(old_program);
		expect(until([&] {
			       OBSSourceAutoRelease current_scene = obs_frontend_get_current_scene();
			       return current_scene.Get() == old_program.Get();
		       }),
		       "could not prepare Studio Mode Program");
		obs_frontend_set_preview_program_mode(true);
		obs_frontend_set_current_preview_scene(layout_source);
		expect(click_row(choices, QString::fromStdString(switch_view)) && until([&] {
			       OBSSourceAutoRelease preview = obs_frontend_get_current_preview_scene();
			       OBSSourceAutoRelease program = obs_frontend_get_current_scene();
			       return preview.Get() == obs_scene_get_source(destination) &&
				      program.Get() == old_program.Get();
		       }),
		       "explicit Studio Mode destination did not use Preview while preserving Program");
		obs_frontend_set_preview_program_mode(false);
	}
	expect(!game_view.empty() && click_row(choices, QString::fromStdString(game_view)) &&
		       until([&] { return active(game_view); }),
	       "could not select the saved restart state");
	obs_frontend_save();
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[StageKit Views test] PASS: real group/mode/view editor, source picker, reorder, single-click state, same-scene default and explicit Program/Studio Preview destination; saved for restart");
}

} // namespace scene_controller_dock_test
