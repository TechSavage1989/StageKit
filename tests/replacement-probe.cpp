#include "replacement-frame.hpp"
#include "replacement-native.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDockWidget>
#include <QComboBox>
#include <QPushButton>
#include <QSpinBox>
#include <QFile>
#include <QListWidget>
#include <QMainWindow>
#include <QSettings>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QTableWidget>
#include <QTextStream>
#include <QThread>
#include <QTimer>

OBS_DECLARE_MODULE()

static QAction *action_named(const QString &name)
{
	for (QWidget *widget : QApplication::allWidgets()) {
		if (auto *menu = qobject_cast<QMenu *>(widget))
			for (QAction *a : menu->actions())
				if (QString(a->text()).remove('&') == name)
					return a;
	}
	return nullptr;
}

static void check_blur_output(QStringList &issues)
{
	const QByteArray fixture = qgetenv("STAGEKIT_TEST_ART");
	if (fixture.isEmpty())
		return;
	obs_data_t *image_settings = obs_data_create();
	obs_data_set_string(image_settings, "file", fixture.constData());
	auto *source = obs_source_create_private("image_source", "StageKit render fixture", image_settings);
	obs_data_release(image_settings);
	if (!source) {
		issues.append("Could not create the render fixture");
		return;
	}
	obs_source_inc_showing(source);
	QThread::msleep(160);
	const auto w = obs_source_get_width(source);
	const auto h = obs_source_get_height(source);
	if (w == 320 && h == 180) {
		const auto input = replacement_test::render(source, w, h);
		const double contrast = replacement_test::stripe_contrast(input, w);
		if (!replacement_test::retains_artwork(input))
			issues.append("Fixture was blank before adding Composite Blur");
		for (const auto *mode : {"composite", "composite-kawase", "composite-hex"}) {
			auto *settings = obs_data_create();
			replacement_test::configure_blur(settings, mode);
			auto *filter = obs_source_create_private("stagekit_port_obs_composite_blur", mode, settings);
			obs_data_release(settings);
			if (!filter) {
				issues.append(QString("Could not create %1").arg(mode));
				continue;
			}
			obs_source_filter_add(source, filter);
			QThread::msleep(160);
			replacement_test::render(source, w, h);
			QThread::msleep(60);
			const auto pixels = replacement_test::render(source, w, h);
			if (!replacement_test::retains_artwork(pixels) ||
			    replacement_test::stripe_contrast(pixels, w) >= 0.85 * contrast)
				issues.append(
					QString("%1 did not preserve the image and soften the stripes").arg(mode));
			obs_source_filter_remove(source, filter);
			obs_source_release(filter);
		}
	} else {
		issues.append("Unexpected render fixture dimensions");
	}
	obs_source_dec_showing(source);
	obs_source_release(source);
}

static void check_native_layout(QStringList &issues)
{
	auto *module = obs_get_module("stagekit-port-layouts");
	const char *author = obs_get_module_author(module);
	auto *action = action_named("Dock Layout Manager (StageKit)");
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!author || QString::fromUtf8(author) != "StageKit contributors" || !action || !main) {
		issues.append("Missing native dock layout manager");
		return;
	}
	char *path = obs_module_get_config_path(module, "dock-layouts.ini");
	QSettings layouts(QString::fromUtf8(path), QSettings::IniFormat);
	bfree(path);
	layouts.setValue("Saved beta234 docks/WindowState", main->saveState());
	layouts.sync();
	bool found = false;
	QObject owner;
	QTimer::singleShot(100, &owner, [&] {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog)
			return;
		auto *list = dialog->findChild<QListWidget *>("nativeLayouts");
		found = list && list->findItems("Saved beta234 docks", Qt::MatchExactly).size() == 1;
		dialog->reject();
	});
	action->trigger();
	if (!found)
		issues.append("Native dock layouts did not read the existing WindowState entry");
}

