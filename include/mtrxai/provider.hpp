#pragma once

#include "mtrxai/config.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mtrxai {

using Messages = std::vector<std::pair<std::string, std::string>>;

std::filesystem::path llama_log_path(const std::string& role);

class InferenceSession {
 public:
  using Status = std::function<void(const std::string&)>;

  InferenceSession(const ModelSlot& slot, const Status& status);
  ~InferenceSession() noexcept;
  InferenceSession(const InferenceSession&) = delete;
  InferenceSession& operator=(const InferenceSession&) = delete;

  std::string generate(const Messages& messages);
  void release();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mtrxai
