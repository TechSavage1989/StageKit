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
