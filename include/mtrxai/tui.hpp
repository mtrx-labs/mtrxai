#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>

namespace mtrxai {

int run_tui(Config& config, const std::filesystem::path& config_file);

}  // namespace mtrxai
