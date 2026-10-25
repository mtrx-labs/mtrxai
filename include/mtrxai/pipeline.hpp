#pragma once

#include "mtrxai/config.hpp"
#include "mtrxai/provider.hpp"

#include <atomic>
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
  // Runs the request on the caller's thread. status and tokens are called
  // from that thread while it works; confirm must marshal to a thread that
  // owns the terminal. tokens receives each streamed text chunk.
  void run(const std::string& request, const Status& status,
           const Confirm& confirm, const InferenceSession::TokenSink& tokens = {});

  // Call from a UI thread to stop the current run cleanly: the active model
  // request is aborted and queued stages throw Cancelled.
  void request_cancel();

 private:
  Config& config_;
  std::atomic<bool> cancelled_{false};
  std::atomic<InferenceSession*> active_session_{nullptr};
};

}  // namespace mtrxai
