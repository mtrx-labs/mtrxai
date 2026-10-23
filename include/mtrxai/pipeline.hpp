#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace mtrxai {

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
