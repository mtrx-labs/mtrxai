#include "mtrxai/provider.hpp"

#include <curl/curl.h>
#include <json-c/json.h>

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <chrono>
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
#include <vector>

namespace mtrxai {
namespace {

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

HttpResponse post_json(const std::string& url, const std::string& body,
                       const std::string& api_key = "",
                       long timeout_seconds = 3600) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create HTTP request");
  HttpResponse response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string authorization = "Authorization: Bearer " + api_key;
  if (!api_key.empty()) headers = curl_slist_append(headers, authorization.c_str());
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

std::string json_string(json_object* object, const char* key) {
  json_object* value = nullptr;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    throw std::runtime_error(std::string("Model response is missing ") + key);
  }
  return json_object_get_string(value);
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
                            const std::string& base_url) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  Json parsed_messages(json_tokener_parse(messages_json(messages).c_str()));
  json_object_object_add(body.get(), "messages", parsed_messages.release());
  json_object_object_add(body.get(), "stream", json_object_new_boolean(false));
  json_object_object_add(body.get(), "keep_alive",
                         json_object_new_string("30s"));
  Json options(json_object_new_object());
  json_object_object_add(options.get(), "num_ctx",
                         json_object_new_int(slot.context_size));
  json_object_object_add(body.get(), "options", options.release());
  const auto response =
      post_json(base_url + "/api/chat",
                json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN));
  Json root(json_tokener_parse(response.body.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("Ollama returned invalid JSON");
  }
  json_object* message = nullptr;
  if (!json_object_object_get_ex(root.get(), "message", &message) ||
      !json_object_is_type(message, json_type_object)) {
    throw std::runtime_error("Ollama response is missing message");
  }
  return json_string(message, "content");
}

std::string openai_generate(const ModelSlot& slot, const Messages& messages,
                            const std::string& base_url,
                            const std::string& api_key) {
  Json body(json_object_new_object());
  json_object_object_add(body.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  Json parsed_messages(json_tokener_parse(messages_json(messages).c_str()));
  json_object_object_add(body.get(), "messages", parsed_messages.release());
  json_object_object_add(body.get(), "stream", json_object_new_boolean(false));
  const auto response =
      post_json(base_url + "/chat/completions",
                json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN),
                api_key);
  Json root(json_tokener_parse(response.body.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("OpenAI-compatible endpoint returned invalid JSON");
  }
  json_object* choices = nullptr;
  json_object* choice = nullptr;
  json_object* message = nullptr;
  if (!json_object_object_get_ex(root.get(), "choices", &choices) ||
      !json_object_is_type(choices, json_type_array) ||
      json_object_array_length(choices) == 0 ||
      (choice = json_object_array_get_idx(choices, 0)) == nullptr ||
      !json_object_object_get_ex(choice, "message", &message)) {
    throw std::runtime_error("OpenAI-compatible response is missing choices[0].message");
  }
  return json_string(message, "content");
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
    status("Starting llama-server (one local model at a time)...");
    pid_ = fork();
    if (pid_ < 0) throw std::runtime_error("Could not start llama-server");
    if (pid_ == 0) {
      if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
      if (getppid() == 1) _exit(1);
      const std::string port = std::to_string(port_);
      const std::string context = std::to_string(slot.context_size);
      execlp("llama-server", "llama-server", "--model", slot.gguf_path.c_str(),
             "--host", "127.0.0.1", "--port", port.c_str(), "--ctx-size",
             context.c_str(), "--n-gpu-layers", "0", static_cast<char*>(nullptr));
      _exit(127);
    }
    const std::string health_url = "http://127.0.0.1:" + std::to_string(port_) + "/health";
    for (int attempt = 0; attempt < 600; ++attempt) {
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
    throw std::runtime_error("llama-server did not become ready within 150 seconds");
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

  Impl(const ModelSlot& model, const Status& update)
      : slot(model), status(update), base_url(normalize_url(model.base_url)) {
    if (slot.provider == "ollama") {
      if (base_url.empty()) {
        throw std::runtime_error("An Ollama URL is required for " + slot.name);
      }
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
      api_key = environment_key(slot.api_key_env);
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
  if (impl_ && impl_->llama_server) impl_->llama_server.reset();
}

std::string InferenceSession::generate(const Messages& messages) {
  if (impl_->released) {
    throw std::runtime_error("Cannot generate after the model stage is unloaded.");
  }
  if (impl_->slot.provider == "ollama") {
    return ollama_generate(impl_->slot, messages, impl_->base_url);
  }
  return openai_generate(impl_->slot, messages, impl_->endpoint, impl_->api_key);
}

void InferenceSession::release() {
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
