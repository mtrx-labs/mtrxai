#include "mtrxai/provider.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/stream_parse.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
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

using TokenSink = InferenceSession::TokenSink;

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

// Feeds every complete line of a streaming response to the handler. Both SSE
// and Ollama NDJSON are newline-delimited, so one callback covers all
// protocols. Stored errors are rethrown after the transfer finishes because
// exceptions must not escape a curl callback.
struct StreamContext {
  std::string pending;
  std::function<void(const std::string&)> on_line;
  std::atomic<bool>* cancelled = nullptr;
  std::exception_ptr error;

  void run_line(const std::string& line) {
    if (error) return;
    try {
      on_line(line);
    } catch (...) {
      error = std::current_exception();
    }
  }
};

std::size_t stream_lines(char* data, std::size_t size, std::size_t count,
                         void* destination) {
  auto& context = *static_cast<StreamContext*>(destination);
  if (context.cancelled != nullptr &&
      context.cancelled->load(std::memory_order_relaxed)) {
    return 0;  // aborts the transfer with CURLE_WRITE_ERROR
  }
  const std::size_t bytes = size * count;
  constexpr std::size_t max_buffer = 4 * 1024 * 1024;
  if (bytes > max_buffer - context.pending.size()) {
    context.error = std::make_exception_ptr(
        std::runtime_error("Streaming response exceeded the 4 MiB safety limit"));
    return 0;
  }
  context.pending.append(data, bytes);
  std::size_t newline = context.pending.find('\n');
  while (newline != std::string::npos && !context.error) {
    std::string line = context.pending.substr(0, newline);
    context.pending.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    context.run_line(line);
    newline = context.pending.find('\n');
  }
  return bytes;
}

// Like post_json but line-oriented; used for every generation request.
void post_json_lines(
    const std::string& url, const std::string& body,
    const std::function<void(const std::string&)>& on_line,
    const std::string& api_key = "",
    const std::vector<std::string>& extra_headers = {},
    std::atomic<bool>* cancelled = nullptr) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create HTTP request");
  StreamContext context;
  context.on_line = on_line;
  context.cancelled = cancelled;
  long status = 0;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key;
  if (!api_key.empty()) headers = curl_slist_append(headers, authorization.c_str());
  for (const auto& header : extra_headers) {
    headers = curl_slist_append(headers, header.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, stream_lines);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  const CURLcode result = curl_easy_perform(curl);
  // The final line may lack a trailing newline.
  if (!context.pending.empty() && !context.error) {
    std::string line = context.pending;
    if (line.back() == '\r') line.pop_back();
    context.run_line(line);
  }
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (context.error) std::rethrow_exception(context.error);
  if (result == CURLE_WRITE_ERROR) {
    throw Cancelled();  // the write callback aborted it
  }
  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("Model request failed: ") +
                             curl_easy_strerror(result));
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error("Model server returned HTTP " +
                             std::to_string(status) + " during streaming");
  }
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
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
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
    throw std::runtime_error("Model server returned HTTP " +
                             std::to_string(response.status) + ": " +
                             response.body.substr(0, 1000));
  }
  return response;
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
                            const TokenSink& on_token,
                            std::atomic<bool>* cancelled) {
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
  std::string content;
  post_json_lines(
      base_url + "/api/chat",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      [&](const std::string& line) {
        if (line.empty()) return;
        const std::string payload_error = stream::error_message(line);
        if (!payload_error.empty() && content.empty()) {
          throw std::runtime_error("Ollama stream reported an error: " +
                                   payload_error);
        }
        const std::string delta = stream::ollama_delta_text(line);
        if (delta.empty()) return;
        content += delta;
        if (on_token) on_token(delta);
      },
      "", {}, cancelled);
  if (content.empty()) {
    throw std::runtime_error("Ollama stream produced no content");
  }
  return content;
}

std::string openai_generate(const ModelSlot& slot, const Messages& messages,
                            const std::string& base_url,
                            const std::string& api_key,
                            const TokenSink& on_token,
                            std::atomic<bool>* cancelled) {
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
  std::string content;
  post_json_lines(
      base_url + "/chat/completions",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      [&](const std::string& line) {
        const std::string payload = stream::sse_payload(line);
        if (payload.empty() || payload == "[DONE]") return;
        if (stream::error_message(payload).empty()) {
          const std::string delta = stream::openai_delta_text(payload);
          if (!delta.empty()) {
            content += delta;
            if (on_token) on_token(delta);
          }
          return;
        }
        // Some endpoints only flag errors in the first chunks; report the
        // accumulated content if any arrives before the error.
        if (content.empty()) {
          throw std::runtime_error("Endpoint stream reported an error: " +
                                   stream::error_message(payload));
        }
      },
      api_key, {}, cancelled);
  if (content.empty()) {
    throw std::runtime_error("Endpoint stream produced no content");
  }
  return content;
}

