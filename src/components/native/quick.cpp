#include "component.hpp"
#include "quick-dialogs.hpp"

#include <obs-frontend-api.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <vector>

namespace stagekit::native {
namespace {
using namespace quick;

QString field(obs_data_t *data, const char *key)
{
	return QString::fromUtf8(obs_data_get_string(data, key));
}
QMainWindow *main_widget()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}
void redraw_manager();

struct Section {
	const char *label;
	QList<Source> sources;
	bool operator==(const Section &other) const
	{
		return qstrcmp(label, other.label) == 0 && sources == other.sources;
	}
};

class Board : public QWidget {
public:
	Options options;
	QString id;
	QLineEdit *search = nullptr;

	Board(Options settings, QString dockId, QWidget *parent = nullptr)
		: QWidget(parent),
		  options(settings),
		  id(dockId)
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(2, 2, 2, 2);
		layout->setSpacing(2);
		graph_update = new QTimer(this);
		graph_update->setSingleShot(true);
		connect(graph_update, &QTimer::timeout, this, [this] {
			if (isVisible())
				refresh();
		});
		if (options.mode == "Source Search") {
			search = new QLineEdit(this);
			search->setObjectName("nativeQuickSearch");
			search->setPlaceholderText(text("Quick.Search"));
			search->setToolTip(text("Native.FindSources"));
			search->setClearButtonEnabled(true);
			layout->addWidget(search);
			connect(search, &QLineEdit::textChanged, this, [this] { refresh(); });
		}
		list = new QTreeWidget(this);
		list->setObjectName("nativeQuickSources");
		list->setColumnCount(4);
		list->setHeaderHidden(true);
		list->setRootIsDecorated(false);
		list->setItemsExpandable(false);
		list->setIndentation(0);
		list->setEditTriggers(QAbstractItemView::NoEditTriggers);
		list->header()->setStretchLastSection(false);
		list->header()->setMinimumSectionSize(22);
		list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
		for (int col = 1; col < 4; ++col) {
			list->header()->setSectionResizeMode(col, QHeaderView::Fixed);
			list->setColumnWidth(col, 26);
		}
		layout->addWidget(list, 1);
		message = new QLabel(this);
		message->setAlignment(Qt::AlignCenter);
		message->setWordWrap(true);
		layout->addWidget(message, 1);
		connect(list, &QTreeWidget::currentItemChanged, this, [this] { update_moves(); });
		connect(list, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *row, int col) {
			const auto entry = row_source(row);
			if (options.clickable && col == 0 && !entry.uuid.isEmpty()) {
				auto source = resolve(entry);
				if (source && obs_source_is_scene(source)) {
					activate_scene(source);
					dismiss_search();
				}
			}
		});
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(list, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
			auto *row = list->itemAt(at);
			if (row && !row_source(row).uuid.isEmpty()) {
				list->setCurrentItem(row);
				open_source_menu(this, list->viewport()->mapToGlobal(at), row_source(row));
			}
		});

		if (!id.isEmpty()) {
			auto *bar = new QHBoxLayout;
			bar->setSpacing(1);
			layout->addLayout(bar);
			auto button = [&](const char *name, const char *label, const char *theme,
					  QStyle::StandardPixmap fallback, auto fn) {
				auto *b = tool_button(this, name, label, theme, style()->standardIcon(fallback));
				bar->addWidget(b);
				connect(b, &QToolButton::clicked, this, fn);
				return b;
			};
			if (options.mode == "Manual") {
				button("nativeQuickPin", "Quick.AddSources", "icon-plus",
				       QStyle::SP_FileDialogNewFolder, [this] { choose_sources(); });
				remove = button("nativeQuickUnpin", "Quick.RemoveSource", "icon-minus",
						QStyle::SP_TrashIcon, [this] { unpin(); });
				up = button("nativeQuickUp", "Quick.MoveUp", "icon-up", QStyle::SP_ArrowUp,
					    [this] { move(-1); });
				down = button("nativeQuickDown", "Quick.MoveDown", "icon-down", QStyle::SP_ArrowDown,
					      [this] { move(1); });
			}
			bar->addStretch();
			button("nativeQuickSettings", "Quick.DockOptions", "icon-gear",
			       QStyle::SP_FileDialogDetailedView, [this] { edit(); });
		}
		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] {
			if (isVisible())
				refresh();
		});
		timer->start(1000);
	}

	QDockWidget *dock() const { return qobject_cast<QDockWidget *>(parentWidget()); }
	QByteArray dock_key() const { return ("stagekit-quick-access-dock_" + id).toUtf8(); }
	void edit()
	{
		dock_options(this, options, false, [this](Options value) { apply(value); });
	}
	void apply(const Options &value)
	{
		options = value;
		if (auto *widget = dock()) {
			widget->setWindowTitle(options.title);
			widget->toggleViewAction()->setText(options.title);
		}
		refresh(true);
		redraw_manager();
		obs_frontend_save();
	}
	void restore_pins(obs_data_t *data)
	{
		OBSDataArrayAutoRelease saved = obs_data_get_array(data, "dock_sources");
		for (size_t i = 0; i < obs_data_array_count(saved); ++i) {
			OBSDataAutoRelease entry = obs_data_array_item(saved, i);
			pins.append({field(entry, "source_name"), field(entry, "source_uuid")});
		}
	}
	OBSDataAutoRelease save() const
	{
		OBSDataAutoRelease data = obs_data_create();
		obs_data_set_string(data, "dock_id", id.toUtf8().constData());
		obs_data_set_string(data, "dock_name", options.title.toUtf8().constData());
		obs_data_set_string(data, "dock_type", options.mode.toUtf8().constData());
		obs_data_set_bool(data, "show_properties", options.properties);
		obs_data_set_bool(data, "show_filters", options.filters);
		obs_data_set_bool(data, "show_scenes", options.parents);
		obs_data_set_bool(data, "clickable_scenes", options.clickable);
		OBSDataArrayAutoRelease entries = obs_data_array_create();
		for (const auto &pin : pins) {
			OBSDataAutoRelease entry = obs_data_create();
			obs_data_set_string(entry, "source_name", pin.name.toUtf8().constData());
			obs_data_set_string(entry, "source_uuid", pin.uuid.toUtf8().constData());
			obs_data_array_push_back(entries, entry);
		}
		obs_data_set_array(data, "dock_sources", entries);
		if (auto *widget = dock()) {
			obs_data_set_bool(data, "dock_hidden", widget->isHidden());
			obs_data_set_bool(data, "dock_floating", widget->isFloating());
			obs_data_set_string(data, "dock_geometry", widget->saveGeometry().toBase64().constData());
			obs_data_set_int(data, "dock_area", main_widget()->dockWidgetArea(widget));
		}
		return data;
	}

	void refresh(bool force = false)
	{
		QList<Section> sections;
		const char *empty;
		if (options.mode == "Manual") {
			sections.append({nullptr, {}});
			QSet<QString> seen;
			for (auto &pin : pins) {
				auto source = resolve(pin);
				if (!source)
					continue;
				pin = describe(source);
				if (!seen.contains(pin.uuid))
					sections[0].sources.append(pin);
				seen.insert(pin.uuid);
			}
			empty = "Quick.EmptyManual";
		} else if (options.mode == "Dynamic") {
			QList<Source> containers;
			sections.append({"Quick.Scene", program_sources(containers)});
			watch_scenes(containers);
			empty = "Quick.EmptyDynamic";
		} else {
			sections = {{"Native.Source", {}},
				    {"Quick.SourceType", {}},
				    {"Quick.FilePath", {}},
				    {"Quick.Url", {}},
				    {"Native.Filters", {}}};
			const auto term = search->text().trimmed();
			empty = term.isEmpty() ? "Native.FindSources" : "Quick.NoResults";
			if (!term.isEmpty()) {
				const unsigned masks[] = {Name, Type, File, Url, Filter};
				for (const auto &entry : available_sources()) {
					auto source = resolve(entry);
					const auto mask = search_matches(source, term);
					for (int i = 0; i < 5; ++i)
						if (mask & masks[i])
							sections[i].sources.append(entry);
				}
			}
		}
		message->setText(text(empty));
		if (!force && drawn && sections == shown)
			return;
		drawn = true;
		auto *current = list->currentItem();
		const auto selected = row_source(current).uuid;
		const auto selectedGroup = current && current->parent() ? current->parent()->text(0) : QString();
		QTreeWidgetItem *restore = nullptr;
		const int scroll = list->verticalScrollBar()->value();
		shown = sections;
		list->clear();
		list->setColumnHidden(1, !options.properties);
		list->setColumnHidden(2, !options.filters);
		list->setColumnHidden(3, !options.parents);
		for (const auto &section : sections) {
			if (section.sources.isEmpty())
				continue;
			auto *group = list->invisibleRootItem();
			if (section.label) {
				group = new QTreeWidgetItem(list, {text(section.label)});
				group->setFirstColumnSpanned(true);
				group->setFlags(Qt::ItemIsEnabled);
				auto font = group->font(0);
				font.setBold(true);
				group->setFont(0, font);
			}
			for (const auto &entry : section.sources) {
				auto source = resolve(entry);
				if (!source)
					continue;
				auto *row = new QTreeWidgetItem(group, {entry.name});
				row->setData(0, Qt::UserRole, entry.uuid);
				row->setIcon(0, source_icon(source));
				auto control = [&](int col, const char *name, const char *label, const char *theme,
						   QStyle::StandardPixmap fallback) {
					auto *button =
						tool_button(list, name, label, theme, style()->standardIcon(fallback));
					button->setFixedSize(24, 23);
					list->setItemWidget(row, col, button);
					return button;
				};
				if (options.properties && can_open_properties(source)) {
					auto *button = control(1, "nativeQuickProperties", "Native.Properties",
							       "icon-gear", QStyle::SP_FileDialogDetailedView);
					connect(button, &QToolButton::clicked, this, [this, entry] {
						auto source = resolve(entry);
						if (source) {
							dismiss_search();
							obs_frontend_open_source_properties(source);
						}
					});
				}
				if (options.filters) {
					auto *button = control(2, "nativeQuickFilters", "Native.Filters", "icon-filter",
							       QStyle::SP_FileDialogContentsView);
					connect(button, &QToolButton::clicked, this, [this, entry] {
						auto source = resolve(entry);
						if (source) {
							dismiss_search();
							obs_frontend_open_source_filters(source);
						}
					});
				}
				if (options.parents) {
					auto *button = control(3, "nativeQuickScenes", "Quick.ParentScenes",
							       "icon-scene", QStyle::SP_DirIcon);
					connect(button, &QToolButton::clicked, this, [this, entry, button] {
						open_parents(this, button->mapToGlobal(QPoint(0, button->height())),
							     entry);
					});
				}
				if (entry.uuid == selected &&
				    (!restore || (row->parent() && row->parent()->text(0) == selectedGroup)))
					restore = row;
			}
			if (section.label)
				group->setExpanded(true);
		}
		if (restore)
			list->setCurrentItem(restore);
		list->verticalScrollBar()->setValue(scroll);
		const bool hasRows = list->topLevelItemCount() > 0;
		list->setVisible(hasRows);
		message->setVisible(!hasRows);
		update_moves();
	}

