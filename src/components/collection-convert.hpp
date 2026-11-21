#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <nlohmann/json.hpp>

namespace stagekit {
struct CollectionReport {
	size_t converted = 0;
	size_t relocated = 0;
	std::map<std::string, size_t> review;
};

CollectionReport convert_collection(nlohmann::json &collection, const nlohmann::json &catalog,
				    const std::filesystem::path &resource_dir);
} // namespace stagekit
