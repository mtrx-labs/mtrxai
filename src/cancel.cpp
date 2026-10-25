#include "mtrxai/cancel.hpp"

namespace mtrxai {
namespace {

std::atomic<int> g_process_signal{0};

}  // namespace

CancelToken& process_cancel_token() noexcept {
  // Function-local static construction is done before any signal handler is
  // installed, so the handler only touches an already-valid atomic.
  static CancelToken token;
  return token;
}

void note_process_signal(int signum) noexcept {
  g_process_signal.store(signum, std::memory_order_relaxed);
}

int process_signal() noexcept {
  return g_process_signal.load(std::memory_order_relaxed);
}

}  // namespace mtrxai
