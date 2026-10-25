#include "mtrxai/signals.hpp"

#include "mtrxai/cancel.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <atomic>

namespace mtrxai {
namespace signals {
namespace {

std::atomic<pid_t> g_active_child{0};

char g_ollama_host[128] = "127.0.0.1";
int g_ollama_port = 11434;
char g_ollama_model[128] = "";
std::atomic<bool> g_ollama_active{false};

// Async-signal-safe: only atomic stores. All cleanup (stopping children,
// unloading Ollama, restoring the terminal) happens in the normal exception and
// shutdown paths that observe the process token.
void on_signal(int signum) {
  note_process_signal(signum);
  process_cancel_token().cancel();
}

void parse_url(const std::string& url, char* host_out, std::size_t host_len,
               int* port_out) {
  std::string value = url;
  const std::string http = "http://";
  const std::string https = "https://";
  if (value.rfind(http, 0) == 0) value.erase(0, http.size());
  if (value.rfind(https, 0) == 0) value.erase(0, https.size());
  while (!value.empty() && value.back() == '/') value.pop_back();

  const std::size_t colon = value.find(':');
  if (colon != std::string::npos) {
    std::string host = value.substr(0, colon);
    if (host == "localhost") host = "127.0.0.1";
    std::snprintf(host_out, host_len, "%s", host.c_str());
    *port_out = std::atoi(value.substr(colon + 1).c_str());
  } else {
    std::string host = value;
    if (host == "localhost") host = "127.0.0.1";
    std::snprintf(host_out, host_len, "%s", host.c_str());
    *port_out = 11434;
  }
}

}  // namespace

void install() {
  struct sigaction action {};
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
}

void set_active_child(pid_t pid) { g_active_child.store(pid); }

void set_active_ollama(const std::string& base_url, const std::string& model) {
  parse_url(base_url, g_ollama_host, sizeof(g_ollama_host), &g_ollama_port);
  std::snprintf(g_ollama_model, sizeof(g_ollama_model), "%s", model.c_str());
  g_ollama_active.store(true);
}

void clear_active_ollama() {
  g_ollama_active.store(false);
  g_ollama_model[0] = '\0';
}

void trigger_signal(int signum) { on_signal(signum); }

}  // namespace signals
}  // namespace mtrxai
