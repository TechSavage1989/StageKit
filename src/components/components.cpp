#include "components.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <set>

namespace stagekit {
namespace {
std::set<std::string> loaded_components;
constexpr auto preference = "LoadNDI";
bool previous_ndi(config_t *config)
{
	char *raw = obs_module_config_path("../../");
	if (!raw)
		return false;
	const QDir obs_config(QDir::cleanPath(QString::fromUtf8(raw)));
	bfree(raw);
	QFile records(obs_config.filePath("plugin_manager/modules.json"));
	if (records.open(QIODevice::ReadOnly)) {
		QByteArray saved = records.readAll();
		if (saved.startsWith("\xEF\xBB\xBF"))
			saved.remove(0, 3);
		const auto modules = QJsonDocument::fromJson(saved).array();
		for (const auto &item : modules) {
			const auto module = item.toObject();
			if (module.value("module_name").toString() != "stagekit-port-ndi")
				continue;
			if (!module.value("enabled").toBool(true))
				return false;
			for (const char *field : {"sources", "outputs"})
				for (const auto id : module.value(field).toArray())
					if (id.toString().startsWith("stagekit_port_ndi_") ||
					    id.toString() == "stagekit_port_ndi_audiofilter")
						return true;
		}
	}
	const char *active = config_get_string(config, "Basic", "SceneCollectionFile");
	if (!active || !*active)
		return false;
	const QString name = QString::fromUtf8(active);
	if (QFileInfo(name).fileName() != name)
		return false;
	QFile scene(obs_config.filePath("basic/scenes/" + name + ".json"));
	if (!scene.open(QIODevice::ReadOnly))
		return false;
	QByteArray tail;
	while (!scene.atEnd()) {
		const auto chunk = scene.read(65536);
		if (chunk.isEmpty())
			break;
		tail += chunk;
		if (tail.contains("\"stagekit_port_ndi_"))
			return true;
		tail = tail.right(32);
	}
	return false;
}
} // namespace

bool ndi_requested()
{
	if (!obs_frontend_get_main_window())
		return false;
	auto *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return false;
	if (config_has_user_value(cfg, "StageKit", preference))
		return config_get_bool(cfg, "StageKit", preference);
	static const bool previous = previous_ndi(cfg);
	return previous;
}

bool set_ndi_requested(bool enabled)
{
	auto *cfg = obs_frontend_get_user_config();
	if (!cfg)
		return false;
	const bool had = config_has_user_value(cfg, "StageKit", preference);
	const bool previous = config_get_bool(cfg, "StageKit", preference);
	config_set_bool(cfg, "StageKit", preference, enabled);
	if (config_save_safe(cfg, "tmp", "bak") == CONFIG_SUCCESS)
		return true;
	if (had)
		config_set_bool(cfg, "StageKit", preference, previous);
	else
		config_remove_value(cfg, "StageKit", preference);
	return false;
}

bool component_loaded(const std::string &key)
{
	return loaded_components.count(key) != 0;
}

void load_components()
{
	const bool frontend = obs_frontend_get_main_window() != nullptr;
	const char *resources = obs_get_module_data_path(obs_current_module());
	if (!resources)
		return;
	const std::string root = resources;
	struct Port {
		const char *key;
		bool needs_window;
	};
	static const Port ports[] = {
		{"3d", false},        {"masks", false},  {"composite", false},  {"retro", false},
		{"noise", false},     {"pixel", false},  {"delay", false},      {"freeze", false},
		{"recursion", false}, {"warp", false},   {"directory", false},  {"move", false},
		{"clone", false},     {"shader", false}, {"defaults", false},   {"copy", true},
		{"dock", true},       {"table", true},   {"collections", true}, {"quick", true},
		{"layouts", true},    {"ndi", true},
	};
	for (const Port &port : ports) {
		if ((port.needs_window && !frontend) || (std::string(port.key) == "ndi" && !ndi_requested()))
			continue;
		const std::string data = root + "/components/" + port.key;
#ifdef __APPLE__
		const std::string location = root + "/../Frameworks";
#else
		const std::string &location = data;
#endif
		std::string path = location + "/stagekit-port-" + port.key + SK_PORT_EXTENSION;
		if (!os_file_exists(path.c_str())) {
			std::string binary = obs_get_module_binary_path(obs_current_module());
			const auto slash = binary.find_last_of("/\\");
			if (slash != std::string::npos)
				path = binary.substr(0, slash) + "/stagekit-ports/" SK_PORT_CONFIG "/stagekit-port-" +
				       port.key + SK_PORT_EXTENSION;
		}
		std::replace(path.begin(), path.end(), '\\', '/');
		obs_module_t *module = nullptr;
		if (obs_open_module(&module, path.c_str(), data.c_str()) != MODULE_SUCCESS ||
		    !obs_init_module(module)) {
			blog(LOG_WARNING, "[stagekit] Could not start component %s", port.key);
			continue;
		}
		loaded_components.insert(port.key);
	}
}

} // namespace stagekit
