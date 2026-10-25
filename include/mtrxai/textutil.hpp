#pragma once

#include <string>

namespace mtrxai {

// Extracts the most likely JSON object from a model reply. It accepts a bare
// object, a fenced code block, an optional leading "json" language marker, and
// surrounding prose. The returned text is not validated as JSON.
std::string extract_json(const std::string& output);

}  // namespace mtrxai