protected:
	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		refresh();
	}

private:
	QTreeWidget *list;
	QLabel *message;
	QToolButton *remove = nullptr, *up = nullptr, *down = nullptr;
	QList<Source> pins;
	QList<Section> shown;
	QTimer *graph_update;
	QStringList watched;
	std::vector<OBSSignal> scene_connections;
	bool drawn = false;
	void watch_scenes(const QList<Source> &containers)
	{
		QStringList ids;
		for (const auto &entry : containers)
			ids.append(entry.uuid);
		if (ids == watched)
			return;
		watched = ids;
		scene_connections.clear();
		for (const auto &entry : containers) {
			auto source = resolve(entry);
			if (!source)
				continue;
			auto *handler = obs_source_get_signal_handler(source);
			for (const char *signal : {"item_add", "item_remove", "reorder", "refresh"})
				scene_connections.emplace_back(
					handler, signal,
					[](void *data, calldata_t *) {
						auto *board = static_cast<Board *>(data);
						QMetaObject::invokeMethod(
							board, [board] { board->graph_update->start(0); },
							Qt::QueuedConnection);
					},
					this);
		}
	}
	static Source row_source(QTreeWidgetItem *row)
	{
		return row ? Source{row->text(0), row->data(0, Qt::UserRole).toString()} : Source{};
	}
	void dismiss_search()
	{
		if (id.isEmpty())
			if (auto *dialog = qobject_cast<QDialog *>(window()))
				dialog->close();
	}
	void choose_sources()
	{
		pick_sources(this, pins, [this](const QList<Source> &chosen) {
			for (const auto &entry : chosen) {
				auto source = resolve(entry);
				if (source && pin_index(entry.uuid) < 0)
					pins.append(describe(source));
			}
			refresh();
			obs_frontend_save();
		});
	}
	int pin_index(const QString &uuid) const
	{
		for (int i = 0; i < pins.size(); ++i)
			if (pins[i].uuid == uuid)
				return i;
		return -1;
	}
	void update_moves()
	{
		if (!remove)
			return;
		const int row = list->indexOfTopLevelItem(list->currentItem());
		remove->setEnabled(row >= 0);
		up->setEnabled(row > 0);
		down->setEnabled(row >= 0 && row + 1 < list->topLevelItemCount());
	}
	void move(int delta)
	{
		const int row = list->indexOfTopLevelItem(list->currentItem());
		auto *next = list->topLevelItem(row + delta);
		if (row < 0 || !next)
			return;
		const int from = pin_index(row_source(list->currentItem()).uuid);
		const int to = pin_index(row_source(next).uuid);
		if (from >= 0 && to >= 0) {
			pins.swapItemsAt(from, to);
			refresh();
			obs_frontend_save();
		}
	}
	void unpin()
	{
		const auto source = row_source(list->currentItem());
		if (source.uuid.isEmpty())
			return;
		auto *confirm = new QMessageBox(QMessageBox::Question, text("Quick.RemoveSource"),
						QString(text("Quick.UnpinConfirm")).arg(source.name),
						QMessageBox::Yes | QMessageBox::No, this);
		confirm->setAttribute(Qt::WA_DeleteOnClose);
		connect(confirm, &QMessageBox::finished, this, [this, source](int result) {
			const int index = pin_index(source.uuid);
			if (result == QMessageBox::Yes && index >= 0) {
				pins.removeAt(index);
				refresh();
				obs_frontend_save();
			}
		});
		confirm->open();
	}
};

