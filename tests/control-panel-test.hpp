#pragma once

#include "scene-controller-dock-test.hpp"
#include <QGridLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QScrollArea>
#include <QVariantMap>
#include <util/bmem.h>

// Exercise the public dock and normal OBS objects. This probe deliberately
// does not link the native catalog or call the panel's private implementation.
namespace control_panel_test {

using namespace scene_controller_dock_test;

inline QString uuid(obs_source_t *source)
{
	return QString::fromUtf8(source ? obs_source_get_uuid(source) : "");
}

inline QJsonObject json(obs_data_t *data)
{
	return QJsonDocument::fromJson(data ? obs_data_get_json(data) : "{}").object();
}

inline QVariantMap target(QAbstractButton *button)
{
	return button ? button->property("viewsControlTarget").toMap() : QVariantMap{};
}

inline bool matches(const QVariantMap &value, const QVariantMap &wanted)
{
	for (auto it = wanted.cbegin(); it != wanted.cend(); ++it)
		if (value.value(it.key()) != it.value())
			return false;
	return true;
}

inline QAbstractButton *button(QWidget *panel, const QVariantMap &wanted)
{
	for (auto *candidate : panel->findChildren<QAbstractButton *>())
		if (candidate->property("viewsControlId").isValid() && matches(target(candidate), wanted))
			return candidate;
	return nullptr;
}

inline QAbstractButton *button_id(QWidget *panel, const QString &id)
{
	return button(panel, {{"id", id}});
}

inline QString control_id(QWidget *panel, const QVariantMap &wanted)
{
	const auto *found = button(panel, wanted);
	return found ? found->property("viewsControlId").toString() : QString();
}

inline bool checked(QWidget *panel, const QString &id)
{
	const auto *found = button_id(panel, id);
	return found && found->isChecked();
}

inline bool panel_ready(QWidget *panel)
{
	const auto *edit = panel->findChild<QAbstractButton *>("viewsPanelEdit");
	return edit && edit->isEnabled();
}

inline bool buttons_fit_sections(QWidget *panel, QString *detail = nullptr)
{
	bool found = false;
	for (auto *candidate : panel->findChildren<QAbstractButton *>()) {
		if (!candidate->property("viewsControlId").isValid())
			continue;
		found = true;
		const auto *section = candidate->parentWidget();
		const int wrapped_height = candidate->heightForWidth(candidate->width());
		if (!section || !section->rect().contains(candidate->geometry()) ||
		    candidate->maximumHeight() < wrapped_height) {
			if (detail) {
				auto rect = [](const QRect &value) {
					return QString("(%1,%2 %3x%4)")
						.arg(value.x())
						.arg(value.y())
						.arg(value.width())
						.arg(value.height());
				};
				auto size = [](const QSize &value) {
					return QString("%1x%2").arg(value.width()).arg(value.height());
				};
				const auto policy = candidate->sizePolicy();
				*detail =
					QString("%1: button=%2 section=%3 hfw=%4 hint=%5 minHint=%6 minSize=%7 maxSize=%8 "
						"policy=%9/%10 widgetRect=%11")
						.arg(candidate->text(), rect(candidate->geometry()),
						     section ? rect(section->rect()) : "missing")
						.arg(wrapped_height)
						.arg(size(candidate->sizeHint()), size(candidate->minimumSizeHint()),
						     size(candidate->minimumSize()), size(candidate->maximumSize()))
						.arg(int(policy.horizontalPolicy()))
						.arg(int(policy.verticalPolicy()))
						.arg(candidate->testAttribute(Qt::WA_LayoutUsesWidgetRect));
				auto layout_detail = [&](const QLayout *layout) {
					if (!layout)
						return QStringLiteral("missing");
					const auto margins = layout->contentsMargins();
					return QString("rect=%1 hint=%2 min=%3 max=%4 hfw=%5 spacing=%6 margins=%7,%8,%9,%10")
						.arg(rect(layout->geometry()), size(layout->sizeHint()),
						     size(layout->minimumSize()), size(layout->maximumSize()))
						.arg(layout->heightForWidth(layout->geometry().width()))
						.arg(layout->spacing())
						.arg(margins.left())
						.arg(margins.top())
						.arg(margins.right())
						.arg(margins.bottom());
				};
				if (section) {
					*detail += "; layout={" + layout_detail(section->layout()) + "}; grid={" +
						   layout_detail(section->findChild<QGridLayout *>()) + "}";
					if (const auto *label =
						    section->findChild<QLabel *>("viewsPanelSectionHeading"))
						*detail +=
							QString("; heading={rect=%1 hint=%2 minHint=%3 min=%4 max=%5 hfw=%6}")
								.arg(rect(label->geometry()), size(label->sizeHint()),
								     size(label->minimumSizeHint()),
								     size(label->minimumSize()),
								     size(label->maximumSize()))
								.arg(label->heightForWidth(label->width()));
				}
			}
			return false;
		}
	}
	if (!found && detail)
		*detail = "No control buttons were present";
	return found;
}

inline void check_geometry(QWidget *panel, QStringList &issues, const char *message)
{
	if (until([&] { return buttons_fit_sections(panel); }))
		return;
	QString detail;
	buttons_fit_sections(panel, &detail);
	issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message) + "; " + detail);
}

