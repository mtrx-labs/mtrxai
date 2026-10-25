#include "mtrxai/textutil.hpp"

#include <algorithm>
#include <cctype>

namespace mtrxai {
namespace {

std::size_t find_case_insensitive(const std::string& haystack,
                                  const std::string& needle,
                                  std::size_t start) {
  if (needle.empty() || start >= haystack.size()) return std::string::npos;
  const auto found = std::search(
      haystack.begin() + static_cast<std::ptrdiff_t>(start), haystack.end(),
      needle.begin(), needle.end(), [](char left, char right) {
        return std::tolower(static_cast<unsigned char>(left)) ==
               std::tolower(static_cast<unsigned char>(right));
      });
  if (found == haystack.end()) return std::string::npos;
  return static_cast<std::size_t>(std::distance(haystack.begin(), found));
}

std::string trim(const std::string& value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

void append_block(std::string& destination, const std::string& block) {
  const std::string trimmed = trim(block);
  if (trimmed.empty()) return;
  if (!destination.empty()) destination += "\n\n";
  destination += trimmed;
}

}  // namespace

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

ThinkingResult extract_thinking(const std::string& text) {
  ThinkingResult result;
  // The canonical tags are <think> and </think>. Older sessions produced by
  // this project contain a leading space instead of "<", so accept that form
  // too when it is unambiguous.
  const std::string open_tag = "\x3cthink\x3e";
  const std::string legacy_open_tag = " thinking";
  const std::string close_tag = "\x3c/think\x3e";
  std::size_t position = 0;

  auto next_open = [&](std::size_t from) {
    const std::size_t canonical = find_case_insensitive(text, open_tag, from);
    const std::size_t legacy = find_case_insensitive(text, legacy_open_tag, from);
    if (legacy == std::string::npos) return canonical;
    if (canonical != std::string::npos && canonical <= legacy) return canonical;
    // Do not turn an ordinary sentence like "thinking about it" into a
    // reasoning block unless a closing tag follows or it starts the reply.
    if (from == 0 ||
        find_case_insensitive(text, close_tag, legacy) != std::string::npos) {
      return legacy;
    }
    return canonical;
  };

  while (position < text.size()) {
    const std::size_t open = next_open(position);
    const std::size_t close = find_case_insensitive(text, close_tag, position);

    // A closing tag without a matching opening one usually means the model
    // omitted the opening tag at the start. Treat the leading text as reasoning.
    if (close != std::string::npos &&
        (open == std::string::npos || close < open)) {
      if (result.content.empty() && result.thinking.empty()) {
        append_block(result.thinking, text.substr(position, close - position));
      } else {
        // A stray closing tag later in the answer is noise; drop it.
        result.content.append(text.substr(position, close - position));
      }
      position = close + close_tag.size();
      continue;
    }

    if (open == std::string::npos) {
      result.content.append(text.substr(position));
      break;
    }
    result.content.append(text.substr(position, open - position));
    const std::size_t open_length =
        open + open_tag.size() <= text.size() &&
                find_case_insensitive(text, open_tag, open) == open
            ? open_tag.size()
            : legacy_open_tag.size();
    const std::size_t thinking_start = open + open_length;
    const std::size_t thinking_end =
        find_case_insensitive(text, close_tag, thinking_start);
    if (thinking_end == std::string::npos) {
      // Unclosed block: everything after the opening tag is reasoning.
      append_block(result.thinking, text.substr(thinking_start));
      break;
    }
    append_block(result.thinking,
                 text.substr(thinking_start, thinking_end - thinking_start));
    position = thinking_end + close_tag.size();
  }

  result.content = trim(result.content);
  result.thinking = trim(result.thinking);
  return result;
}

}  // namespace mtrxai
