#include "mtrxai/signals.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
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

void parse_url(const std::string& url, char* host_out, std::size_t host_len, int* port_out) {
  std::string s = url;
  const std::string prefix = "http://";
  if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
  const std::string s_prefix = "https://";
  if (s.rfind(s_prefix, 0) == 0) s.erase(0, s_prefix.size());
  while (!s.empty() && s.back() == '/') s.pop_back();

  std::size_t colon = s.find(':');
  if (colon != std::string::npos) {
    std::string host = s.substr(0, colon);
    if (host == "localhost") host = "127.0.0.1";
    std::snprintf(host_out, host_len, "%s", host.c_str());
    *port_out = std::atoi(s.substr(colon + 1).c_str());
  } else {
    std::string host = s;
    if (host == "localhost") host = "127.0.0.1";
    std::snprintf(host_out, host_len, "%s", host.c_str());
    *port_out = 11434;
  }
}

void unload_ollama_raw() {
  if (!g_ollama_active.load() || g_ollama_model[0] == '\0') return;

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) return;

  struct timeval tv{};
  tv.tv_sec = 1;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(g_ollama_port));
  if (inet_pton(AF_INET, g_ollama_host, &addr.sin_addr) <= 0) {
    close(sock);
    return;
  }

  if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
    char body[512];
    int body_len = std::snprintf(body, sizeof(body),
                                 "{\"model\":\"%s\",\"keep_alive\":0}",
                                 g_ollama_model);
    char request[1024];
    int req_len = std::snprintf(request, sizeof(request),
                                "POST /api/generate HTTP/1.1\r\n"
                                "Host: %s:%d\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: %d\r\n"
                                "Connection: close\r\n\r\n"
                                "%s",
                                g_ollama_host, g_ollama_port, body_len, body);
    if (req_len > 0) {
      send(sock, request, static_cast<std::size_t>(req_len), 0);
    }
  }
  close(sock);
}

void on_signal(int signum) {
  // The TUI installs its own handlers on top of these: FTXUI restores the
  // terminal, then re-raises, so by the time this runs no screen cleanup is
  // needed here.

  // 1. Terminate any active child process group / child
  const pid_t child = g_active_child.load();
  if (child > 0) {
    kill(-child, SIGTERM);
    kill(child, SIGTERM);
  }

  // 2. Unload active Ollama model
  unload_ollama_raw();

  // 3. Exit non-zero
  _exit(128 + signum);
}

}  // namespace

void install() {
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

void set_active_child(pid_t pid) {
  g_active_child.store(pid);
}

void set_active_ollama(const std::string& base_url, const std::string& model) {
  parse_url(base_url, g_ollama_host, sizeof(g_ollama_host), &g_ollama_port);
  std::snprintf(g_ollama_model, sizeof(g_ollama_model), "%s", model.c_str());
  g_ollama_active.store(true);
}

void clear_active_ollama() {
  g_ollama_active.store(false);
  g_ollama_model[0] = '\0';
}

void trigger_signal(int signum) {
  on_signal(signum);
}

}  // namespace signals
}  // namespace mtrxai
