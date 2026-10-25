#include "mtrxai/coder_transcript.hpp"

#include <json-c/json.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace mtrxai {
namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

bool valid_role(const std::string& role) {
  return role == "system" || role == "user" || role == "assistant";
}

}  // namespace

CoderTranscript::CoderTranscript(const std::filesystem::path& path)
    : path_(path) {
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create Coder transcript directory: " +
                             error.message());
  }
  descriptor_ = open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (descriptor_ < 0) {
    throw std::runtime_error("Could not open Coder transcript " +
                             path_.string() + ": " + std::strerror(errno));
  }
}

CoderTranscript::~CoderTranscript() {
  if (descriptor_ >= 0) close(descriptor_);
}

void CoderTranscript::append(const CoderMessage& message) {
  Json record(json_object_new_object());
  json_object_object_add(record.get(), "role",
                         json_object_new_string(message.role.c_str()));
  json_object_object_add(record.get(), "content",
                         json_object_new_string(message.content.c_str()));
  if (!message.thinking.empty()) {
    json_object_object_add(record.get(), "thinking",
                           json_object_new_string(message.thinking.c_str()));
  }
  if (message.turn > 0) {
    json_object_object_add(record.get(), "turn",
                           json_object_new_int(message.turn));
  }
  const std::string line =
      std::string(json_object_to_json_string_ext(record.get(),
                                                 JSON_C_TO_STRING_PLAIN)) +
      "\n";
  std::size_t written = 0;
  while (written < line.size()) {
    const ssize_t count =
        write(descriptor_, line.data() + written, line.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      throw std::runtime_error("Could not append to Coder transcript " +
                               path_.string() + ": " + std::strerror(errno));
    }
    written += static_cast<std::size_t>(count);
  }
  // Resume depends on this message being on disk before the next model call,
  // so unlike streamed tokens (fsynced about once a second) it is forced now.
  if (fsync(descriptor_) != 0) {
    throw std::runtime_error("Could not flush Coder transcript " +
                             path_.string() + ": " + std::strerror(errno));
  }
}

std::vector<CoderMessage> CoderTranscript::load(
    const std::filesystem::path& path) {
  std::vector<CoderMessage> messages;
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error) {
    return messages;
  }
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("Could not read Coder transcript " + path.string());
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;
    Json record(json_tokener_parse(line.c_str()));
    if (record == nullptr) continue;
    json_object* role = nullptr;
    json_object* content = nullptr;
    if (!json_object_object_get_ex(record.get(), "role", &role) ||
        !json_object_object_get_ex(record.get(), "content", &content) ||
        !json_object_is_type(role, json_type_string) ||
        !json_object_is_type(content, json_type_string)) {
      continue;
    }
    CoderMessage message;
    message.role = json_object_get_string(role);
    if (!valid_role(message.role)) continue;
    message.content = json_object_get_string(content);
    json_object* thinking = nullptr;
    if (json_object_object_get_ex(record.get(), "thinking", &thinking) &&
        json_object_is_type(thinking, json_type_string)) {
      message.thinking = json_object_get_string(thinking);
    }
    json_object* turn = nullptr;
    if (json_object_object_get_ex(record.get(), "turn", &turn) &&
        json_object_is_type(turn, json_type_int)) {
      message.turn = json_object_get_int(turn);
    }
    messages.push_back(std::move(message));
  }
  return messages;
}

}  // namespace mtrxai
