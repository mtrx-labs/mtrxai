#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace mtrxai {

// Returns true if filename matches sensitive file names or patterns that must
// not be read, listed, or written by the automated coder.
bool sensitive_filename(const std::string& name);

struct ThinkingResult {
  std::string content;
  std::string thinking;
};

// Strips <think>...</think> blocks from text and returns both the clean content
// and any extracted thinking/reasoning text.
ThinkingResult extract_thinking(const std::string& text);

class Pipeline {
 public:
  using Status = std::function<void(const std::string&)>;
  using Confirm = std::function<bool(const std::string&)>;

  explicit Pipeline(Config& config);
  void run(const std::string& request, const Status& status,
           const Confirm& confirm);

 private:
  Config& config_;
};

}  // namespace mtrxai
