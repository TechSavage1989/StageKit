#pragma once

#include <string>

namespace stagekit {
void load_components();
bool ndi_requested();
bool set_ndi_requested(bool enabled);
bool component_loaded(const std::string &key);
} // namespace stagekit
