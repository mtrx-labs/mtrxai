#pragma once

#include <string>

namespace mtrxai {

// Validate an HTTP provider endpoint without changing its destination.
void validate_provider_endpoint(const std::string& url);

}  // namespace mtrxai
