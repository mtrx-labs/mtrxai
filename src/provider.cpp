#include "mtrxai/provider.hpp"
#include "mtrxai/endpoint.hpp"
#include "mtrxai/cancel.hpp"
#include "mtrxai/provider_stream.hpp"
#include "mtrxai/signals.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mtrxai {

std::filesystem::path llama_log_path(const std::string& role) {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  std::filesystem::path root;
  if (xdg != nullptr && *xdg != '\0') {
    root = std::filesystem::path(xdg) / "mtrxai" / "logs";
  } else {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
      throw std::runtime_error("HOME is not set; cannot locate logs directory");
    }
    root = std::filesystem::path(home) / ".local" / "state" / "mtrxai" / "logs";
  }
  return root / (role + ".log");
}

namespace {

using ChunkSink = InferenceSession::ChunkSink;
using RawSink = InferenceSession::RawSink;

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

struct HttpResponse {
  std::string body;
  long status = 0;
  bool exceeded_limit = false;
};

std::size_t append_body(char* data, std::size_t size, std::size_t count,
                        void* destination) {
  const std::size_t bytes = size * count;
  auto& response = *static_cast<HttpResponse*>(destination);
  constexpr std::size_t max_response = 4 * 1024 * 1024;
  if (bytes > max_response - response.body.size()) {
    response.exceeded_limit = true;
    return 0;
  }
  response.body.append(data, bytes);
  return bytes;
}

std::size_t discard_body(char*, std::size_t size, std::size_t count, void*) {
  return size * count;
}

struct StreamContext {
  std::function<void(const std::string&)> on_bytes;
  std::size_t received = 0;
  std::atomic<bool>* cancelled = nullptr;
  const CancelToken* external_cancel = nullptr;
  std::exception_ptr error;

  bool cancel_requested() const {
    return (cancelled != nullptr &&
            cancelled->load(std::memory_order_relaxed)) ||
           (external_cancel != nullptr && external_cancel->cancelled()) ||
           process_cancel_token().cancelled();
  }

};

std::size_t stream_bytes(char* data, std::size_t size, std::size_t count,
                         void* destination) {
  auto& context = *static_cast<StreamContext*>(destination);
  if (context.cancel_requested()) {
    return 0;  // aborts the transfer with CURLE_WRITE_ERROR
  }
  const std::size_t bytes = size * count;
  constexpr std::size_t max_response = 4 * 1024 * 1024;
  if (bytes > max_response - context.received) {
    context.error = std::make_exception_ptr(
        std::runtime_error("Streaming response exceeded the 4 MiB safety limit"));
    return 0;
  }
  context.received += bytes;
  try {
    context.on_bytes(std::string(data, bytes));
  } catch (...) {
    context.error = std::current_exception();
    return 0;
  }
  return bytes;
}

int transfer_progress(void* destination, curl_off_t, curl_off_t,
                      curl_off_t, curl_off_t) {
  const auto& context = *static_cast<StreamContext*>(destination);
  return context.cancel_requested() ? 1 : 0;
}

// Save and parse each exact response-body callback before asking curl for more.
void post_json_stream(
    const std::string& url, const std::string& body,
    const std::function<void(const std::string&)>& on_bytes,
    const std::string& api_key = "",
    const std::vector<std::string>& extra_headers = {},
    std::atomic<bool>* cancelled = nullptr,
    const CancelToken* external_cancel = nullptr) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create HTTP request");
  StreamContext context;
  context.on_bytes = on_bytes;
  context.cancelled = cancelled;
  context.external_cancel = external_cancel;
  long status = 0;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key;
  if (!api_key.empty()) headers = curl_slist_append(headers, authorization.c_str());
  for (const auto& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  validate_provider_endpoint(url);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_bytes);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &context);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (context.error) std::rethrow_exception(context.error);
  if (result == CURLE_WRITE_ERROR || result == CURLE_ABORTED_BY_CALLBACK) {
    if (context.cancel_requested()) throw Cancelled();
    if (result == CURLE_ABORTED_BY_CALLBACK) {
      throw std::runtime_error("Model request was aborted unexpectedly");
    }
    throw Cancelled();  // the write callback aborted it
  }
  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("Model request failed: ") +
                             curl_easy_strerror(result));
  }
  if (status < 200 || status >= 300) {
    if (status >= 300 && status < 400)
      throw std::runtime_error("Model server returned HTTP redirect " + std::to_string(status) + "; redirects are not followed for provider requests");
    throw std::runtime_error("Model server returned HTTP " +
                             std::to_string(status) + " during streaming");
  }
}

