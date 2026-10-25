#include "mtrxai/textutil.hpp"

namespace mtrxai {

std::string extract_json(const std::string& output) {
  const std::size_t fence = output.find("```");
  std::string candidate = output;
  if (fence != std::string::npos) {
    const std::size_t first_line = output.find('\n', fence);
    const std::size_t last_fence = output.find("```", fence + 3);
    if (first_line != std::string::npos && last_fence > first_line) {
      candidate = output.substr(first_line + 1, last_fence - first_line - 1);
    }
  }
  const auto first = candidate.find_first_not_of(" \t\r\n");
  const auto last = candidate.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) return candidate;
  candidate = candidate.substr(first, last - first + 1);
  if (candidate.rfind("json\n", 0) == 0) candidate.erase(0, 5);
  if (!candidate.empty() && candidate.front() == '{') return candidate;

  const std::size_t object_start = candidate.find('{');
  const std::size_t object_end = candidate.rfind('}');
  if (object_start != std::string::npos && object_end > object_start) {
    return candidate.substr(object_start, object_end - object_start + 1);
  }
  return candidate;
}

}  // namespace mtrxai
