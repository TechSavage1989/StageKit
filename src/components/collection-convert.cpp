#include "collection-convert.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>

namespace stagekit {
namespace {
using Json = nlohmann::json;
using Path = std::filesystem::path;
using Map = std::map<std::string, std::string>;

std::string string_at(const Json &obj, const char *key)
{
	auto it = obj.find(key);
	return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string utf8_path(const Path &path)
{
	const auto value = path.generic_u8string();
	return {value.begin(), value.end()};
}

bool bundled_shader(const std::string &saved, Path &relative)
{
	std::string path = saved;
	std::replace(path.begin(), path.end(), '\\', '/');
	std::string lower = path;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
		return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
	});
	for (const char *marker : {"/obs-shaderfilter/examples/", "/obs-shaderfilter/data/examples/"}) {
		const size_t pos = lower.rfind(marker);
		if (pos == std::string::npos)
			continue;
		relative = std::filesystem::u8path(path.substr(pos + std::string(marker).size()));
		if (relative.empty() || relative.is_absolute() || relative.has_root_name())
			return false;
		return std::find(relative.begin(), relative.end(), Path("..")) == relative.end();
	}
	return false;
}

bool identical(const Path &a, const Path &b)
{
	std::error_code ec;
	const auto size = std::filesystem::file_size(a, ec);
	if (ec || std::filesystem::file_size(b, ec) != size || ec)
		return false;
	std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
	return left && right && std::equal(std::istreambuf_iterator<char>(left), {}, std::istreambuf_iterator<char>(right));
}

void shader_file(Json &settings, const Path &data, CollectionReport &report)
{
	if (!settings.is_object())
		return;
	const std::string saved = string_at(settings, "shader_file_name");
	Path relative;
	if (!bundled_shader(saved, relative))
		return;
	const Path next = data / "components" / "shader" / "examples" / relative;
	const Path old = std::filesystem::u8path(saved);
	std::error_code ec;
	if (!std::filesystem::is_regular_file(next, ec) || ec) {
		++report.review["Missing bundled shader: " + utf8_path(relative)];
		return;
	}
	if (std::filesystem::is_regular_file(old, ec) && !identical(old, next)) {
		++report.review["Customized shader kept in its original folder: " + utf8_path(relative)];
		return;
	}
	settings["shader_file_name"] = utf8_path(next);
	++report.relocated;
}

bool built_in(const std::string &id)
{
	static const std::set<std::string> types = {
		"color_filter", "color_key_filter", "chroma_key_filter", "mask_filter", "clut_filter",
		"crop_filter", "gpu_delay", "luma_key_filter", "scale_filter", "scroll_filter",
		"sharpness_filter", "hdr_tonemap_filter", "sdr_hdr_filter", "async_delay_filter",
		"compressor_filter", "limiter_filter", "noise_suppress_filter", "noise_gate_filter",
		"expander_filter", "gain_filter", "invert_polarity_filter", "upward_compressor_filter",
		"scene", "group", "image_source", "color_source", "browser_source", "ffmpeg_source",
		"monitor_capture", "game_capture", "window_capture", "dshow_input", "text_gdiplus", "text_ft2_source",
		"wasapi_input_capture", "wasapi_output_capture", "wasapi_process_output_capture",
		"fade_transition", "cut_transition", "slide_transition", "obs_stinger_transition",
		"fade_to_color_transition", "wipe_transition", "swipe_transition", "luma_wipe_transition",
	};
	return id.rfind("stagekit_", 0) == 0 || types.count(id) != 0;
}

void convert_source(Json &source, const Map &mapping, const Path &data, CollectionReport &report)
{
	if (!source.is_object())
		return;
	const std::string id = string_at(source, "id");
	auto match = mapping.find(id);
	if (match != mapping.end()) {
		const std::string version = string_at(source, "versioned_id");
		if (version.empty() || version == id || version.rfind(id + "_v", 0) == 0) {
			source["id"] = match->second;
			if (!version.empty())
				source["versioned_id"] = match->second + version.substr(id.size());
			if (id == "shader_filter" && source.contains("settings"))
				shader_file(source["settings"], data, report);
			++report.converted;
		} else {
			++report.review[id + " / " + version];
		}
	} else if (!id.empty() && !built_in(id)) {
		++report.review[id];
	}
	auto children = source.find("filters");
	if (children != source.end() && children->is_array())
		for (auto &filter : *children)
			convert_source(filter, mapping, data, report);
}

void rename_key(Json &obj, const std::string &from, const std::string &to)
{
	if (!obj.is_object() || !obj.contains(from) || obj.contains(to))
		return;
	obj[to] = obj.at(from);
	obj.erase(from);
}
} // namespace

CollectionReport convert_collection(Json &collection, const Json &catalog, const Path &resource_dir)
{
	if (!collection.is_object() || !collection.contains("sources") || !collection["sources"].is_array())
		throw std::invalid_argument("Expected a saved OBS scene collection.");
	Map mapping;
	for (auto it = catalog.begin(); it != catalog.end(); ++it) {
		const auto &port = it.value();
		if (port.contains("types"))
			for (const auto &type : port.at("types")) {
				const auto old = type.at(0).get<std::string>();
				mapping[old] = "stagekit_port_" + old;
			}
	}
	for (auto &[old, type] : catalog.at("native").at("ids").items())
		mapping[old] = type.get<std::string>();
	CollectionReport report;
	for (auto &[name, value] : collection.items()) {
		if ((name == "sources" || name == "groups" || name == "transitions") && value.is_array()) {
			for (auto &source : value)
				convert_source(source, mapping, resource_dir, report);
		} else if (value.is_object() && value.contains("id") && value.contains("settings") &&
			   value.contains("name")) {
			convert_source(value, mapping, resource_dir, report);
		}
	}
	auto modules = collection.find("modules");
	if (modules != collection.end()) {
		rename_key(*modules, "quick_access_utility", "stagekit.quick_access");
		rename_key(*modules, "source-dock", "stagekit.dock");
		rename_key(*modules, "transition-table", "stagekit.table");
		for (const char *key : {"copyTransformHotkey", "pasteTransformHotkey", "sceneCollectionManagerHotkey",
					"backupHotkey", "loadLastBackupHotkey", "loadFirstBackupHotkey"})
			rename_key(*modules, key, std::string("stagekit.port.") + key);
	}
	return report;
}
} // namespace stagekit
