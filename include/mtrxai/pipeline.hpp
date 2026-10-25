#pragma once

#include "mtrxai/cancel.hpp"
#include "mtrxai/config.hpp"
#include "mtrxai/events.hpp"
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

  // Event-driven entry points. They never throw for model, approval, or
  // cancellation failures; the caller gets RunResult and a final log note.
  RunResult run(const std::string& request, PipelineEvents& events,
                CancelToken& cancel);

  // Legacy callback API kept for the existing UI/tests. status, tokens, and
  // confirm are called from the worker thread; confirm must marshal to the
  // terminal thread. These overloads throw Cancelled/std::exception on failure.
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
  struct RunHooks;
  void run_impl(const std::filesystem::path& resume_session,
                std::string request, const Status& status,
                const Confirm& confirm,
                const InferenceSession::TokenSink& tokens);
  void run_impl_events(const std::filesystem::path& resume_session,
                       std::string request, RunHooks& hooks);

  Config& config_;
  std::atomic<bool> cancelled_{false};
  std::atomic<InferenceSession*> active_session_{nullptr};
};

}  // namespace mtrxai