QList<QPointer<Board>> boards;
QPointer<QObject> owner;
QPointer<QDialog> manager_window, quick_search;
QPointer<QTableWidget> board_table;
obs_hotkey_id key = OBS_INVALID_HOTKEY_ID;

void redraw_manager()
{
	if (!board_table)
		return;
	auto *table = board_table.data();
	const QSignalBlocker block(table);
	auto *previous = table->item(table->currentRow(), 0);
	const auto selected = previous ? previous->data(Qt::UserRole).toString() : QString();
	table->setRowCount(0);
	for (const auto &board : boards) {
		if (!board)
			continue;
		const auto &o = board->options;
		const int row = table->rowCount();
		table->insertRow(row);
		auto *label = new QTableWidgetItem(o.title + " [" + mode_label(o.mode) + "]");
		label->setData(Qt::UserRole, board->id);
		table->setItem(row, 0, label);
		const bool states[] = {o.properties, o.filters, o.parents, o.clickable};
		for (int i = 0; i < 4; ++i) {
			auto *option = new QTableWidgetItem;
			option->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
			option->setCheckState(states[i] ? Qt::Checked : Qt::Unchecked);
			table->setItem(row, i + 1, option);
		}
		if (board->id == selected)
			table->selectRow(row);
	}
}
Board *find_board(const QString &id)
{
	for (const auto &board : boards)
		if (board && board->id == id)
			return board.data();
	return nullptr;
}
bool add_board(const Options &options, const QString &id, obs_data_t *saved = nullptr)
{
	if (!main_widget() || id.isEmpty() || options.title.isEmpty() || find_board(id))
		return false;
	auto *board = new Board(options, id);
	board->restore_pins(saved);
	if (!obs_frontend_add_dock_by_id(board->dock_key().constData(), options.title.toUtf8().constData(), board)) {
		delete board;
		return false;
	}
	boards.push_back(board);
	if (auto *widget = board->dock()) {
		auto area = static_cast<Qt::DockWidgetArea>(obs_data_get_int(saved, "dock_area"));
		if (area == Qt::TopDockWidgetArea || area == Qt::BottomDockWidgetArea ||
		    area == Qt::LeftDockWidgetArea || area == Qt::RightDockWidgetArea)
			main_widget()->addDockWidget(area, widget);
		widget->setFloating(!saved || obs_data_get_bool(saved, "dock_floating"));
		const auto geometry = QByteArray::fromBase64(field(saved, "dock_geometry").toLatin1());
		if (!geometry.isEmpty())
			widget->restoreGeometry(geometry);
		widget->setVisible(!obs_data_get_bool(saved, "dock_hidden"));
	}
	board->refresh();
	redraw_manager();
	return true;
}
void clear_boards()
{
	delete quick_search.data();
	delete manager_window.data();
	const auto old = boards;
	boards.clear();
	for (const auto &board : old)
		if (board)
			obs_frontend_remove_dock(board->dock_key().constData());
}
void search_window()
{
	if (quick_search && quick_search->isVisible()) {
		quick_search->close();
		return;
	}
	if (!main_widget())
		return;
	auto *dialog = new QDialog(main_widget(), Qt::Popup);
	quick_search = dialog;
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setWindowTitle(text("Quick.QuickSearch"));
	dialog->setObjectName("nativeQuickPopup");
	auto *layout = new QVBoxLayout(dialog);
	auto *board = new Board({dialog->windowTitle(), "Source Search", true, true, true, true}, {}, dialog);
	layout->addWidget(board);
	board->refresh();
	dialog->resize(420, 520);
	dialog->move(main_widget()->frameGeometry().center() - dialog->rect().center());
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
	board->search->setFocus();
}
void manager()
{
	if (manager_window && manager_window->isVisible()) {
		manager_window->raise();
		manager_window->activateWindow();
		return;
	}
	auto *dialog = new QDialog(main_widget());
	manager_window = dialog;
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setObjectName("nativeQuickManager");
	dialog->setWindowTitle(text("Ports.Tool.quick"));
	auto *layout = new QVBoxLayout(dialog);
	layout->addWidget(new QLabel(text("Quick.EditOptions"), dialog));
	auto *table = new QTableWidget(0, 5, dialog);
	board_table = table;
	table->setObjectName("nativeQuickBoards");
	table->setHorizontalHeaderLabels({text("Quick.Dock"), text("Native.Properties"), text("Native.Filters"),
					  text("Quick.ParentScenes"), text("Quick.Clickable")});
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->verticalHeader()->hide();
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	layout->addWidget(table);
	redraw_manager();
	auto selected = [table]() -> Board * {
		auto *row = table->item(table->currentRow(), 0);
		return row ? find_board(row->data(Qt::UserRole).toString()) : nullptr;
	};
	QObject::connect(table, &QTableWidget::itemChanged, dialog, [table](QTableWidgetItem *item) {
		const int col = item->column();
		auto *row = table->item(item->row(), 0);
		auto *board = row ? find_board(row->data(Qt::UserRole).toString()) : nullptr;
		if (!board || col < 1)
			return;
		auto opts = board->options;
		bool *fields[] = {&opts.properties, &opts.filters, &opts.parents, &opts.clickable};
		*fields[col - 1] = item->checkState() == Qt::Checked;
		board->options = opts;
		board->refresh(true);
		obs_frontend_save();
	});
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
	layout->addWidget(buttons);
	auto button = [&](const char *name, const char *label, auto fn) {
		auto *b = buttons->addButton(text(label), QDialogButtonBox::ActionRole);
		b->setObjectName(name);
		QObject::connect(b, &QPushButton::clicked, dialog, fn);
		return b;
	};
	button("nativeCreateQuickDock", "Quick.NewDock", [dialog] {
		dock_options(dialog, {}, true, [](Options value) {
			if (add_board(value, QUuid::createUuid().toString(QUuid::WithoutBraces)))
				obs_frontend_save();
		});
	});
	auto *edit = button("nativeQuickEdit", "Quick.DockOptions", [dialog, selected] {
		if (auto *board = selected()) {
			QPointer<Board> live = board;
			dock_options(dialog, board->options, false, [live](Options value) {
				if (live)
					live->apply(value);
			});
		}
	});
	auto *show = button("nativeQuickShow", "Native.ShowDock", [selected] {
		auto *board = selected();
		if (board && board->dock()) {
			board->dock()->show();
			board->dock()->raise();
		}
	});
	auto *remove = button("nativeQuickDelete", "Quick.RemoveDock", [dialog, selected] {
		QPointer<Board> board = selected();
		if (!board)
			return;
		auto *confirm = new QMessageBox(QMessageBox::Question, dialog->windowTitle(),
						QString(text("Native.RemoveDock")).arg(board->options.title),
						QMessageBox::Yes | QMessageBox::No, dialog);
		confirm->setAttribute(Qt::WA_DeleteOnClose);
		QObject::connect(confirm, &QMessageBox::finished, dialog, [board](int result) {
			if (result == QMessageBox::Yes && board) {
				boards.removeAll(board);
				obs_frontend_remove_dock(board->dock_key().constData());
				redraw_manager();
				obs_frontend_save();
			}
		});
		confirm->open();
	});
	button("nativeQuickOpenSearch", "Quick.QuickSearch", search_window);
	auto enable = [=] {
		const bool has = selected() != nullptr;
		for (auto *b : {edit, show, remove})
			b->setEnabled(has);
	};
	QObject::connect(table, &QTableWidget::itemSelectionChanged, dialog, enable);
	QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
	enable();
	dialog->resize(760, 410);
	dialog->show();
}