static void check_native_table(QStringList &issues)
{
	auto *action = action_named("Transition Table (StageKit)");
	if (!action)
		return;
	QObject owner;
	bool exercised = false;
	QTimer::singleShot(100, &owner, [&] {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog)
			return;
		auto *from = dialog->findChild<QComboBox *>("nativeRuleFrom");
		auto *to = dialog->findChild<QComboBox *>("nativeRuleTo");
		auto *transition = dialog->findChild<QComboBox *>("nativeRuleTransition");
		auto *add = dialog->findChild<QPushButton *>("nativeSetRule");
		auto *duration = dialog->findChild<QSpinBox *>();
		if (from && to && transition && add && duration) {
			from->setCurrentText("Fixture source scene");
			to->setCurrentText("Any");
			transition->setCurrentText("Fade");
			duration->setValue(416);
			add->click();
			calldata_t result = {};
			calldata_set_string(&result, "from_scene", "Fixture source scene");
			calldata_set_string(&result, "to_scene", "Other destination");
			exercised = proc_handler_call(obs_get_proc_handler(),
						      "stagekit_port_get_transition_table_transition", &result) &&
				    QString::fromUtf8(calldata_string(&result, "transition")) == "Fade" &&
				    calldata_int(&result, "duration") == 416;
			calldata_free(&result);
		}
		dialog->reject();
	});
	action->trigger();
	if (!exercised)
		issues.append(
			"Native transition table did not resolve an edited wildcard rule through the saved procedure");
}

static void check_native_quick(QStringList &issues)
{
	auto *action = action_named("Quick Access Utility (StageKit)");
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!action || !main)
		return;
	auto *source = obs_source_create("image_source", "StageKit search fixture", nullptr, nullptr);
	bool found = false;
	QObject owner;
	QTimer::singleShot(100, &owner, [&] {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog)
			return;
		auto *title = dialog->findChild<QLineEdit *>("nativeQuickTitle");
		auto *create = dialog->findChild<QPushButton *>("nativeCreateQuickDock");
		if (title && create) {
			title->setText("StageKit fixture dock");
			create->click();
			for (auto *dock : main->findChildren<QDockWidget *>()) {
				if (dock->windowTitle() != "StageKit fixture dock")
					continue;
				auto *search = dock->findChild<QLineEdit *>("nativeQuickSearch");
				auto *list = dock->findChild<QListWidget *>("nativeQuickSources");
				if (search && list) {
					search->setText("StageKit search fixture");
					found = list->count() == 1 &&
						list->item(0)->text() == "StageKit search fixture";
				}
			}
		}
		dialog->reject();
	});
	action->trigger();
	if (!found)
		issues.append("The native Quick Access dock did not find a live OBS source");
	obs_source_release(source);
}

static void check_plugin_manager(QStringList &issues)
{
	const auto expected = qEnvironmentVariable("STAGEKIT_TEST_MODULE_NAMES").split('\n', Qt::SkipEmptyParts);
	if (expected.isEmpty())
		return;
	auto *action = action_named("Plugin Manager");
	if (!action) {
		issues.append("Missing OBS Plugin Manager action");
		return;
	}

	bool opened = false;
	QObject timer_owner;
	QTimer::singleShot(200, &timer_owner, [&] {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog)
			return;
		opened = true;
		QStringList labels;
		for (auto *box : dialog->findChildren<QCheckBox *>())
			labels.append(box->text());
		for (const auto &name : expected)
			if (labels.count(name) != 1)
				issues.append("Expected one Plugin Manager row for " + name);
		dialog->reject();
	});
	action->trigger();
	if (!opened)
		issues.append("Could not inspect the OBS Plugin Manager window");
}

static void check_ndi_preference(QStringList &issues)
{
	const auto phase = qEnvironmentVariable("STAGEKIT_TEST_NDI");
	const bool previously_used = phase == "upgrade";
	if ((obs_get_module("stagekit-port-ndi") != nullptr) != previously_used)
		issues.append("NDI module load did not match the saved preference");
	auto *action = action_named("OBS filter replacements");
	if (!action) {
		issues.append("No replacement catalog for NDI preference");
		return;
	}
	bool checked = false;
	QObject owner;
	QTimer::singleShot(100, &owner, [&] {
		auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
		if (!dialog)
			return;
		auto *option = dialog->findChild<QCheckBox *>("stagekitLoadNdi");
		checked = option && option->isChecked() == previously_used;
		if (previously_used && option)
			option->click();
		dialog->reject();
	});
	action->trigger();
	if (!checked)
		issues.append("NDI preference in the catalog did not match this installation");
}

static void finish_probe(const QStringList &issues)
{
	QFile report(qEnvironmentVariable("STAGEKIT_TEST_REPORT"));
	if (report.open(QIODevice::WriteOnly))
		report.write(issues.isEmpty() ? QByteArray("PASS\n") : issues.join('\n').toUtf8());
	// Close the OBS main window as the user would. QApplication::quit can close a
	// floating dock first and queue OBS's first-time dock-close information dialog.
	auto *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QTimer::singleShot(50, main, &QWidget::close);
}

