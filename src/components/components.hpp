#pragma once

#include <string>

namespace stagekit {
void load_components();
bool component_loaded(const std::string &key);
}