ReplayResult generate_stream(
    const std::string& url, const std::string& body,
    ProviderKind kind, const RawSink& on_raw, const ChunkSink& on_chunk,
    const std::string& api_key = "",
    const std::vector<std::string>& extra_headers = {},
    std::atomic<bool>* cancelled = nullptr,
    const CancelToken* external_cancel = nullptr) {
  ReplayParser parser(kind);
  post_json_stream(
      url, body,
      [&](const std::string& bytes) {
        if (on_raw) on_raw(bytes);
        parser.append(bytes, on_chunk);
      },
      api_key, extra_headers, cancelled, external_cancel);
  parser.finish(on_chunk);
  return parser.result();
}

HttpResponse post_json(const std::string& url, const std::string& body,
                       const std::string& api_key = "",
                       long timeout_seconds = 3600,
                       const std::vector<std::string>& extra_headers = {}) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create HTTP request");
  HttpResponse response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key;
  if (!api_key.empty()) headers = curl_slist_append(headers, authorization.c_str());
  for (const auto& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  validate_provider_endpoint(url);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    if (response.exceeded_limit) {
      throw std::runtime_error("Model response exceeded the 4 MiB safety limit");
    }
    throw std::runtime_error(std::string("Model request failed: ") +
                             curl_easy_strerror(result));
  }
  if (response.status < 200 || response.status >= 300) {
    if (response.status >= 300 && response.status < 400)
      throw std::runtime_error("Model server returned HTTP redirect " + std::to_string(response.status) + "; redirects are not followed for provider requests");
    throw std::runtime_error("Model server returned HTTP " +
                             std::to_string(response.status) + ": " +
                             response.body.substr(0, 1000));
  }
  return response;
}