static void exercise()
{
	QStringList issues;
	check_ndi_preference(issues);
	if (qEnvironmentVariable("STAGEKIT_TEST_NDI") == "restart") {
		finish_probe(issues);
		return;
	}
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		const auto *box = qobject_cast<QMessageBox *>(widget);
		if (box && box->isVisible() && box->text().contains("NDI", Qt::CaseInsensitive))
			issues.append("Unexpected NDI startup dialog");
	}
	QStringList registered;
	const char *id = nullptr;
	for (size_t i = 0; obs_enum_filter_types(i, &id); ++i) {
		const char *label = obs_source_get_display_name(id);
		if (label)
			registered.append(QString::fromUtf8(label));
	}
	const QStringList expected = {"Source Filter: Advanced Mask (StageKit)",
				      "Source Filter: Gaussian Blur (StageKit)", "Scene Filter: Move Source (StageKit)",
				      "Scene Filter: Move Source Swap (StageKit)"};
	for (const auto &name : expected)
		if (!registered.contains(name))
			issues.append(name);
	const QStringList tools = {"Source Copy (StageKit)",          "Source Dock (StageKit)",
				   "Transition Table (StageKit)",     "Scene Collection Manager (StageKit)",
				   "Quick Access Utility (StageKit)", "Dock Layout Manager (StageKit)"};
	for (const QString &name : tools)
		if (!action_named(name))
			issues.append(name);
	QAction *catalog = action_named("OBS filter replacements");
	if (!catalog) {
		issues.append("Missing StageKit catalog");
	} else {
		bool opened = false, matched = false;
		QObject timer_owner;
		QTimer::singleShot(250, &timer_owner, [&] {
			auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
			if (!dialog)
				return;
			auto *search = dialog->findChild<QLineEdit *>();
			auto *table = dialog->findChild<QTableWidget *>();
			opened = search && table && table->rowCount() > 40;
			if (opened) {
				for (int i = 0; i < table->rowCount(); ++i) {
					const auto original = table->item(i, 0)->text();
					const auto status = table->item(i, 2)->text();
					const auto implementation =
						table->columnCount() == 4 ? table->item(i, 3)->text() : QString();
					if ((original == "Freeze Filter" && implementation != "StageKit native") ||
					    (original == "Composite Blur" && implementation != "Bundled port") ||
					    (original == "Chroma Key" && implementation != "OBS built-in"))
						issues.append(original + " has an incorrect implementation label");
					if ((original == "Composite Blur" || original == "Source Copy") &&
					    status != "Available")
						issues.append(original + ": " + status);
#ifdef __linux__
					if (original == "Move Video Capture Device" &&
					    status != "Requires Windows DirectShow")
						issues.append(original + ": " + status);
#endif
				}
				search->setText("DWM.Name");
				int visible = 0;
				for (int i = 0; i < table->rowCount(); ++i)
					if (!table->isRowHidden(i)) {
						++visible;
						matched = table->item(i, 1)->text() ==
							  "Source Filter: Directory Watch Media (StageKit)";
					}
				matched = matched && visible == 1;
			}
			dialog->reject();
		});
		catalog->trigger();
		if (!opened || !matched)
			issues.append("Catalog search did not return the labeled Directory Watch Media filter");
	}
	replacement_test::check_native_filters(qEnvironmentVariable("STAGEKIT_TEST_ART"), issues);
	for (const char *key : {"freeze", "pixel", "3d", "recursion", "warp", "layouts", "directory", "defaults",
				"delay", "table", "quick", "clone"}) {
		const auto module_name = QByteArray("stagekit-port-") + key;
		auto *module = obs_get_module(module_name.constData());
		const char *author = module ? obs_get_module_author(module) : nullptr;
		if (!author || QString::fromUtf8(author) != "StageKit contributors")
			issues.append("Incorrect author for " + QString::fromUtf8(module_name));
	}
	check_native_layout(issues);
	check_native_table(issues);
	check_native_quick(issues);
	check_plugin_manager(issues);
	check_blur_output(issues);
	finish_probe(issues);
}

bool obs_module_load()
{
	if (!qEnvironmentVariableIsSet("STAGEKIT_TEST_REPORT"))
		return false;
	obs_frontend_add_event_callback(
		[](obs_frontend_event event, void *) {
			if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
				QTimer::singleShot(2600, qApp, &exercise);
		},
		nullptr);
	return true;
}