void persist(obs_data_t *root, bool saving, void *)
{
	if (saving) {
		OBSDataAutoRelease data = obs_data_create();
		OBSDataArrayAutoRelease array = obs_data_array_create();
		for (const auto &board : boards)
			if (board) {
				auto item = board->save();
				obs_data_array_push_back(array, item);
			}
		obs_data_set_bool(data, "first_run", false);
		obs_data_set_array(data, "docks", array);
		OBSDataArrayAutoRelease binding = obs_hotkey_save(key);
		if (binding)
			obs_data_set_array(data, "quick_search_hotkey", binding);
		obs_data_set_obj(root, "stagekit.quick_access", data);
	} else {
		clear_boards();
		OBSDataAutoRelease data = obs_data_get_obj(root, "stagekit.quick_access");
		if (!data && !obs_get_module("obs-quick-access-utility"))
			data = obs_data_get_obj(root, "quick_access_utility");
		OBSDataArrayAutoRelease binding = obs_data_get_array(data, "quick_search_hotkey");
		obs_hotkey_load(key, binding);
		OBSDataArrayAutoRelease entries = obs_data_get_array(data, "docks");
		for (size_t i = 0; i < obs_data_array_count(entries); ++i) {
			OBSDataAutoRelease d = obs_data_array_item(entries, i);
			Options opts{field(d, "dock_name"),
				     field(d, "dock_type"),
				     obs_data_get_bool(d, "show_properties"),
				     obs_data_get_bool(d, "show_filters"),
				     obs_data_get_bool(d, "show_scenes"),
				     obs_data_get_bool(d, "clickable_scenes")};
			if (opts.mode != "Manual" && opts.mode != "Dynamic" && opts.mode != "Source Search")
				continue;
			add_board(opts, field(d, "dock_id"), d);
		}
	}
}
void event(obs_frontend_event which, void *)
{
	if (which == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || which == OBS_FRONTEND_EVENT_EXIT)
		clear_boards();
	else if (which == OBS_FRONTEND_EVENT_SCENE_CHANGED && owner)
		QMetaObject::invokeMethod(
			owner,
			[] {
				for (const auto &board : boards)
					if (board && board->options.mode == "Dynamic")
						board->refresh();
			},
			Qt::QueuedConnection);
}
void load()
{
	if (!main_widget())
		return;
	owner = new QObject(main_widget());
	auto *menu = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(text("Ports.Tool.quick")));
	if (menu)
		QObject::connect(menu, &QAction::triggered, owner, manager);
	key = obs_hotkey_register_frontend(
		"Quick Search", text("Quick.OpenSearch"),
		[](void *, obs_hotkey_id, obs_hotkey_t *, bool down) {
			if (down && owner)
				QMetaObject::invokeMethod(owner, search_window, Qt::QueuedConnection);
		},
		nullptr);
	obs_frontend_add_save_callback(persist, nullptr);
	obs_frontend_add_event_callback(event, nullptr);
}
void unload()
{
	if (key == OBS_INVALID_HOTKEY_ID && !owner)
		return;
	obs_frontend_remove_event_callback(event, nullptr);
	obs_frontend_remove_save_callback(persist, nullptr);
	obs_hotkey_unregister(key);
	key = OBS_INVALID_HOTKEY_ID;
	clear_boards();
	delete owner.data();
}
} // namespace
const Component component{load, unload};
} // namespace stagekit::native
