#include "../src/components/collection-convert.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;

void require(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(message);
}

Json read_json(const Path &path)
{
	std::ifstream file(path, std::ios::binary);
	return Json::parse(file);
}
} // namespace

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	try {
		const auto catalog = read_json(argv[1]);
		const Path resources = std::filesystem::absolute(argv[2]);
		if (argc > 3) {
			for (int i = 3; i < argc; ++i) {
				auto collection = read_json(std::filesystem::u8path(argv[i]));
				const auto report = stagekit::convert_collection(collection, catalog, resources);
				const auto converted = collection;
				const auto repeat = stagekit::convert_collection(collection, catalog, resources);
				require(converted == collection && repeat.converted == 0,
					"Second conversion changed the collection");
				std::cout << Path(argv[i]).filename().string() << " | converted " << report.converted
					  << " | shaders " << report.relocated << '\n';
				for (const auto &[id, count] : report.review)
					std::cout << "  " << count << " " << id << '\n';
			}
			return 0;
		}
		auto original = Json::parse(R"({
			"name":"Fixture", "unknown":9007199254740993,
			"sources":[{"id":"scene","name":"Keep this","filters":[
			  {"id":"move_source_filter","versioned_id":"move_source_filter","name":"Camera movement","enabled":false,
			   "settings":{"source":"Camera","next_move":"Switch","id":"freeze_filter"}},
			  {"id":"shader_filter","versioned_id":"shader_filter","settings":{
			   "shader_file_name":"C:/old/data/obs-plugins/obs-shaderfilter/examples/drop_shadow.shader","shadow_size":7}},
			  {"id":"filter-dropshadow","settings":{"offset_x":-13}},
			  {"id":"filter-blur","settings":{"size":7}},{"id":"color_filter","versioned_id":"color_filter_v2"},
			  {"id":"freeze_filter","versioned_id":"different_kind"}]},
			{"id":"source-clone","name":"Camera clone","uuid":"same-id","settings":{"source":"Camera"}}],
			"groups":[{"id":"group","filters":[{"id":"obs_composite_blur","versioned_id":"obs_composite_blur_v2"}]}],
			"transitions":[{"id":"move_transition","settings":{"duration":300}}],
			"AuxAudioDevice1":{"id":"wasapi_input_capture","name":"Mic","settings":{},"filters":[{"id":"move_audio_action_filter"}]},
			"modules":{"quick_access_utility":{"entries":[1]},"source-dock":{"old":1},"stagekit.dock":{"new":2},
			"advanced-scene-switcher":{"macros":[{"name":"kept"},{"name":"Folder","group":true}]}}
		})");
		auto collection = original;
		auto result = stagekit::convert_collection(collection, catalog, resources);
		const auto &filters = collection["sources"][0]["filters"];
		require(result.converted == 7 && result.relocated == 1, "Unexpected conversion count");
		require(result.review.count("filter-blur") && result.review.count("freeze_filter / different_kind"),
			"Unsupported IDs must be reported");
		require(collection["unknown"].get<uint64_t>() == 9007199254740993ULL, "64-bit value changed");
		require(collection["sources"][1]["uuid"] == "same-id" &&
				collection["sources"][1]["id"] == "stagekit_port_source-clone",
			"Clone identity changed");
		require(filters.at(0).at("name") == "Camera movement" && filters.at(0).at("enabled") == false &&
				filters.at(0).at("settings") == original["sources"][0]["filters"][0]["settings"],
			"Move filter settings changed");
		require(filters.at(1).at("settings")
					.at("shader_file_name")
					.get<std::string>()
					.find("/components/shader/examples/drop_shadow.shader") != std::string::npos,
			"Shader path was not repaired");
		require(filters.at(3) == original["sources"][0]["filters"][3] &&
				filters.at(4) == original["sources"][0]["filters"][4],
			"Unmapped filter changed");
		require(collection["groups"][0]["filters"][0]["versioned_id"] == "stagekit_port_obs_composite_blur_v2",
			"Version suffix was lost");
		require(collection["modules"]["stagekit.quick_access"]["entries"][0] == 1 &&
				collection["modules"].contains("source-dock") &&
				collection["modules"]["stagekit.dock"]["new"] == 2,
			"Existing utility settings overwritten");
		require(collection["modules"]["advanced-scene-switcher"] ==
					original["modules"]["advanced-scene-switcher"] &&
				result.review.at("Advanced Scene Switcher macros") == 1,
			"Macros must be preserved and reported");
		auto with_registry = original;
		const std::set<std::string> installed = {"stagekit_port_move_source_filter", "stagekit_port_obs_composite_blur"};
		result = stagekit::convert_collection(with_registry, catalog, resources, &installed);
		require(result.converted == 1 &&
				with_registry["sources"][0]["filters"][0]["id"] == "stagekit_port_move_source_filter",
			"Installed filter was not converted");
		require(with_registry["groups"] == original["groups"] &&
				with_registry["sources"][0]["filters"][1] == original["sources"][0]["filters"][1] &&
				result.review.count("obs_composite_blur / obs_composite_blur_v2") && result.review.count("shader_filter"),
			"Missing types or versions must retain their original settings and be reported");
		const auto after = collection;
		require(stagekit::convert_collection(collection, catalog, resources).converted == 0 &&
				collection == after,
			"Conversion must be idempotent");
		auto temp = std::filesystem::temp_directory_path() /
			    ("stagekit-filter-test-" +
			     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(temp / "obs-shaderfilter/examples");
		auto path = temp / "obs-shaderfilter/examples/drop_shadow.shader";
		std::ofstream(path) << "user shader";
		collection = original;
		collection["sources"][0]["filters"][1]["settings"]["shader_file_name"] = path.generic_string();
		result = stagekit::convert_collection(collection, catalog, resources);
		require(result.relocated == 0 &&
				collection["sources"][0]["filters"][1]["settings"]["shader_file_name"] ==
					path.generic_string(),
			"Custom shader was overwritten");
		std::filesystem::remove(path);
		std::filesystem::remove(temp / "obs-shaderfilter/examples");
		std::filesystem::remove(temp / "obs-shaderfilter");
		std::filesystem::remove(temp);
		bool invalid = false;
		try {
			auto input = Json::array();
			stagekit::convert_collection(input, catalog, resources);
		} catch (const std::invalid_argument &) {
			invalid = true;
		}
		require(invalid, "Invalid collection accepted");
		std::cout << "Collection conversion tests passed\n";
		return 0;
	} catch (const std::exception &e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