HttpResponse get_json(const std::string& url,
                      const std::vector<std::string>& extra_headers,
                      bool allow_model_unavailable = false) {
  validate_provider_endpoint(url);
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create model-list request");
  HttpResponse response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  for (const auto& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  const CURLcode result = curl_easy_perform(curl);
  if (result == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    if (response.exceeded_limit)
      throw std::runtime_error("Model list response exceeded the 4 MiB safety limit");
    throw std::runtime_error(std::string("Model list request failed: ") +
                             curl_easy_strerror(result));
  }
  if (response.status >= 300 && response.status < 400)
    throw std::runtime_error("Model list endpoint returned a redirect; redirects are not followed");
  if (allow_model_unavailable &&
      (response.status == 403 || response.status == 404)) {
    return response;
  }
  if (response.status < 200 || response.status >= 300)
    throw std::runtime_error("Model list endpoint returned HTTP " +
                             std::to_string(response.status));
  return response;
}

std::string json_string_field(json_object* object, const char* key) {
  json_object* value = nullptr;
  if (object == nullptr || !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return {};
  }
  return json_object_get_string(value);
}

bool model_allows_sampling(json_object* model) {
  json_object* permissions = nullptr;
  if (!json_object_object_get_ex(model, "permission", &permissions) ||
      permissions == nullptr ||
      json_object_is_type(permissions, json_type_null)) {
    return true;
  }
  if (!json_object_is_type(permissions, json_type_array)) return false;
  const std::size_t count = json_object_array_length(permissions);
  for (std::size_t index = 0; index < count; ++index) {
    json_object* permission = json_object_array_get_idx(permissions, index);
    json_object* sampling = nullptr;
    json_object* blocking = nullptr;
    if (permission != nullptr &&
        json_object_object_get_ex(permission, "allow_sampling", &sampling) &&
        json_object_is_type(sampling, json_type_boolean) &&
        json_object_get_boolean(sampling) &&
        (!json_object_object_get_ex(permission, "is_blocking", &blocking) ||
         !json_object_is_type(blocking, json_type_boolean) ||
         !json_object_get_boolean(blocking))) {
      return true;
    }
  }
  return false;
}

std::string url_escape(CURL* curl, const std::string& value) {
  char* escaped = curl_easy_escape(curl, value.c_str(),
                                  static_cast<int>(value.size()));
  if (escaped == nullptr) throw std::runtime_error("Could not encode model-list cursor");
  std::string result(escaped);
  curl_free(escaped);
  return result;
}

std::string messages_json(const Messages& messages) {
  Json array(json_object_new_array());
  for (const auto& [role, content] : messages) {
    Json item(json_object_new_object());
    json_object_object_add(item.get(), "role",
                           json_object_new_string(role.c_str()));
    json_object_object_add(item.get(), "content",
                           json_object_new_string(content.c_str()));
    json_object_array_add(array.get(), item.release());
  }
  return json_object_to_json_string(array.get());
}

std::string normalize_url(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

std::string ollama_generate(const ModelSlot& slot, const Messages& messages,
                            const std::string& base_url,
                            const RawSink& on_raw,
                            const ChunkSink& on_chunk,
                            std::atomic<bool>* cancelled,
                            const CancelToken* external_cancel) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  Json parsed_messages(json_tokener_parse(messages_json(messages).c_str()));
  json_object_object_add(body.get(), "messages", parsed_messages.release());
  json_object_object_add(body.get(), "stream", json_object_new_boolean(true));
  json_object_object_add(body.get(), "keep_alive",
                         json_object_new_string("30s"));
  Json options(json_object_new_object());
  json_object_object_add(options.get(), "num_ctx",
                         json_object_new_int(slot.context_size));
  if (slot.max_tokens > 0) {
    json_object_object_add(options.get(), "num_predict",
                           json_object_new_int(slot.max_tokens));
  }
  json_object_object_add(body.get(), "options", options.release());
  const ReplayResult response = generate_stream(
      base_url + "/api/chat",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      ProviderKind::Ollama, on_raw, on_chunk, "", {}, cancelled,
      external_cancel);
  if (response.content.empty()) {
    throw std::runtime_error("Ollama stream produced no content");
  }
  return response.content;
}

std::string openai_generate(const ModelSlot& slot, const Messages& messages,
                            const std::string& base_url,
                            const std::string& api_key,
                            const RawSink& on_raw,
                            const ChunkSink& on_chunk,
                            std::atomic<bool>* cancelled,
                            const CancelToken* external_cancel) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  Json parsed_messages(json_tokener_parse(messages_json(messages).c_str()));
  json_object_object_add(body.get(), "messages", parsed_messages.release());
  json_object_object_add(body.get(), "stream", json_object_new_boolean(true));
  if (slot.max_tokens > 0) {
    json_object_object_add(body.get(), "max_tokens",
                           json_object_new_int(slot.max_tokens));
  }
  const ReplayResult response = generate_stream(
      base_url + "/chat/completions",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      ProviderKind::OpenAI, on_raw, on_chunk, api_key, {}, cancelled,
      external_cancel);
  if (response.content.empty()) {
    throw std::runtime_error("Endpoint stream produced no content");
  }
  return response.content;
}

std::string anthropic_generate(const ModelSlot& slot, const Messages& messages,
                               const std::string& base_url,
                               const std::string& api_key,
                               const RawSink& on_raw,
                               const ChunkSink& on_chunk,
                               std::atomic<bool>* cancelled,
                               const CancelToken* external_cancel) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  json_object_object_add(body.get(), "max_tokens",
                         json_object_new_int(slot.max_tokens));
  json_object_object_add(body.get(), "stream", json_object_new_boolean(true));
  Json request_messages(json_object_new_array());
  std::string system_prompt;
  for (const auto& [role, content] : messages) {
    if (role == "system") {
      if (!system_prompt.empty()) system_prompt += "\n\n";
      system_prompt += content;
      continue;
    }
    Json item(json_object_new_object());
    json_object_object_add(item.get(), "role",
                           json_object_new_string(role.c_str()));
    json_object_object_add(item.get(), "content",
                           json_object_new_string(content.c_str()));
    json_object_array_add(request_messages.get(), item.release());
  }
  if (!system_prompt.empty()) {
    json_object_object_add(body.get(), "system",
                           json_object_new_string(system_prompt.c_str()));
  }
  json_object_object_add(body.get(), "messages", request_messages.release());
  const std::vector<std::string> headers = {
      "x-api-key: " + api_key, "anthropic-version: 2023-06-01"};
  std::string endpoint = normalize_url(base_url);
  if (endpoint.size() < 3 || endpoint.substr(endpoint.size() - 3) != "/v1") {
    endpoint += "/v1";
  }
  const ReplayResult response = generate_stream(
      endpoint + "/messages",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      ProviderKind::Anthropic, on_raw, on_chunk, "", headers, cancelled,
      external_cancel);
  if (response.content.empty()) {
    throw std::runtime_error("Anthropic stream produced no content");
  }
  return response.content;
}

