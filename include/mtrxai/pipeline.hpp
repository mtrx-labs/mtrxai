#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace mtrxai {

// Returns true if filename matches sensitive file names or patterns that must
// not be read, listed, or written by the automated coder.
bool sensitive_filename(const std::string& name);

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
