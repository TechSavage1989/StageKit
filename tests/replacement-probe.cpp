#include "replacement-frame.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QDialog>
#include <QFile>
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

static void exercise()
{
	QStringList issues;
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
	check_blur_output(issues);
	QFile report(qEnvironmentVariable("STAGEKIT_TEST_REPORT"));
	if (report.open(QIODevice::WriteOnly))
		report.write(issues.isEmpty() ? QByteArray("PASS\n") : issues.join('\n').toUtf8());
	QTimer::singleShot(50, qApp, &QApplication::quit);
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