unsigned short available_port() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) throw std::runtime_error("Could not allocate a local port");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    close(socket_fd);
    throw std::runtime_error("Could not reserve a local port for llama server");
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    close(socket_fd);
    throw std::runtime_error("Could not inspect the local llama server port");
  }
  const auto port = ntohs(address.sin_port);
  close(socket_fd);
  return port;
}

bool supports_llama_server_subcommand() {
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("llama", "llama", "server", "--help", static_cast<char*>(nullptr));
    _exit(127);
  }

  // Older llama.cpp builds may have an executable named `llama` without the
  // `server` subcommand. Probe briefly before trying the standalone binary.
  for (int attempt = 0; attempt < 100; ++attempt) {
    int status = 0;
    const pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (result < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  kill(child, SIGKILL);
  waitpid(child, nullptr, 0);
  return false;
}

std::string llama_log_tail(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  input.seekg(0, std::ios::end);
  const std::streamoff end = static_cast<std::streamoff>(input.tellg());
  if (end <= 0) return {};
  constexpr std::streamoff limit = 2400;
  const std::streamoff start = end > limit ? end - limit : 0;
  input.seekg(start, std::ios::beg);
  std::string tail(static_cast<std::size_t>(end - start), '\0');
  input.read(tail.data(), static_cast<std::streamsize>(tail.size()));
  if (!input && !input.eof()) return {};
  tail.erase(std::remove_if(tail.begin(), tail.end(), [](unsigned char c) {
               return (c < 0x20 && c != '\n' && c != '\r' && c != '\t') ||
                      c == 0x7f;
             }),
             tail.end());
  return tail;
}

class LlamaServer {
 public:
  LlamaServer(const ModelSlot& slot,
              const std::function<void(const std::string&)>& status,
              const CancelToken* cancel)
      : port_(available_port()), cancel_(cancel) {
    if (access(slot.gguf_path.c_str(), R_OK) != 0) {
      throw std::runtime_error("GGUF file is not readable: " + slot.gguf_path);
    }
    const std::string role = slot.name.empty() ? "llama" : slot.name;
    const auto log_file = llama_log_path(role);
    std::error_code error;
    std::filesystem::create_directories(log_file.parent_path(), error);
    if (error) {
      throw std::runtime_error("Could not create logs directory: " + error.message());
    }
    std::filesystem::permissions(
        log_file.parent_path(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, error);
    if (error) {
      throw std::runtime_error("Could not protect logs directory: " +
                               error.message());
    }
    const bool modern_cli = supports_llama_server_subcommand();
    status(modern_cli
               ? "Starting llama server (one local model at a time)..."
               : "Starting legacy standalone server (one local model at a time)...");
    pid_ = fork();
    if (pid_ < 0) throw std::runtime_error("Could not start llama server");
    if (pid_ > 0) mtrxai::signals::set_active_child(pid_);
    if (pid_ == 0) {
      if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
      if (getppid() == 1) _exit(1);

      // The child must never write to the terminal: llama server is chatty
      // while loading and would corrupt the ncurses/FTXUI screen. Keep its
      // diagnostics in a private per-role log instead.
      const int devnull_in = open("/dev/null", O_RDONLY);
      if (devnull_in >= 0) {
        dup2(devnull_in, STDIN_FILENO);
        close(devnull_in);
      }
      const int log_fd =
          open(log_file.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
      if (log_fd >= 0) {
        // O_CREAT mode only applies to a new file; enforce owner-only even if
        // an older log was created with broader permissions.
        fchmod(log_fd, 0600);
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
      } else {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
          dup2(devnull, STDOUT_FILENO);
          dup2(devnull, STDERR_FILENO);
          close(devnull);
        }
      }

      const std::string port = std::to_string(port_);
      const std::string context = std::to_string(slot.context_size);
      if (modern_cli) {
        execlp("llama", "llama", "server", "--model", slot.gguf_path.c_str(),
               "--host", "127.0.0.1", "--port", port.c_str(), "--ctx-size",
               context.c_str(), "--device", "none", "--n-gpu-layers", "0",
               static_cast<char*>(nullptr));
      } else {
        execlp("llama-server", "llama-server", "--model",
               slot.gguf_path.c_str(), "--host", "127.0.0.1", "--port",
               port.c_str(), "--ctx-size", context.c_str(), "--n-gpu-layers",
               "0", static_cast<char*>(nullptr));
      }
      _exit(127);
    }
    const std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    const int timeout = std::clamp(slot.gguf_load_timeout_seconds, 1, 3600);
    const int attempts = timeout * 4;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if ((cancel_ != nullptr && cancel_->cancelled()) ||
          process_cancel_token().cancelled()) {
        stop();
        throw Cancelled("Cancelled while starting llama server.");
      }
      int child_status = 0;
      if (waitpid(pid_, &child_status, WNOHANG) == pid_) {
        pid_ = -1;
        std::string message =
            "llama server exited before becoming ready; check that the runtime "
            "supports this GGUF model.";
        const std::string details = llama_log_tail(log_file);
        if (!details.empty()) message += "\nRecent llama server log:\n" + details;
        throw std::runtime_error(message);
      }
      CURL* curl = curl_easy_init();
      if (curl != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, health_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 1L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        const CURLcode result = curl_easy_perform(curl);
        long status_code = 0;
        if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
        curl_easy_cleanup(curl);
        if (result == CURLE_OK && status_code >= 200 && status_code < 300) return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    stop();
    throw std::runtime_error("llama server did not become ready within " +
                             std::to_string(timeout) + " seconds");
  }

  ~LlamaServer() { stop(); }
  LlamaServer(const LlamaServer&) = delete;
  LlamaServer& operator=(const LlamaServer&) = delete;

  std::string endpoint() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
  }

 private:
  void stop() {
    if (pid_ <= 0) return;
    mtrxai::signals::set_active_child(0);
    kill(pid_, SIGTERM);
    for (int attempt = 0; attempt < 40; ++attempt) {
      if (waitpid(pid_, nullptr, WNOHANG) == pid_) {
        pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    kill(pid_, SIGKILL);
    waitpid(pid_, nullptr, 0);
    pid_ = -1;
  }

  pid_t pid_ = -1;
  unsigned short port_;
  const CancelToken* cancel_ = nullptr;
};

std::string environment_key(const std::string& name) {
  if (name.empty()) return "";
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || *value == '\0') {
    throw std::runtime_error("API key environment variable is unset: " + name);
  }
  return value;
}

}  // namespace

std::vector<std::string> fetch_available_models(
    const std::string& provider, const std::string& base_url,
    const std::string& api_key) {
  if (provider != "openai" && provider != "anthropic") {
    throw std::runtime_error("Model listing is supported for OpenAI-compatible and Anthropic providers");
  }
  if (api_key.empty()) throw std::runtime_error("An API key is required to list models");
  validate_provider_endpoint(base_url);

  std::string endpoint = normalize_url(base_url);
  if (endpoint.size() < 3 || endpoint.substr(endpoint.size() - 3) != "/v1")
    endpoint += "/v1";
  endpoint += "/models";

  std::vector<std::string> headers;
  const bool anthropic = provider == "anthropic";
  if (anthropic) {
    headers = {"x-api-key: " + api_key,
               "anthropic-version: 2023-06-01"};
  } else {
    headers = {"Authorization: Bearer " + api_key};
  }

  std::vector<std::string> models;
  std::string cursor;
  for (int page = 0; page < 100; ++page) {
    std::string page_url = endpoint;
    if (!cursor.empty()) {
      CURL* encoder = curl_easy_init();
      if (encoder == nullptr)
        throw std::runtime_error("Could not create model-list request");
      const std::string escaped = url_escape(encoder, cursor);
      curl_easy_cleanup(encoder);
      page_url += anthropic ? "?after_id=" : "?after=";
      page_url += escaped;
    }

    const HttpResponse response = get_json(page_url, headers);
    Json parsed(json_tokener_parse(response.body.c_str()));
    if (!parsed || !json_object_is_type(parsed.get(), json_type_object))
      throw std::runtime_error("Model list endpoint returned invalid JSON");

    json_object* data = nullptr;
    if (!json_object_object_get_ex(parsed.get(), "data", &data) ||
        !json_object_is_type(data, json_type_array)) {
      if (page == 0)
        throw std::runtime_error("Model list response does not contain a data array");
      break;
    }
    const std::size_t previous_count = models.size();
    const std::size_t count = json_object_array_length(data);
    for (std::size_t index = 0; index < count; ++index) {
      json_object* item = json_object_array_get_idx(data, index);
      const std::string id = json_string_field(item, "id");
      if (!id.empty() &&
          std::find(models.begin(), models.end(), id) == models.end()) {
        models.push_back(id);
        const std::size_t max_models = anthropic ? 5000 : 250;
        if (models.size() >= max_models) {
          throw std::runtime_error(
              "Model list contains too many entries to verify safely");
        }
      }
    }

    json_object* has_more_value = nullptr;
    const bool has_more =
        json_object_object_get_ex(parsed.get(), "has_more", &has_more_value) &&
        json_object_is_type(has_more_value, json_type_boolean) &&
        json_object_get_boolean(has_more_value);
    if (!has_more) break;
    cursor = json_string_field(parsed.get(), "last_id");
    if (cursor.empty() || models.size() == previous_count)
      throw std::runtime_error("Model list pagination did not advance");
  }

  if (!anthropic) {
    // The list can include model IDs that this key cannot retrieve. Confirm
    // each ID against the authenticated model-detail endpoint before showing it.
    std::vector<std::string> accessible;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> encoder(
        curl_easy_init(), curl_easy_cleanup);
    if (!encoder)
      throw std::runtime_error("Could not create model access check");
    for (const auto& id : models) {
      const std::string escaped = url_escape(encoder.get(), id);
      const HttpResponse detail =
          get_json(endpoint + "/" + escaped, headers, true);
      if (detail.status == 403 || detail.status == 404) continue;
      Json parsed(json_tokener_parse(detail.body.c_str()));
      if (!parsed || !json_object_is_type(parsed.get(), json_type_object))
        continue;
      if (json_string_field(parsed.get(), "id") == id &&
          model_allows_sampling(parsed.get()))
        accessible.push_back(id);
    }
    models = std::move(accessible);
  }
  return models;
}

struct InferenceSession::Impl {
  ModelSlot slot;
  Status status;
  std::string base_url;
  std::string endpoint;
  std::string api_key;
  std::unique_ptr<LlamaServer> llama_server;
  const CancelToken* external_cancel = nullptr;
  bool released = false;
  std::atomic<bool> cancelled{false};

  Impl(const ModelSlot& model, const Status& update, const CancelToken* cancel)
      : slot(model),
        status(update),
        base_url(normalize_url(model.base_url)),
        external_cancel(cancel) {
    if (slot.provider == "ollama") {
      if (base_url.empty()) {
        throw std::runtime_error("An Ollama URL is required for " + slot.name);
      }
      mtrxai::signals::set_active_ollama(slot.base_url, slot.model);
      status("Ollama will keep this role loaded only during its stage.");
      return;
    }
    if (slot.provider == "openai") {
      if (base_url.empty()) {
        throw std::runtime_error("An endpoint URL is required for " + slot.name);
      }
      endpoint = base_url;
      if (endpoint.size() < 3 || endpoint.substr(endpoint.size() - 3) != "/v1") {
        endpoint += "/v1";
      }
      api_key = slot.api_key.empty() ? environment_key(slot.api_key_env)
                                     : slot.api_key;
      return;
    }
    if (slot.provider == "anthropic") {
      if (base_url.empty()) {
        throw std::runtime_error("An Anthropic API URL is required for " +
                                 slot.name);
      }
      api_key = slot.api_key.empty() ? environment_key(slot.api_key_env)
                                     : slot.api_key;
      if (api_key.empty()) {
        throw std::runtime_error("An Anthropic API key is required for " +
                                 slot.name +
                                 "; configure credentials.json or api_key_env.");
      }
      return;
    }
    if (slot.provider == "gguf") {
      if (slot.gguf_path.empty()) {
        throw std::runtime_error(
            "No GGUF file is configured for " + slot.name +
            ". Set gguf_path in config.json or use `/gguf " + slot.name +
            " /path/to/model.gguf` in the TUI.");
      }
      llama_server = std::make_unique<LlamaServer>(slot, status, external_cancel);
      endpoint = llama_server->endpoint();
      return;
    }
    throw std::runtime_error("Unsupported provider: " + slot.provider);
  }
};

InferenceSession::InferenceSession(const ModelSlot& slot, const Status& status,
                                   const CancelToken* cancel)
    : impl_(std::make_unique<Impl>(slot, status, cancel)) {}

InferenceSession::~InferenceSession() noexcept {
  // Cancellation unwinds through this destructor; try to unload the model
  // exactly as a normal release would so we do not leave RAM occupied until
  // Ollama's keep_alive window expires. Failures here must not mask the
  // original exception.
  try {
    release();
  } catch (...) {
  }
  mtrxai::signals::clear_active_ollama();
  if (impl_ && impl_->llama_server) impl_->llama_server.reset();
}

std::string InferenceSession::generate(const Messages& messages) {
  return generate(messages, ChunkSink());
}

std::string InferenceSession::generate(const Messages& messages,
                                       const TokenSink& on_token) {
  ChunkSink chunk_sink;
  if (on_token) {
    chunk_sink = [&on_token](const StreamChunk& chunk) {
      if (chunk.kind == StreamChunk::Kind::Content) on_token(chunk.text);
    };
  }
  return generate(messages, chunk_sink);
}

std::string InferenceSession::generate(const Messages& messages,
                                       const ChunkSink& on_chunk) {
  return generate(messages, RawSink(), on_chunk);
}

std::string InferenceSession::generate(const Messages& messages,
                                       const RawSink& on_raw,
                                       const ChunkSink& on_chunk) {
  if (impl_->released) {
    throw std::runtime_error("Cannot generate after the model stage is unloaded.");
  }
  if (impl_->cancelled.load(std::memory_order_relaxed) ||
      process_cancel_token().cancelled() ||
      (impl_->external_cancel != nullptr &&
       impl_->external_cancel->cancelled())) {
    throw Cancelled();
  }
  if (impl_->slot.provider == "ollama") {
    return ollama_generate(impl_->slot, messages, impl_->base_url, on_raw,
                           on_chunk, &impl_->cancelled,
                           impl_->external_cancel);
  }
  if (impl_->slot.provider == "anthropic") {
    return anthropic_generate(impl_->slot, messages, impl_->base_url,
                              impl_->api_key, on_raw, on_chunk,
                              &impl_->cancelled, impl_->external_cancel);
  }
  return openai_generate(impl_->slot, messages, impl_->endpoint, impl_->api_key,
                         on_raw, on_chunk, &impl_->cancelled,
                         impl_->external_cancel);
}

void InferenceSession::request_cancel() {
  if (impl_) impl_->cancelled.store(true, std::memory_order_relaxed);
}

void InferenceSession::clear_cancel() {
  if (impl_) impl_->cancelled.store(false, std::memory_order_relaxed);
}

void InferenceSession::release() {
  mtrxai::signals::clear_active_ollama();
  if (!impl_ || impl_->released) return;
  if (impl_->slot.provider == "ollama") {
    impl_->status("Unloading Ollama model before the next role...");
    Json body(json_object_new_object());
    json_object_object_add(body.get(), "model",
                           json_object_new_string(impl_->slot.model.c_str()));
    json_object_object_add(body.get(), "keep_alive", json_object_new_int(0));
    try {
      post_json(impl_->base_url + "/api/generate",
                json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
                "", 10);
    } catch (const std::exception& error) {
      impl_->released = true;
      throw std::runtime_error(
          std::string("Could not unload the Ollama model before the next role; "
                      "it will auto-unload after 30 seconds: ") +
          error.what());
    }
    impl_->status("Ollama model unloaded.");
  }
  if (impl_->llama_server) {
    impl_->status("Stopping llama server before the next role...");
    impl_->llama_server.reset();
  }
  impl_->released = true;
}

}  // namespace mtrxai
