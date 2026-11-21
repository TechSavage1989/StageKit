#include "components.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <set>

namespace stagekit {
namespace {
std::set<std::string> loaded_components;
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
		if (port.needs_window && !frontend)
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
		if (obs_open_module(&module, path.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(module)) {
			blog(LOG_WARNING, "[stagekit] Could not start component %s", port.key);
			continue;
		}
		loaded_components.insert(port.key);
	}
}

} // namespace stagekit
