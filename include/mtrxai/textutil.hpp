#pragma once

#include <string>

namespace mtrxai {

struct ThinkingResult {
  std::string content;
  std::string thinking;
};

// Extracts the most likely JSON object from a model reply. It accepts a bare
// object, a fenced code block, an optional leading "json" language marker, and
// surrounding prose. The returned text is not validated as JSON.
std::string extract_json(const std::string& output);

// Separates visible answer text from ` thinking...</think>` reasoning. Handles
// case-insensitive tags, multiple blocks, a missing opening tag (text before a
// stray closing tag is reasoning), and an unclosed block (the rest is
// reasoning). Both results are trimmed.
ThinkingResult extract_thinking(const std::string& text);

}  // namespace mtrxai
