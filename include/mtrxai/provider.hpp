#pragma once

#include "mtrxai/cancel.hpp"
#include "mtrxai/config.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mtrxai {

using Messages = std::vector<std::pair<std::string, std::string>>;

// Thrown when the user requested cancellation while a request was running.
// It must not be treated like a provider failure.
class Cancelled : public std::runtime_error {
 public:
  explicit Cancelled(const std::string& what = "Cancelled by user.")
      : std::runtime_error(what) {}
};

std::filesystem::path llama_log_path(const std::string& role);

class InferenceSession {
 public:
  using Status = std::function<void(const std::string&)>;
  // Receives each decoded text chunk as the model streams it. Must not throw.
  using TokenSink = std::function<void(const std::string&)>;

  // The optional token aborts model loading and generation. The process-wide
  // SIGINT/SIGTERM token is always checked as well.
  explicit InferenceSession(const ModelSlot& slot, const Status& status,
                            const CancelToken* cancel = nullptr);
  ~InferenceSession() noexcept;
  InferenceSession(const InferenceSession&) = delete;
  InferenceSession& operator=(const InferenceSession&) = delete;

  // Generates the full reply (streaming is used internally; no sink attached).
  std::string generate(const Messages& messages);
  // Same request, but every text chunk is also passed to on_token.
  std::string generate(const Messages& messages, const TokenSink& on_token);

  // Request cancellation of an in-flight generate(); the running request
  // is aborted with Cancelled. Clear before starting the next request.
  void request_cancel();
  void clear_cancel();

  void release();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mtrxai
