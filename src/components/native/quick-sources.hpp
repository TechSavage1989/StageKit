#pragma once

#include <obs.hpp>
#include <QIcon>
#include <QList>
#include <QString>

class QMenu;
class QToolButton;
class QWidget;

namespace stagekit::native::quick {
struct Source {
	QString name, uuid;
	bool operator==(const Source &other) const { return uuid == other.uuid && name == other.name; }
};
enum Match { Name = 1, Type = 2, Filter = 4, Url = 8, File = 16 };

Source describe(obs_source_t *source);
OBSSourceAutoRelease resolve(const Source &source);
QList<Source> available_sources();
QList<Source> program_sources();
unsigned search_matches(obs_source_t *source, const QString &query);
QIcon source_icon(obs_source_t *source);
bool can_open_properties(obs_source_t *source);
void activate_scene(obs_source_t *source);
QToolButton *tool_button(QWidget *parent, const char *name, const char *label, const char *theme, const QIcon &icon);
void open_parents(QWidget *parent, const QPoint &position, const Source &source);
void open_source_menu(QWidget *parent, const QPoint &position, const Source &source);
} // namespace stagekit::native::quick
