/*
StageKit - quality-of-life tools for OBS Studio
Copyright (C) 2026 StageKit contributors

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "window-layout.hpp"

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAction>
#include <QByteArray>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>

#include <string>

namespace stagekit {

namespace {

// How long after OBS says it has loaded the layout is brought back: docks
// that plugins and browser docks add late are there by then.
constexpr int kAfterLoadingMs = 1500;

std::string layout_file()
{
	char *path = obs_module_config_path("window-layout.json");
	std::string out = path ? path : "";
	bfree(path);
	return out;
}

// What is kept: the layout itself, and the two choices. (Each missing until
// first set: a layout that was saved comes back at start-up as it comes.)
obs_data_t *read_file()
{
	const std::string file = layout_file();
	obs_data_t *data = file.empty() ? nullptr : obs_data_create_from_json_file(file.c_str());
	if (!data)
		data = obs_data_create();
	obs_data_set_default_bool(data, "at_start", true);
	obs_data_set_default_bool(data, "at_exit", false);
	return data;
}

bool write_file(obs_data_t *data)
{
	const std::string file = layout_file();
	if (file.empty())
		return false;
	char *folder = obs_module_config_path("");
	if (folder)
		os_mkdirs(folder);
	bfree(folder);
	return obs_data_save_json_safe(data, file.c_str(), "tmp", "bak");
}

bool choice(const char *name)
{
	obs_data_t *data = read_file();
	const bool on = obs_data_get_bool(data, name);
	obs_data_release(data);
	return on;
}

void set_choice(const char *name, bool on)
{
	obs_data_t *data = read_file();
	obs_data_set_bool(data, name, on);
	write_file(data);
	obs_data_release(data);
}

QMainWindow *main_window()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

const char *text(const char *key)
{
	return obs_module_text(key);
}

} // namespace

bool window_layout_save(QMainWindow *window)
{
	if (!window)
		return false;
	obs_data_t *data = read_file();
	obs_data_set_string(data, "state", window->saveState().toBase64().constData());
	const bool written = write_file(data);
	obs_data_release(data);
	obs_log(written ? LOG_INFO : LOG_WARNING, "Window layout: %s", written ? "saved" : "could not be saved");
	return written;
}

bool window_layout_saved()
{
	obs_data_t *data = read_file();
	const bool there = *obs_data_get_string(data, "state") != '\0';
	obs_data_release(data);
	return there;
}

bool window_layout_restore(QMainWindow *window)
{
	if (!window)
		return false;
	obs_data_t *data = read_file();
	const QByteArray state = QByteArray::fromBase64(obs_data_get_string(data, "state"));
	obs_data_release(data);
	if (state.isEmpty())
		return false;
	const bool fitted = window->restoreState(state);
	obs_log(fitted ? LOG_INFO : LOG_WARNING, "Window layout: %s",
		fitted ? "brought back" : "the saved layout does not fit this window");
	return fitted;
}

void window_layout_menu(QMenu *menu)
{
	if (!menu)
		return;
	menu->addSeparator();
	QAction *save = menu->addAction(text("WindowLayout.Save"));
	save->setObjectName("layoutSave");
	QObject::connect(save, &QAction::triggered, menu, [] {
		QMainWindow *window = main_window();
		const bool saved = window_layout_save(window);
		if (window)
			QMessageBox::information(window, text("WindowLayout.Title"),
						 text(saved ? "WindowLayout.Saved" : "WindowLayout.NotSaved"));
	});
	QAction *restore = menu->addAction(text("WindowLayout.Restore"));
	restore->setObjectName("layoutRestore");
	QObject::connect(restore, &QAction::triggered, menu, [] {
		QMainWindow *window = main_window();
		if (!window_layout_restore(window) && window)
			QMessageBox::information(window, text("WindowLayout.Title"),
						 text(window_layout_saved() ? "WindowLayout.NoFit"
									    : "WindowLayout.None"));
	});
	QAction *at_start = menu->addAction(text("WindowLayout.AtStart"));
	at_start->setObjectName("layoutAtStart");
	at_start->setCheckable(true);
	at_start->setChecked(choice("at_start"));
	QObject::connect(at_start, &QAction::toggled, menu, [](bool on) { set_choice("at_start", on); });
	QAction *at_exit = menu->addAction(text("WindowLayout.AtExit"));
	at_exit->setObjectName("layoutAtExit");
	at_exit->setCheckable(true);
	at_exit->setChecked(choice("at_exit"));
	QObject::connect(at_exit, &QAction::toggled, menu, [](bool on) { set_choice("at_exit", on); });
}

bool window_layout_restores_after(enum obs_frontend_event event)
{
	// When OBS starts, and after every change of scene collection: OBS takes
	// down and puts up again the docks that belong to a collection (a
	// source's dock, a browser dock), and the layout came back only once,
	// at start-up. The owner: "Dock/windows layout doesn't automatically
	// restore after saving and switching to another collection".
	return event == OBS_FRONTEND_EVENT_FINISHED_LOADING || event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED;
}

void window_layout_event(enum obs_frontend_event event)
{
	if (window_layout_restores_after(event)) {
		if (!choice("at_start") || !window_layout_saved())
			return;
		QPointer<QMainWindow> window(main_window());
		if (!window)
			return;
		QTimer::singleShot(kAfterLoadingMs, window, [window] {
			if (window)
				window_layout_restore(window);
		});
	} else if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		// (Sent before OBS takes its window apart: the docks are still
		// where they were.)
		if (choice("at_exit"))
			window_layout_save(main_window());
	}
}

} // namespace stagekit