inline bool press_button(QWidget *panel, const QString &id)
{
	auto *found = button_id(panel, id);
	if (!found || !found->isEnabled() || !found->isVisible())
		return false;
	found->click();
	QCoreApplication::processEvents();
	return true;
}

inline int pin_row(QListWidget *pins, const QString &id)
{
	for (int i = 0; pins && i < pins->count(); ++i)
		if (pins->item(i)->data(Qt::UserRole).toString() == id)
			return i;
	return -1;
}

inline QJsonObject resolved(obs_source_t *source)
{
	OBSDataAutoRelease settings = obs_source_get_settings(source);
	OBSDataAutoRelease defaults = obs_data_get_defaults(settings);
	obs_data_apply(defaults, settings);
	return json(defaults);
}

inline QString collection_name()
{
	char *raw = obs_frontend_get_current_scene_collection();
	const QString name = QString::fromUtf8(raw ? raw : "");
	bfree(raw);
	return name;
}

inline QStringList saved_labels()
{
	return {"Live game", "Studio close-up",      "Side view",        "Camera group",
		"Watermark", "Studio title renamed", "Group visibility", "Spare (saved)"};
}

inline bool check_saved_panel(QWidget *panel)
{
	if (!click(panel, "viewsPanelEdit") || !until([&] { return dialog("viewsPanelEditor"); }))
		return false;
	QPointer<QDialog> editor = dialog("viewsPanelEditor");
	auto *pins = editor->findChild<QListWidget *>("viewsPanelPins");
	const auto names = saved_labels();
	bool valid = pins && pins->count() == names.size();
	for (int i = 0; valid && i < pins->count(); ++i) {
		const auto pin = pins->item(i)->data(Qt::UserRole + 1).toMap();
		const QString id = pin.value("id").toString();
		auto *found = button_id(panel, id);
		valid = found && found->text() == names[i] &&
			pin.value("section").toString() == (i < 3 ? "Quick choices" : "Visibility") &&
			target(found).value("id").toString() == id;
		if (valid && i == names.size() - 1) {
			OBSSourceAutoRelease replacement = obs_get_source_by_name("Studio spare");
			valid = !found->isEnabled() && replacement &&
				target(found).value("sourceUuid").toString() != uuid(replacement);
		}
	}
	editor->reject();
	until([&] { return !editor || !editor->isVisible(); });
	return valid;
}

