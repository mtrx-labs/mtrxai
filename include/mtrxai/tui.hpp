#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>

namespace mtrxai {

// Runs the interactive interface. When resume_session is set, that saved
// session starts immediately; otherwise the TUI opens at the task prompt.
int run_tui(Config& config, const std::filesystem::path& config_file,
            const std::filesystem::path& resume_session = {});

}  // namespace mtrxai
