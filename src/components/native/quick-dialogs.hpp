#pragma once

#include "quick-sources.hpp"
#include <functional>

namespace stagekit::native::quick {
struct Options {
	QString title;
	QString mode = "Manual";
	bool properties = true;
	bool filters = true;
	bool parents = true;
	bool clickable = false;
};
QString mode_label(const QString &mode);
void dock_options(QWidget *parent, const Options &initial, bool creating, std::function<void(Options)> apply);
void pick_sources(QWidget *parent, const QList<Source> &pinned, std::function<void(QList<Source>)> add);
} // namespace stagekit::native::quick
