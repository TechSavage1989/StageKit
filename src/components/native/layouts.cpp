#include "component.hpp"

#include <obs-frontend-api.h>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace stagekit::native {
namespace {
QString settings_path()
{
	char *path = obs_module_config_path("dock-layouts.ini");
	const QString file = QString::fromUtf8(path);
	bfree(path);
	return file;
}
QMainWindow *obs_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_obs_window());
}
constexpr auto default_key = "Settings/DefaultLayout";
QString state_key(const QString &name)
{
	return name + "/WindowState";
}

class Layouts : public QDialog {
public:
	explicit Layouts(QMainWindow *parent, const QString &file)
		: QDialog(parent),
		  settings(file, QSettings::IniFormat)
	{
		setWindowTitle(text("Ports.Tool.layouts"));
		auto *layout = new QVBoxLayout(this);
		list = new QListWidget(this);
		list->setObjectName("nativeLayouts");
		layout->addWidget(list);
		status = new QLabel(this);
		layout->addWidget(status);
		auto *commands = new QHBoxLayout;
		layout->addLayout(commands);
		auto button = [&](const char *key, auto action) {
			auto *b = new QPushButton(text(key), this);
			commands->addWidget(b);
			connect(b, &QPushButton::clicked, this, action);
			return b;
		};
		button("Native.NewLayout", [this] { name_layout(false); });
		auto *save = button("Native.SaveLayout", [this] { store_state(current()); });
		auto *apply = button("Native.ApplyLayout", [this] { restore(); });
		auto *rename = button("Native.Rename", [this] { name_layout(true); });
		auto *startup = button("Native.StartupLayout", [this] {
			settings.setValue(default_key, current());
			sync();
			refresh(current());
		});
		auto *remove = button("Native.Remove", [this] {
			const auto name = current();
			if (QMessageBox::question(this, windowTitle(),
						  QString(text("Native.RemoveLayout")).arg(name)) != QMessageBox::Yes)
				return;
			settings.remove(name);
			if (settings.value(default_key).toString() == name)
				settings.remove(default_key);
			sync();
			refresh({});
		});
		connect(list, &QListWidget::currentItemChanged, this, [=](QListWidgetItem *item) {
			for (auto *b : {save, apply, rename, startup, remove})
				b->setEnabled(item != nullptr);
		});
		for (auto *b : {save, apply, rename, startup, remove})
			b->setEnabled(false);
		connect(list, &QListWidget::itemDoubleClicked, this, [this] { restore(); });
		auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
		connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
		layout->addWidget(close);
		refresh({});
	}

private:
	QSettings settings;
	QListWidget *list;
	QLabel *status;
	QString current() const { return list->currentItem() ? list->currentItem()->text() : QString(); }
	void sync()
	{
		settings.sync();
		if (settings.status() != QSettings::NoError)
			QMessageBox::warning(this, windowTitle(), text("Native.LayoutWriteError"));
	}
	void refresh(const QString &select)
	{
		const QString choice = select;
		list->clear();
		for (const auto &name : settings.childGroups()) {
			if (!settings.contains(state_key(name)))
				continue;
			auto *item = new QListWidgetItem(name, list);
			if (name == choice)
				list->setCurrentItem(item);
		}
		const auto def = settings.value(default_key).toString();
		status->setText(def.isEmpty() ? text("Native.NoStartupLayout")
					      : QString(text("Native.CurrentStartup")).arg(def));
	}
	void store_state(const QString &name)
	{
		if (name.isEmpty())
			return;
		settings.setValue(state_key(name), obs_window()->saveState());
		sync();
		refresh(name);
	}
	void restore()
	{
		const auto bytes = settings.value(state_key(current())).toByteArray();
		if (bytes.isEmpty() || !obs_window()->restoreState(bytes))
			QMessageBox::warning(this, windowTitle(), text("Native.LayoutRestoreError"));
	}
	void name_layout(bool rename)
	{
		const auto old = current();
		bool accepted = false;
		const auto name = QInputDialog::getText(this, windowTitle(), text("Native.LayoutName"),
							QLineEdit::Normal, rename ? old : QString(), &accepted)
					  .trimmed();
		if (!accepted || name.isEmpty() || (rename && name == old))
			return;
		if (name.contains('/') || name.contains('\\') || name.compare("Settings", Qt::CaseInsensitive) == 0 ||
		    settings.childGroups().contains(name, Qt::CaseInsensitive)) {
			QMessageBox::warning(this, windowTitle(), text("Native.LayoutNameTaken"));
			return;
		}
		if (!rename) {
			store_state(name);
			return;
		}
		settings.setValue(state_key(name), settings.value(state_key(old)));
		if (settings.value(default_key).toString() == old)
			settings.setValue(default_key, name);
		settings.remove(old);
		sync();
		refresh(name);
	}
};

QMetaObject::Connection menu_connection;
void event(enum obs_frontend_event which, void *)
{
	if (which != OBS_FRONTEND_EVENT_FINISHED_LOADING || !obs_window())
		return;
	QSettings settings(settings_path(), QSettings::IniFormat);
	const auto name = settings.value(default_key).toString();
	const auto state = settings.value(state_key(name)).toByteArray();
	if (!name.isEmpty() && !state.isEmpty())
		obs_window()->restoreState(state);
}
void load()
{
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(text("Ports.Tool.layouts")));
	menu_connection = QObject::connect(action, &QAction::triggered, obs_window(), [] {
		const auto path = settings_path();
		if (!path.isEmpty() && QDir().mkpath(QFileInfo(path).absolutePath())) {
			Layouts dialog(obs_window(), path);
			dialog.exec();
		}
	});
	obs_frontend_add_event_callback(event, nullptr);
}
void unload()
{
	obs_frontend_remove_event_callback(event, nullptr);
	QObject::disconnect(menu_connection);
}
} // namespace
const Component component{load, unload};
} // namespace stagekit::native