inline void check_collection_isolation(QWidget *panel, QStringList &issues)
{
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	const QString original = collection_name();
	if (original.isEmpty()) {
		expect(false, "could not identify the current scene collection");
		return;
	}
	// An unsaved draft must not follow the user to another collection or be
	// accidentally committed by the outgoing collection's save callback.
	expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
	       "could not open a draft before changing collections");
	QPointer<QDialog> draft = dialog("viewsPanelEditor");
	if (draft)
		if (auto *label = draft->findChild<QLineEdit *>("viewsPanelLabel"))
			label->setText("Uncommitted draft");
	const QString temporary = "Panel empty collection";
	const bool created = obs_frontend_add_scene_collection(temporary.toUtf8().constData());
	expect(created && until([&] { return collection_name() == temporary; }),
	       "could not open the second scene collection");
	if (!created)
		return;
	expect(until([&] {
		       return (!draft || !draft->isVisible()) &&
			      panel->findChild<QWidget *>("viewsOperatorBoard")->isVisible() && !button(panel, {}) &&
			      panel_ready(panel);
	       }),
	       "collection change retained a draft, management page or another collection's pins");
	if (click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); })) {
		QPointer<QDialog> editor = dialog("viewsPanelEditor");
		expect(click(editor, "viewsPanelSave") && until([&] { return !editor || !editor->isVisible(); }),
		       "could not save an intentionally empty panel");
	} else {
		expect(false, "empty panel editor did not open");
	}
	{
		OBSSourceAutoRelease scene = obs_frontend_get_current_scene();
		OBSSourceAutoRelease source =
			obs_source_create("stagekit_test_controller_item", "Empty collection sample", nullptr, nullptr);
		if (scene && obs_scene_from_source(scene))
			obs_scene_add(obs_scene_from_source(scene), source);
		OBSSourceAutoRelease filter =
			obs_source_create("stagekit_show_in_scenes", "Empty collection visibility", nullptr, nullptr);
		obs_source_filter_add(source, filter);
		// Opening Edit panel forces a fresh inventory. The new control must be
		// available there while the deliberately empty operator board stays empty.
		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "empty panel could not be edited after adding a control");
		QPointer<QDialog> editor = dialog("viewsPanelEditor");
		if (editor) {
			auto *available = editor->findChild<QListWidget *>("viewsPanelAvailable");
			expect(available && available->count() > 0,
			       "panel editor opened before the new collection's control inventory was available");
			expect(!button(panel, {}), "automatic discovery replaced an intentionally empty panel");
			editor->reject();
		}
	}
	obs_frontend_save();
	obs_frontend_set_current_scene_collection(original.toUtf8().constData());
	expect(until([&] { return collection_name() == original && panel_ready(panel) && button(panel, {}); }) &&
		       check_saved_panel(panel),
	       "original pins, labels or order did not return after switching collections");
	obs_frontend_set_current_scene_collection(temporary.toUtf8().constData());
	expect(until([&] { return collection_name() == temporary && panel_ready(panel) && !button(panel, {}); }),
	       "the second collection did not retain its intentionally empty panel");
	obs_frontend_set_current_scene_collection(original.toUtf8().constData());
	expect(until([&] { return collection_name() == original && panel_ready(panel) && button(panel, {}); }) &&
		       check_saved_panel(panel),
	       "collection round trip changed the saved panel");
	obs_frontend_save();
}

