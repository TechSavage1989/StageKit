#include "component.hpp"
#include "../../third-party/obs-websocket-api.h"

#include <obs-frontend-api.h>
#include <obs.hpp>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QPointer>
#include <QSaveFile>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>

namespace stagekit::native {
namespace {
using Pair = std::pair<std::string, std::string>;
struct Rule {
	std::string name;
	long long ms = 0;
};
using Rules = std::map<Pair, Rule>;
Rules rules;
std::mutex model;
std::atomic_bool enabled{true};
QPointer<QObject> context;
QSize dialog_size{760, 410};
obs_hotkey_pair_id keys = OBS_INVALID_HOTKEY_PAIR_ID;
obs_websocket_vendor vendor = nullptr;

Rule pick(const Rules &entries, const Pair &pair)
{
	for (const auto &from : {pair.first, std::string("Any")})
		for (const auto &to : {pair.second, std::string("Any")}) {
			auto found = entries.find({from, to});
			if (found != entries.end())
				return found->second;
		}
	return {};
}

Rules snapshot()
{
	std::lock_guard lock(model);
	return rules;
}

void assign_field(obs_data_t *dest, obs_data_t *src, const char *field, bool number)
{
	if (!obs_data_has_user_value(src, field))
		obs_data_erase(dest, field);
	else if (number)
		obs_data_set_int(dest, field, obs_data_get_int(src, field));
	else
		obs_data_set_string(dest, field, obs_data_get_string(src, field));
}

// A saved scene can already have an OBS transition override. Keep its previous values
// alongside our last write, including across a save and restart.
void apply_rules()
{
	const Rules entries = enabled ? snapshot() : Rules{};
	OBSSourceAutoRelease current = obs_frontend_get_current_scene();
	const std::string from = current ? obs_source_get_name(current) : "";
	obs_frontend_source_list scenes{};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; ++i) {
		auto *scene = scenes.sources.array[i];
		OBSDataAutoRelease data = obs_source_get_private_settings(scene);
		const Rule rule = pick(entries, {from, obs_source_get_name(scene)});
		OBSDataAutoRelease backup = obs_data_get_obj(data, "stagekit.table.override");
		if (backup) {
			if (std::string(obs_data_get_string(data, "transition")) ==
				    obs_data_get_string(backup, "applied") &&
			    obs_data_get_int(data, "transition_duration") == obs_data_get_int(backup, "applied_ms")) {
				assign_field(data, backup, "transition", false);
				assign_field(data, backup, "transition_duration", true);
			}
			obs_data_erase(data, "stagekit.table.override");
		}
		if (rule.name.empty())
			continue;
		backup = obs_data_create();
		assign_field(backup, data, "transition", false);
		assign_field(backup, data, "transition_duration", true);
		obs_data_set_string(backup, "applied", rule.name.c_str());
		obs_data_set_int(backup, "applied_ms", rule.ms);
		obs_data_set_obj(data, "stagekit.table.override", backup);
		obs_data_set_string(data, "transition", rule.name.c_str());
		obs_data_set_int(data, "transition_duration", rule.ms);
	}
	obs_frontend_source_list_free(&scenes);
}
void changed()
{
	if (context)
		QMetaObject::invokeMethod(
			context,
			[] {
				apply_rules();
				obs_frontend_save();
			},
			Qt::QueuedConnection);
}

bool decode(obs_data_t *data, Rules &result)
{
	OBSDataArrayAutoRelease list = obs_data_get_array(data, "transitions");
	if (!list)
		return false;
	Rules next;
	for (size_t i = 0; i < obs_data_array_count(list); ++i) {
		OBSDataAutoRelease row = obs_data_array_item(list, i);
		Pair pair{obs_data_get_string(row, "from_scene"), obs_data_get_string(row, "to_scene")};
		Rule rule{obs_data_get_string(row, "transition"), obs_data_get_int(row, "duration")};
		if (pair.first.empty() || pair.second.empty() || rule.name.empty() || rule.ms < 0)
			return false;
		next[pair] = rule;
	}
	result.swap(next);
	return true;
}

void read_matrix(obs_data_t *data, Rules &result)
{
	OBSDataArrayAutoRelease matrix = obs_data_get_array(data, "matrix");
	for (size_t i = 0; i < obs_data_array_count(matrix); ++i) {
		OBSDataAutoRelease row = obs_data_array_item(matrix, i);
		const std::string from = obs_data_get_string(row, "scene");
		OBSDataArrayAutoRelease columns = obs_data_get_array(row, "data");
		for (size_t j = 0; j < obs_data_array_count(columns); ++j) {
			OBSDataAutoRelease cell = obs_data_array_item(columns, j);
			const std::string to = obs_data_get_string(cell, "to"),
					  name = obs_data_get_string(cell, "transition");
			if (!from.empty() && !to.empty() && !name.empty())
				result[{from, to}] = {name, obs_data_get_int(cell, "duration")};
		}
	}
}

void encode(obs_data_t *data)
{
	OBSDataArrayAutoRelease list = obs_data_array_create();
	for (const auto &[pair, rule] : snapshot()) {
		OBSDataAutoRelease row = obs_data_create();
		obs_data_set_string(row, "from_scene", pair.first.c_str());
		obs_data_set_string(row, "to_scene", pair.second.c_str());
		obs_data_set_string(row, "transition", rule.name.c_str());
		obs_data_set_int(row, "duration", rule.ms);
		obs_data_array_push_back(list, row);
	}
	obs_data_set_array(data, "transitions", list);
}

void persist(obs_data_t *root, bool save, void *)
{
	if (save) {
		OBSDataAutoRelease data = obs_data_create();
		encode(data);
		obs_data_set_int(data, "dialog_width", dialog_size.width());
		obs_data_set_int(data, "dialog_height", dialog_size.height());
		obs_data_set_bool(data, "enabled", enabled);
		obs_data_array_t *on = nullptr, *off = nullptr;
		obs_hotkey_pair_save(keys, &on, &off);
		if (on)
			obs_data_set_array(data, "enable_hotkey", on);
		if (off)
			obs_data_set_array(data, "disable_hotkey", off);
		obs_data_array_release(on);
		obs_data_array_release(off);
		obs_data_set_obj(root, "stagekit.table", data);
	} else {
		OBSDataAutoRelease data = obs_data_get_obj(root, "stagekit.table");
		Rules next;
		if (data) {
			decode(data, next);
			const int width = (int)obs_data_get_int(data, "dialog_width"),
				  height = (int)obs_data_get_int(data, "dialog_height");
			if (width > 0 && height > 0)
				dialog_size = {width, height};
		} else {
			OBSDataAutoRelease old = obs_data_get_obj(root, "obs-transition-matrix");
			read_matrix(old, next);
		}
		enabled = !obs_data_has_user_value(data, "enabled") || obs_data_get_bool(data, "enabled");
		OBSDataArrayAutoRelease on = obs_data_get_array(data, "enable_hotkey"),
					off = obs_data_get_array(data, "disable_hotkey");
		obs_hotkey_pair_load(keys, on, off);
		std::lock_guard lock(model);
		rules.swap(next);
	}
}

QStringList names(bool transitions)
{
	obs_frontend_source_list items{};
	if (transitions)
		obs_frontend_get_transitions(&items);
	else
		obs_frontend_get_scenes(&items);
	QStringList result;
	for (size_t i = 0; i < items.sources.num; ++i)
		result.append(QString::fromUtf8(obs_source_get_name(items.sources.array[i])));
	obs_frontend_source_list_free(&items);
	return result;
}

bool set_rule(const Pair &pair, const Rule &rule)
{
	if (pair.first.empty() || pair.second.empty() || rule.ms < 0)
		return false;
	std::lock_guard lock(model);
	if (rule.name.empty())
		return rules.erase(pair) != 0;
	rules[pair] = rule;
	return true;
}

void editor()
{
	QDialog dialog(static_cast<QWidget *>(obs_frontend_get_main_window()));
	dialog.setWindowTitle(text("Ports.Tool.table"));
	auto *layout = new QVBoxLayout(&dialog);
	auto *active = new QCheckBox(text("Native.EnableTable"), &dialog);
	active->setChecked(enabled);
	layout->addWidget(active);
	auto *list = new QTableWidget(0, 4, &dialog);
	list->setObjectName("nativeTransitionRules");
	list->setHorizontalHeaderLabels({text("Native.FromScene"), text("Native.ToScene"), text("Native.Transition"),
					 text("Native.DurationMs")});
	list->setSelectionBehavior(QAbstractItemView::SelectRows);
	list->setEditTriggers(QAbstractItemView::NoEditTriggers);
	list->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	layout->addWidget(list);
	auto refresh = [&] {
		list->setRowCount(0);
		for (const auto &[pair, value] : snapshot()) {
			const auto row = list->rowCount();
			list->insertRow(row);
			const QStringList cells{QString::fromStdString(pair.first), QString::fromStdString(pair.second),
						QString::fromStdString(value.name), QString::number(value.ms)};
			for (int col = 0; col < 4; ++col)
				list->setItem(row, col, new QTableWidgetItem(cells.at(col)));
		}
	};
	refresh();
	auto *inputs = new QHBoxLayout;
	layout->addLayout(inputs);
	auto combo = [&](QStringList options) {
		auto *box = new QComboBox(&dialog);
		box->setEditable(true);
		box->addItems(options);
		inputs->addWidget(box);
		return box;
	};
	auto scenes = names(false);
	scenes.prepend("Any");
	auto *from = combo(scenes), *to = combo(scenes), *transition = combo(names(true));
	from->setObjectName("nativeRuleFrom");
	to->setObjectName("nativeRuleTo");
	transition->setObjectName("nativeRuleTransition");
	auto *ms = new QSpinBox(&dialog);
	ms->setRange(0, 3600000);
	ms->setValue(300);
	ms->setSuffix(" ms");
	inputs->addWidget(ms);
	QObject::connect(list, &QTableWidget::currentCellChanged, &dialog, [&](int r) {
		if (r < 0 || !list->item(r, 3))
			return;
		from->setCurrentText(list->item(r, 0)->text());
		to->setCurrentText(list->item(r, 1)->text());
		transition->setCurrentText(list->item(r, 2)->text());
		ms->setValue(list->item(r, 3)->text().toInt());
	});
	QObject::connect(active, &QCheckBox::toggled, &dialog, [](bool state) {
		enabled = state;
		changed();
	});
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto button = [&](const char *label, auto fn) {
		auto *btn = buttons->addButton(text(label), QDialogButtonBox::ActionRole);
		QObject::connect(btn, &QPushButton::clicked, &dialog, fn);
		return btn;
	};
	auto key = [&] {
		return Pair{from->currentText().toStdString(), to->currentText().toStdString()};
	};
	button("Native.SetRule", [&] {
		if (!transition->currentText().isEmpty() &&
		    set_rule(key(), {transition->currentText().toStdString(), ms->value()})) {
			refresh();
			changed();
		}
	})->setObjectName("nativeSetRule");
	button("Native.Remove", [&] {
		if (set_rule(key(), {})) {
			refresh();
			changed();
		}
	});
	button("Native.Export", [&] {
		const auto path = QFileDialog::getSaveFileName(&dialog, text("Native.Export"), {}, "JSON (*.json)");
		if (path.isEmpty())
			return;
		OBSDataAutoRelease data = obs_data_create();
		encode(data);
		const QByteArray payload(obs_data_get_json(data));
		QSaveFile file(path);
		if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit())
			QMessageBox::warning(&dialog, dialog.windowTitle(), file.errorString());
	});
	button("Native.Import", [&] {
		const auto path = QFileDialog::getOpenFileName(&dialog, text("Native.Import"), {}, "JSON (*.json)");
		if (path.isEmpty())
			return;
		OBSDataAutoRelease data = obs_data_create_from_json_file(path.toUtf8().constData());
		Rules input;
		if (!decode(data, input)) {
			QMessageBox::warning(&dialog, dialog.windowTitle(), text("Native.InvalidRules"));
			return;
		}
		{
			std::lock_guard lock(model);
			for (const auto &[pair, rule] : input)
				rules[pair] = rule;
		}
		refresh();
		changed();
	});
	QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	layout->addWidget(buttons);
	dialog.resize(dialog_size);
	dialog.exec();
	dialog_size = dialog.size();
}
void renamed(void *, calldata_t *cd)
{
	auto *source = static_cast<obs_source_t *>(calldata_ptr(cd, "source"));
	if (!source)
		return;
	const bool scene = obs_scene_from_source(source) != nullptr;
	if (!scene && obs_source_get_type(source) != OBS_SOURCE_TYPE_TRANSITION)
		return;
	const std::string before = calldata_string(cd, "prev_name"), after = calldata_string(cd, "new_name");
	std::lock_guard lock(model);
	Rules next;
	for (const auto &[pair, rule] : rules) {
		Pair key = pair;
		Rule value = rule;
		if (scene && before != "Any") {
			if (key.first == before)
				key.first = after;
			if (key.second == before)
				key.second = after;
		} else if (!scene && value.name == before)
			value.name = after;
		next[key] = value;
	}
	rules.swap(next);
}
void event(obs_frontend_event which, void *)
{
	if (which == OBS_FRONTEND_EVENT_SCENE_CHANGED || which == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED ||
	    which == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		apply_rules();
	else if (which == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP || which == OBS_FRONTEND_EVENT_EXIT) {
		std::lock_guard lock(model);
		rules.clear();
	}
}
bool toggle(void *value, obs_hotkey_pair_id, obs_hotkey_t *, bool pressed)
{
	bool wanted = value != nullptr;
	if (!pressed || enabled.exchange(wanted) == wanted)
		return false;
	changed();
	return true;
}
void load()
{
	context = new QObject(static_cast<QWidget *>(obs_frontend_get_main_window()));
	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(text("Ports.Tool.table")));
	QObject::connect(action, &QAction::triggered, context, editor);
	keys = obs_hotkey_pair_register_frontend("stagekit.port.transition-table.enable", text("Native.EnableTable"),
						 "stagekit.port.transition-table.disable", text("Native.DisableTable"),
						 toggle, toggle, reinterpret_cast<void *>(1), nullptr);
	obs_frontend_add_save_callback(persist, nullptr);
	obs_frontend_add_event_callback(event, nullptr);
	signal_handler_connect(obs_get_signal_handler(), "source_rename", renamed, nullptr);
	proc_handler_add(
		obs_get_proc_handler(),
		"void stagekit_port_get_transition_table_transition(string from_scene, string to_scene, out string transition, out int duration)",
		[](void *, calldata_t *cd) {
			const char *from = calldata_string(cd, "from_scene"), *to = calldata_string(cd, "to_scene");
			const auto match = pick(snapshot(), {from ? from : "", to ? to : ""});
			calldata_set_string(cd, "transition", match.name.c_str());
			calldata_set_int(cd, "duration", match.ms);
		},
		nullptr);
}
void post()
{
	vendor = obs_websocket_register_vendor("stagekit-port-transition-table");
	if (!vendor)
		return;
	obs_websocket_vendor_register_request(
		vendor, "get_table",
		[](obs_data_t *, obs_data_t *out, void *) {
			encode(out);
			obs_data_set_bool(out, "success", true);
		},
		nullptr);
	obs_websocket_vendor_register_request(
		vendor, "get_transition",
		[](obs_data_t *in, obs_data_t *out, void *) {
			const auto match = pick(snapshot(), {obs_data_get_string(in, "from_scene"),
							     obs_data_get_string(in, "to_scene")});
			obs_data_set_string(out, "transition", match.name.c_str());
			obs_data_set_int(out, "duration", match.ms);
			obs_data_set_bool(out, "success", true);
		},
		nullptr);
	obs_websocket_vendor_register_request(
		vendor, "set_transition",
		[](obs_data_t *in, obs_data_t *out, void *) {
			bool ok = set_rule({obs_data_get_string(in, "from_scene"), obs_data_get_string(in, "to_scene")},
					   {obs_data_get_string(in, "transition"), obs_data_get_int(in, "duration")});
			obs_data_set_bool(out, "success", ok);
			if (ok)
				changed();
			else
				obs_data_set_string(out, "error", "Invalid or missing transition rule");
		},
		nullptr);
}
void unload()
{
	if (vendor)
		for (const char *name : {"get_table", "get_transition", "set_transition"})
			obs_websocket_vendor_unregister_request(vendor, name);
	signal_handler_disconnect(obs_get_signal_handler(), "source_rename", renamed, nullptr);
	obs_frontend_remove_save_callback(persist, nullptr);
	obs_frontend_remove_event_callback(event, nullptr);
	obs_hotkey_pair_unregister(keys);
	delete context;
	context = nullptr;
}
} // namespace
const Component component{load, unload, post};
} // namespace stagekit::native
