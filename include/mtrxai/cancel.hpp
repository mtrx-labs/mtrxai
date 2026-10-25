#pragma once

#include <atomic>

namespace mtrxai {

// A cooperative cancellation flag. It is intentionally just an atomic boolean:
// signal handlers may call cancel(), and worker loops poll cancelled() between
// blocking operations. Checking it never allocates or takes a lock.
class CancelToken {
 public:
  CancelToken() = default;
  ~CancelToken() = default;
  CancelToken(const CancelToken&) = delete;
  CancelToken& operator=(const CancelToken&) = delete;

  void cancel() noexcept {
    cancelled_.store(true, std::memory_order_relaxed);
  }

  bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancelled_{false};
};

// Token set by the SIGINT/SIGTERM handler. The handler itself only sets this
// flag; the normal code path stops children, unloads models, and restores the
// terminal before exiting.
CancelToken& process_cancel_token() noexcept;

// Signal number recorded by the handler, or 0. Kept separately because the
// cancellation token itself only needs a boolean.
void note_process_signal(int signum) noexcept;
int process_signal() noexcept;

}  // namespace mtrxai