std::string anthropic_generate(const ModelSlot& slot, const Messages& messages,
                               const std::string& base_url,
                               const std::string& api_key,
                               const TokenSink& on_token,
                               std::atomic<bool>* cancelled) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  json_object_object_add(body.get(), "max_tokens",
                         json_object_new_int(slot.max_tokens > 0 ? slot.max_tokens : 4096));
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
  std::string content;
  post_json_lines(
      endpoint + "/messages",
      json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
      [&](const std::string& line) {
        const std::string payload = stream::sse_payload(line);
        if (payload.empty()) return;
        if (!stream::error_message(payload).empty() && content.empty()) {
          throw std::runtime_error("Anthropic stream reported an error: " +
                                   stream::error_message(payload));
        }
        const std::string delta = stream::anthropic_delta_text(payload);
        if (delta.empty()) return;
        content += delta;
        if (on_token) on_token(delta);
      },
      "", headers, cancelled);
  if (content.empty()) {
    throw std::runtime_error("Anthropic stream produced no content");
  }
  return content;
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
    throw std::runtime_error("Could not reserve a local port for llama-server");
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) < 0) {
    close(socket_fd);
    throw std::runtime_error("Could not inspect the local llama-server port");
  }
  const auto port = ntohs(address.sin_port);
  close(socket_fd);
  return port;
}

class LlamaServer {
 public:
  LlamaServer(const ModelSlot& slot, const std::function<void(const std::string&)>& status)
      : port_(available_port()) {
    if (access(slot.gguf_path.c_str(), R_OK) != 0) {
      throw std::runtime_error("GGUF file is not readable: " + slot.gguf_path);
    }
    const std::string role = slot.name.empty() ? "llama-server" : slot.name;
    const auto log_file = llama_log_path(role);
    std::error_code error;
    std::filesystem::create_directories(log_file.parent_path(), error);
    if (error) {
      throw std::runtime_error("Could not create logs directory: " + error.message());
    }
    status("Starting llama-server (one local model at a time)...");
    pid_ = fork();
    if (pid_ < 0) throw std::runtime_error("Could not start llama-server");
    if (pid_ > 0) mtrxai::signals::set_active_child(pid_);
    if (pid_ == 0) {
      if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
      if (getppid() == 1) _exit(1);

      // Redirect stdout and stderr to the role log file so ncurses is not corrupted
      const int log_fd = open(log_file.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
      if (log_fd >= 0) {
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
      execlp("llama-server", "llama-server", "--model", slot.gguf_path.c_str(),
             "--host", "127.0.0.1", "--port", port.c_str(), "--ctx-size",
             context.c_str(), "--n-gpu-layers", "0", static_cast<char*>(nullptr));
      _exit(127);
    }
    const std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    const int timeout = std::clamp(slot.gguf_load_timeout_seconds, 1, 3600);
    const int attempts = timeout * 4;
    for (int attempt = 0; attempt < attempts; ++attempt) {
      int child_status = 0;
      if (waitpid(pid_, &child_status, WNOHANG) == pid_) {
        pid_ = -1;
        throw std::runtime_error(
            "llama-server exited before becoming ready; verify it is installed and "
            "supports this GGUF model.");
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
    throw std::runtime_error("llama-server did not become ready within " +
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

struct InferenceSession::Impl {
  ModelSlot slot;
  Status status;
  std::string base_url;
  std::string endpoint;
  std::string api_key;
  std::unique_ptr<LlamaServer> llama_server;
  bool released = false;
  std::atomic<bool> cancelled{false};

  Impl(const ModelSlot& model, const Status& update)
      : slot(model), status(update), base_url(normalize_url(model.base_url)) {
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
      llama_server = std::make_unique<LlamaServer>(slot, status);
      endpoint = llama_server->endpoint();
      return;
    }
    throw std::runtime_error("Unsupported provider: " + slot.provider);
  }
};

InferenceSession::InferenceSession(const ModelSlot& slot, const Status& status)
    : impl_(std::make_unique<Impl>(slot, status)) {}

InferenceSession::~InferenceSession() noexcept {
  mtrxai::signals::clear_active_ollama();
  if (impl_ && impl_->llama_server) impl_->llama_server.reset();
}

std::string InferenceSession::generate(const Messages& messages) {
  return generate(messages, TokenSink());
}

std::string InferenceSession::generate(const Messages& messages,
                                       const TokenSink& on_token) {
  if (impl_->released) {
    throw std::runtime_error("Cannot generate after the model stage is unloaded.");
  }
  if (impl_->cancelled.load(std::memory_order_relaxed)) {
    throw Cancelled();
  }
  if (impl_->slot.provider == "ollama") {
    return ollama_generate(impl_->slot, messages, impl_->base_url, on_token,
                           &impl_->cancelled);
  }
  if (impl_->slot.provider == "anthropic") {
    return anthropic_generate(impl_->slot, messages, impl_->base_url,
                              impl_->api_key, on_token, &impl_->cancelled);
  }
  return openai_generate(impl_->slot, messages, impl_->endpoint, impl_->api_key,
                         on_token, &impl_->cancelled);
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
    impl_->status("Stopping llama-server before the next role...");
    impl_->llama_server.reset();
  }
  impl_->released = true;
}

}  // namespace mtrxai
