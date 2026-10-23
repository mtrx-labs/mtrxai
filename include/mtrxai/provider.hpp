#pragma once

#include "mtrxai/config.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mtrxai {

using Messages = std::vector<std::pair<std::string, std::string>>;

std::string generate(const ModelSlot& slot, const Messages& messages,
                     const std::function<void(const std::string&)>& status);

}  // namespace mtrxai
