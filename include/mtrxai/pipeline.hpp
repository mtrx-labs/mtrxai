#pragma once

#include "mtrxai/config.hpp"
#include "mtrxai/provider.hpp"
#include "mtrxai/safety.hpp"
#include "mtrxai/textutil.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace mtrxai {

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

  // Continues a saved session. The request is read from request.md; completed
  // Chief and Prompter stages are reused without starting those models again.
  void resume(const std::filesystem::path& session, const Status& status,
              const Confirm& confirm,
              const InferenceSession::TokenSink& tokens = {});

  // Call from a UI thread to stop the current run cleanly: the active model
  // request is aborted and queued stages throw Cancelled.
  void request_cancel();

 private:
  void run_impl(const std::filesystem::path& resume_session,
                std::string request, const Status& status,
                const Confirm& confirm,
                const InferenceSession::TokenSink& tokens);

  Config& config_;
  std::atomic<bool> cancelled_{false};
  std::atomic<InferenceSession*> active_session_{nullptr};
};

}  // namespace mtrxai