inline void check(QStringList &issues, bool restart)
{
	const auto before = issues.size();
	auto expect = [&](bool value, const char *message) {
		if (!value)
			issues.append(QStringLiteral("StageKit controls: ") + QString::fromUtf8(message));
	};
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	auto *panel = main ? main->findChild<QWidget *>("stagekitViewsDock") : nullptr;
	if (!panel || !click(panel, "viewsBackToControls")) {
		expect(false, "operator panel was not available from controller management");
		return;
	}
	auto *board = panel->findChild<QWidget *>("viewsOperatorBoard");
	expect(board && board->isVisible(), "operator panel was not visible");
	if (restart) {
		expect(until([&] { return panel_ready(panel) && button(panel, {}); }) && check_saved_panel(panel),
		       "saved pins, target identities, labels, sections or order did not survive restart");
		if (issues.size() == before)
			blog(LOG_INFO,
			     "[StageKit controls test] PASS: customized cross-scene panel, renamed source and disabled missing target restored after OBS restart");
		return;
	}

	// Keep every native reference inside this scope so the later real scene
	// collection switch can release the old collection normally.
	{
		OBSSourceAutoRelease program = obs_frontend_get_current_scene();
		auto *scenes = panel->findChild<QComboBox *>("viewsScene");
		auto *controllers = panel->findChild<QComboBox *>("viewsController");
		auto *scope = panel->findChild<QComboBox *>("viewsScope");
		if (!scenes || !controllers || !scope)
			return;
		const auto old_scene = scenes->currentData(), old_controller = controllers->currentData(),
			   old_scope = scope->currentData();
		OBSDataAutoRelease existing = find_controller("Stream views");
		const QString first_id = QString::fromUtf8(obs_data_get_string(existing, "controllerId"));
		OBSDataArrayAutoRelease old_views = obs_data_get_array(existing, "views");
		QString first_view;
		for (size_t i = 0; i < obs_data_array_count(old_views); ++i) {
			OBSDataAutoRelease view = obs_data_array_item(old_views, i);
			if (std::strcmp(obs_data_get_string(view, "name"), "Game and camera") == 0)
				first_view = QString::fromUtf8(obs_data_get_string(view, "id"));
		}
		OBSSceneAutoRelease studio = obs_scene_create("Control panel studio");
		auto *scene = obs_scene_get_source(studio);
		auto add_item = [&](const char *name) {
			OBSSourceAutoRelease source =
				obs_source_create("stagekit_test_controller_item", name, nullptr, nullptr);
			return obs_scene_add(studio, source);
		};
		auto *main_camera = add_item("Studio main camera");
		auto *detail_camera = add_item("Studio detail camera");
		auto *legacy_main = add_item("Studio front view");
		auto *legacy_detail = add_item("Studio side view");
		auto *nested = obs_scene_add_group(studio, "Studio camera group");
		auto *nested_camera = add_item("Studio grouped camera");
		obs_sceneitem_group_add_item(nested, nested_camera);
		auto *wide_camera = add_item("Studio wide camera");
		auto *watermark = add_item("Studio watermark");
		auto *title = add_item("Studio title");
		auto *spare = add_item("Studio spare");
		add_item("Studio overlay note");
		add_item("Studio overlay badge");
		expect(click(panel, "viewsManageControllers") &&
			       until([&] { return scenes->findData(uuid(scene)) >= 0; }),
		       "new scene was not available for controller setup");
		scenes->setCurrentIndex(scenes->findData(uuid(scene)));
		expect(click(panel, "viewsCreate") && until([&] { return dialog("viewsCreateDialog"); }),
		       "Add controller did not open its name dialog");
		QPointer<QDialog> naming = dialog("viewsCreateDialog");
		if (!naming)
			return;
		auto *controller_name = naming->findChild<QLineEdit *>("viewsControllerName");
		if (controller_name)
			controller_name->setText("Studio controls");
		expect(controller_name && click(naming, "viewsControllerNameSave") &&
			       until([&] { return !naming || !naming->isVisible(); }) && !dialog("viewsEditor"),
		       "Add controller did not leave Import available directly in management");
		OBSDataAutoRelease created = find_controller("Studio controls");
		const std::string id = obs_data_get_string(created, "controllerId");
		expect(!id.empty() && controllers->currentData().toString() == QString::fromStdString(id),
		       "newly added controller was not selected");
		expect(click(panel, "viewsBackToControls"), "could not return to the operator panel after setup");
		OBSDataAutoRelease config = obs_data_create();
		obs_data_set_int(config, "schema", 1);
		OBSDataArrayAutoRelease groups = obs_data_array_create();
		OBSDataAutoRelease camera_settings = modes("Studio main camera", "Studio detail camera");
		add_group(groups, "camera", "Studio camera", "modes", camera_settings);
		obs_data_set_array(config, "groups", groups);
		expect(!id.empty() && configure(id, config), "could not configure the second scene controller");
		OBSDataAutoRelease legacy_settings = modes("Studio front view", "Studio side view");
		obs_data_set_int(legacy_settings, "mode|detail|place_ms", 725);
		obs_data_set_string(legacy_settings, "for|Studio side view|hide_transition", "fade_transition");
		obs_data_set_int(legacy_settings, "for|Studio side view|hide_ms", 110);
		OBSSourceAutoRelease legacy =
			obs_source_create("stagekit_scene_modes", "Studio angles", legacy_settings, nullptr);
		obs_source_filter_add(scene, legacy);
		OBSDataAutoRelease link_settings = obs_data_create();
		obs_data_set_int(link_settings, "mode", 0);
		list(link_settings, "items", {"Studio camera group", "Studio wide camera"});
		obs_data_set_string(link_settings, "transition", "none");
		OBSSourceAutoRelease link = obs_source_create("stagekit_visibility_link", "Studio camera selection",
							      link_settings, nullptr);
		obs_source_filter_add(scene, link);
		OBSDataAutoRelease disabled_settings = obs_data_create();
		obs_data_set_int(disabled_settings, "mode", 1);
		list(disabled_settings, "items", {"Studio overlay note", "Studio overlay badge"});
		obs_data_set_int(disabled_settings, "transition_ms", 312);
		OBSSourceAutoRelease disabled =
			obs_source_create("stagekit_visibility_link", "Studio overlays", disabled_settings, nullptr);
		obs_source_filter_add(scene, disabled);
		obs_source_set_enabled(disabled, false);
		OBSDataAutoRelease show_settings = obs_data_create();
		list(show_settings, "sources", {"Studio watermark"});
		OBSSourceAutoRelease show =
			obs_source_create("stagekit_show_in_scenes", "Watermark visibility", show_settings, nullptr);
		obs_source_filter_add(obs_sceneitem_get_source(watermark), show);

		const QVariantMap first_target{{"kind", "controller"},
					       {"controllerId", first_id},
					       {"viewId", first_view}};
		const QVariantMap second_target{{"kind", "controller"},
						{"controllerId", QString::fromStdString(id)},
						{"groupId", "camera"},
						{"choiceId", "detail"}};
		const QVariantMap mode_target{{"kind", "mode"}, {"filterUuid", uuid(legacy)}, {"modeId", "detail"}};
		const QVariantMap link_target{{"kind", "link"},
					      {"filterUuid", uuid(link)},
					      {"sourceUuid", uuid(obs_sceneitem_get_source(nested))}};
		const QVariantMap show_target{{"kind", "show-in-scenes"}, {"filterUuid", uuid(show)}};
		expect(!first_id.isEmpty() && !first_view.isEmpty() && until([&] {
			return button(panel, first_target) && button(panel, second_target) &&
			       button(panel, mode_target) && button(panel, link_target) && button(panel, show_target);
		}),
		       "automatic panel did not show both controller scenes and standalone controls together");
		const QString first = control_id(panel, first_target), second = control_id(panel, second_target),
			      mode = control_id(panel, mode_target), linked = control_id(panel, link_target),
			      shown = control_id(panel, show_target);
		if (first.isEmpty() || second.isEmpty() || mode.isEmpty() || linked.isEmpty() || shown.isEmpty())
			return;
		expect(!first.isEmpty() && press_button(panel, first),
		       "combined view could not be pressed from the panel");
		expect(press_button(panel, second) && until([&] {
			       return current(id, "camera") == "detail" && obs_sceneitem_visible(detail_camera) &&
				      !obs_sceneitem_visible(main_camera) && checked(panel, second);
		       }),
		       "a second scene's mode was not applied from the same panel");
		expect(press_button(panel, mode) && until([&] {
			       return obs_sceneitem_visible(legacy_detail) && !obs_sceneitem_visible(legacy_main) &&
				      checked(panel, mode);
		       }),
		       "standalone Scene Modes did not apply through the panel");
		expect(press_button(panel, linked) && until([&] {
			       return obs_sceneitem_visible(nested) && !obs_sceneitem_visible(wide_camera) &&
				      checked(panel, linked);
		       }),
		       "Link Visibility could not choose an OBS group from the panel");
		QPointer<QAbstractButton> unchanged = button_id(panel, linked);
		obs_sceneitem_set_visible(wide_camera, true);
		expect(until([&] {
			       return !obs_sceneitem_visible(nested) && obs_sceneitem_visible(wide_camera) &&
				      unchanged && unchanged == button_id(panel, linked) && !unchanged->isChecked();
		       }),
		       "external visibility changes did not update state without rebuilding the button");
		obs_source_set_enabled(show, false);
		expect(until([&] { return button_id(panel, shown) && !button_id(panel, shown)->isChecked(); }) &&
			       press_button(panel, shown) &&
			       until([&] { return obs_source_enabled(show) && checked(panel, shown); }),
		       "Show in Scenes did not display external state or toggle its native filter");

		// The remainder of this fixture customizes and saves these same controls,
		// then imports their originals without changing their pinned identities.
		QStringList ids{first, second, mode, linked, shown};
		QPointer<QDialog> editor;
		expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
		       "panel editor did not open");
		editor = dialog("viewsPanelEditor");
		if (!editor)
			return;
		auto *pins = editor->findChild<QListWidget *>("viewsPanelPins");
		auto *available = editor->findChild<QListWidget *>("viewsPanelAvailable");
		if (!pins || !available) {
			expect(false, "panel editor lists were missing");
			editor->reject();
			return;
		}
		while (pins->count()) {
			pins->setCurrentRow(0);
			click(editor, "viewsPanelRemove");
		}
		for (obs_sceneitem_t *item : {title, nested, spare}) {
			const QVariantMap wanted{{"kind", "source"},
						 {"sourceUuid", uuid(obs_sceneitem_get_source(item))}};
			for (int i = 0; i < available->count(); ++i) {
				const auto control = available->item(i)->data(Qt::UserRole + 1).toMap();
				if (matches(control.value("target").toMap(), wanted)) {
					ids.append(available->item(i)->data(Qt::UserRole).toString());
					break;
				}
			}
		}
		expect(ids.size() == 8 && !ids.contains(QString()),
		       "source and OBS group controls were not available to pin");
		for (int i = 0; i < available->count(); ++i)
			available->item(i)->setSelected(
				ids.contains(available->item(i)->data(Qt::UserRole).toString()));
		expect(available->selectedItems().size() == ids.size() && click(editor, "viewsPanelAdd") &&
			       pins->count() == ids.size(),
		       "adding several selected controls at once did not preserve the complete selection");
		auto *label = editor->findChild<QLineEdit *>("viewsPanelLabel");
		auto *section = editor->findChild<QLineEdit *>("viewsPanelSection");
		const auto names = saved_labels();
		for (int i = 0; label && section && i < ids.size(); ++i) {
			int from = pin_row(pins, ids[i]);
			pins->setCurrentRow(from);
			while (from > i) {
				click(editor, "viewsPanelUp");
				--from;
			}
			label->setText(i == 5 ? QString() : names[i]);
			section->setText(i < 3 ? "Quick choices" : "Visibility");
		}
		expect(label && section && click(editor, "viewsPanelSave") &&
			       until([&] { return !editor || !editor->isVisible(); }),
		       "panel customization was not saved");
		if (ids.size() != 8)
			return;
		const QString title_id = ids[5], group_id = ids[6], spare_id = ids[7];
		obs_source_set_name(obs_sceneitem_get_source(title), "Studio title renamed");
		expect(until([&] {
			       auto *renamed = button_id(panel, title_id);
			       return renamed && renamed->text() == "Studio title renamed" &&
				      button_id(panel, second) && button_id(panel, second)->text() == "Studio close-up";
		       }),
		       "source rename changed its identity or replaced a custom label");
		obs_source_set_enabled(link, false);
		expect(press_button(panel, group_id) && until([&] { return obs_sceneitem_visible(nested); }) &&
			       press_button(panel, group_id) && until([&] { return !obs_sceneitem_visible(nested); }),
		       "ordinary OBS group visibility could not be toggled from a pin");
		obs_source_set_enabled(link, true);
		{
			OBSSourceAutoRelease removed = obs_source_get_ref(obs_sceneitem_get_source(spare));
			obs_sceneitem_remove(spare);
			obs_source_remove(removed);
		}
		add_item("Studio spare");
		expect(until([&] {
			       auto *missing = button_id(panel, spare_id);
			       return missing && !missing->isEnabled() && missing->text() == "Spare (saved)";
		       }),
		       "missing pin was removed or rebound to a same-named replacement");

		// One bad supported filter rejects the whole batch. Removing it then
		// imports all three valid filters, including the disabled original.
		OBSDataAutoRelease invalid_settings = obs_data_create();
		obs_data_set_int(invalid_settings, "mode", 33);
		list(invalid_settings, "items", {"Studio overlay note"});
		OBSSourceAutoRelease invalid = obs_source_create("stagekit_visibility_link", "Invalid import fixture",
								 invalid_settings, nullptr);
		obs_source_filter_add(scene, invalid);
		const auto before_modes = resolved(legacy), before_link = resolved(link),
			   before_disabled = resolved(disabled);
		expect(click(panel, "viewsManageControllers"), "could not open management for bulk import");
		expect(until([&] { return scenes->findData(uuid(scene)) >= 0; }), "import scene was not listed");
		scenes->setCurrentIndex(scenes->findData(uuid(scene)));
		expect(until([&] { return controllers->findData(QString::fromStdString(id)) >= 0; }),
		       "import controller was not listed");
		controllers->setCurrentIndex(controllers->findData(QString::fromStdString(id)));
		expect(click(panel, "viewsImportLegacy") && until([&] { return dialog("viewsImportDialog"); }),
		       "existing scene controls import did not open");
		QPointer<QDialog> importing = dialog("viewsImportDialog");
		if (importing) {
			auto *ownership = importing->findChild<QCheckBox *>("viewsImportActivate");
			auto *result = importing->findChild<QLabel *>("viewsImportError");
			auto group_count = [&] {
				OBSDataAutoRelease state = get(id);
				OBSDataAutoRelease definition = obs_data_get_obj(state, "config");
				OBSDataArrayAutoRelease list = obs_data_get_array(definition, "groups");
				return obs_data_array_count(list);
			};
			expect(ownership && ownership->isChecked() && click(importing, "viewsImportAll") && result &&
				       !result->text().isEmpty() && group_count() == 1 && !imported_group(id, legacy) &&
				       !imported_group(id, link) && resolved(legacy) == before_modes &&
				       resolved(link) == before_link && resolved(disabled) == before_disabled &&
				       obs_source_enabled(legacy) && obs_source_enabled(link) &&
				       !obs_source_enabled(disabled),
			       "invalid Import all batch changed settings, ownership or part of the controller");
			obs_source_filter_remove(scene, invalid);
			const size_t filter_count = obs_source_filter_count(scene);
			expect(click(importing, "viewsImportAll") && until([&] { return group_count() == 4; }) &&
				       result &&
				       result->text() == "Imported 3 controls. Skipped 0 already imported controls.",
			       "Import all did not import all supported controls with clear counts");
			for (const auto &[source, saved] :
			     std::vector<std::pair<obs_source_t *, QJsonObject>>{{legacy, before_modes},
										 {link, before_link},
										 {disabled, before_disabled}}) {
				OBSDataAutoRelease group = imported_group(id, source);
				OBSDataAutoRelease origin = obs_data_get_obj(group, "origin");
				OBSDataAutoRelease original_settings = obs_data_get_obj(origin, "settings");
				expect(group && json(original_settings) == saved &&
					       obs_data_get_bool(origin, "activate") &&
					       obs_data_get_bool(origin, "enabled") == (source != disabled.Get()) &&
					       obs_data_get_bool(group, "enabled") == (source != disabled.Get()) &&
					       obs_source_enabled(source) == (source != disabled.Get()),
				       "bulk import lost complete original settings or enabled state");
			}
			expect(obs_source_filter_count(scene) == filter_count,
			       "bulk import added hidden filters or removed originals");
			OBSDataAutoRelease mode_group = imported_group(id, legacy);
			OBSDataAutoRelease link_group = imported_group(id, link);
			const std::string imported_mode_id = obs_data_get_string(mode_group, "id");
			const std::string imported_link_id = obs_data_get_string(link_group, "id");
			expect(click(importing, "viewsImportAll") && group_count() == 4 && result &&
				       result->text() == "Imported 0 controls. Skipped 3 already imported controls.",
			       "repeated Import all duplicated controls or did not report skipped originals");
			OBSDataAutoRelease repeated_mode = imported_group(id, legacy);
			OBSDataAutoRelease repeated_link = imported_group(id, link);
			expect(imported_mode_id == obs_data_get_string(repeated_mode, "id") &&
				       imported_link_id == obs_data_get_string(repeated_link, "id"),
			       "repeated Import all changed imported identities");
			importing->reject();
			until([&] { return !importing || !importing->isVisible(); });
			expect(legacy_press(studio, legacy, "front") && until([&] {
				       return current(id, imported_mode_id.c_str()) == "front" &&
					      obs_sceneitem_visible(legacy_main) &&
					      !obs_sceneitem_visible(legacy_detail);
			       }),
			       "existing Scene Mode key did not remain an alias after Import all");
			expect(click(panel, "viewsBackToControls") && until([&] {
				       return button_id(panel, mode) && button_id(panel, mode)->isEnabled() &&
					      button_id(panel, linked) && button_id(panel, linked)->isEnabled();
			       }) && press_button(panel, mode) &&
				       until([&] {
					       return current(id, imported_mode_id.c_str()) == "detail" &&
						      obs_sceneitem_visible(legacy_detail) &&
						      !obs_sceneitem_visible(legacy_main);
				       }) &&
				       press_button(panel, linked) && until([&] {
					       return obs_sceneitem_visible(nested) &&
						      !obs_sceneitem_visible(wide_camera);
				       }),
			       "pinned legacy controls stopped working after controller ownership transferred");
		}

		OBSSourceAutoRelease after_actions = obs_frontend_get_current_scene();
		expect(after_actions.Get() == program.Get(), "panel operation or management browsing changed Program");
		// Restore the previous management selection before saving for its own
		// restart fixture; operator controls remain the default page.
		expect(click(panel, "viewsManageControllers"), "could not restore controller management selection");
		scenes->setCurrentIndex(scenes->findData(old_scene));
		expect(until([&] { return controllers->findData(old_controller) >= 0; }),
		       "previous controller was not available after bulk import");
		controllers->setCurrentIndex(controllers->findData(old_controller));
		expect(until([&] { return scope->findData(old_scope) >= 0; }),
		       "previous controller scope was not restored");
		scope->setCurrentIndex(scope->findData(old_scope));
		expect(click(panel, "viewsBackToControls") && check_saved_panel(panel),
		       "custom labels, sections or saved order changed while importing controls");
		check_geometry(panel, issues, "a control button extended outside its section at the normal dock width");
		obs_frontend_save();

		const QString capture = qEnvironmentVariable("STAGEKIT_VIEWS_CAPTURE");
		if (!capture.isEmpty()) {
			const QDir directory = QFileInfo(capture).isDir() ? QDir(capture)
									  : QFileInfo(capture).absoluteDir();
			QDockWidget *dock = nullptr;
			for (auto *parent = panel->parentWidget(); !dock && parent; parent = parent->parentWidget())
				dock = qobject_cast<QDockWidget *>(parent);
			if (dock)
				dock->resize(520, 600);
			check_geometry(panel, issues,
				       "a control button was clipped by its section after resizing the dock");
			expect(panel->grab().save(directory.filePath("stagekit-controls.png")),
			       "could not capture the real operator panel");
			expect(click(panel, "viewsPanelEdit") && until([&] { return dialog("viewsPanelEditor"); }),
			       "could not open the panel editor for capture");
			editor = dialog("viewsPanelEditor");
			if (editor) {
				auto *search = editor->findChild<QLineEdit *>("viewsPanelSearch");
				if (search)
					search->setText("Studio");
				QCoreApplication::processEvents();
				expect(editor->grab().save(directory.filePath("stagekit-controls-editor.png")),
				       "could not capture the real panel editor");
				editor->reject();
				until([&] { return !editor || !editor->isVisible(); });
			}
		}
	}
	check_collection_isolation(panel, issues);
	if (issues.size() == before)
		blog(LOG_INFO,
		     "[StageKit controls test] PASS: cross-scene controls, native legacy actions, external state, multi-pin customization, rename/missing identity, bulk import and scene collection isolation");
}

} // namespace control_panel_test
