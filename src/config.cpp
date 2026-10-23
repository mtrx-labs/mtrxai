#include "mtrxai/config.hpp"
#include "mtrxai/files.hpp"

#include <json-c/json.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace mtrxai {
namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

Json slot_json(const ModelSlot& slot) {
  Json result(json_object_new_object());
  json_object_object_add(result.get(), "provider",
                         json_object_new_string(slot.provider.c_str()));
  json_object_object_add(result.get(), "model",
                         json_object_new_string(slot.model.c_str()));
  json_object_object_add(result.get(), "base_url",
                         json_object_new_string(slot.base_url.c_str()));
  json_object_object_add(result.get(), "api_key_env",
                         json_object_new_string(slot.api_key_env.c_str()));
  json_object_object_add(result.get(), "gguf_path",
                         json_object_new_string(slot.gguf_path.c_str()));
  json_object_object_add(result.get(), "context_size",
                         json_object_new_int(slot.context_size));
  return result;
}

std::string string_field(json_object* object, const char* field,
                         const std::string& fallback) {
  json_object* value = nullptr;
  if (!json_object_object_get_ex(object, field, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return fallback;
  }
  return json_object_get_string(value);
}

ModelSlot parse_slot(const char* name, json_object* root) {
  json_object* object = nullptr;
  if (!json_object_object_get_ex(root, name, &object) ||
      !json_object_is_type(object, json_type_object)) {
    throw std::runtime_error(std::string("Missing model configuration: ") + name);
  }

  ModelSlot slot;
  slot.name = name;
  slot.provider = string_field(object, "provider", "");
  slot.model = string_field(object, "model", "");
  slot.base_url = string_field(object, "base_url", "");
  slot.api_key_env = string_field(object, "api_key_env", "");
  slot.gguf_path = string_field(object, "gguf_path", "");
  json_object* context = nullptr;
  if (json_object_object_get_ex(object, "context_size", &context) &&
      json_object_is_type(context, json_type_int)) {
    slot.context_size = json_object_get_int(context);
  }

  if (slot.provider != "ollama" && slot.provider != "openai" &&
      slot.provider != "anthropic" && slot.provider != "gguf") {
    throw std::runtime_error("Provider for " + slot.name +
                             " must be ollama, openai, anthropic, or gguf");
  }
  if (slot.model.empty() && slot.provider != "gguf") {
    throw std::runtime_error("Model name is required for " + slot.name);
  }
  if (slot.context_size < 512 || slot.context_size > 32768) {
    throw std::runtime_error("context_size for " + slot.name +
                             " must be between 512 and 32768");
  }
  return slot;
}

void load_credentials(Config& config, const std::filesystem::path& path) {
  const auto credentials_path = path.parent_path() / "credentials.json";
  if (!std::filesystem::exists(credentials_path)) return;
  if (std::filesystem::is_symlink(
          std::filesystem::symlink_status(credentials_path))) {
    throw std::runtime_error("Credentials file must not be a symbolic link: " +
                             credentials_path.string());
  }
  const auto permissions =
      std::filesystem::status(credentials_path).permissions();
  if ((permissions & (std::filesystem::perms::group_all |
                      std::filesystem::perms::others_all)) !=
      std::filesystem::perms::none) {
    throw std::runtime_error(
        "Credentials file permissions are too broad; restrict it to owner "
        "read/write: " +
        credentials_path.string());
  }
  Json credentials(json_object_from_file(credentials_path.c_str()));
  if (!credentials ||
      !json_object_is_type(credentials.get(), json_type_object)) {
    throw std::runtime_error("Could not parse credentials file: " +
                            credentials_path.string());
  }
  for (auto* slot : {&config.chief, &config.prompter, &config.coder}) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(credentials.get(), slot->name.c_str(),
                                   &value)) {
      continue;
    }
    if (!json_object_is_type(value, json_type_string)) {
      throw std::runtime_error("Credential for " + slot->name +
                               " must be a string");
    }
    slot->api_key = json_object_get_string(value);
  }
}

Json credentials_json(const Config& config) {
  Json result(json_object_new_object());
  const ModelSlot* slots[] = {&config.chief, &config.prompter, &config.coder};
  for (const auto* slot : slots) {
    if (!slot->api_key.empty()) {
      json_object_object_add(result.get(), slot->name.c_str(),
                             json_object_new_string(slot->api_key.c_str()));
    }
  }
  return result;
}

}  // namespace

std::filesystem::path config_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "mtrxai" / "config.json";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; cannot locate mtrxAI configuration");
  }
  return std::filesystem::path(home) / ".config" / "mtrxai" / "config.json";
}

Config load_config(const std::filesystem::path& path) {
  Json root(json_object_from_file(path.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("Could not parse config file: " + path.string());
  }
  Config config{parse_slot("chief", root.get()), parse_slot("prompter", root.get()),
                parse_slot("coder", root.get())};
  load_credentials(config, path);
  return config;
}

void save_config(const Config& config, const std::filesystem::path& path) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "chief", slot_json(config.chief).release());
  json_object_object_add(root.get(), "prompter",
                         slot_json(config.prompter).release());
  json_object_object_add(root.get(), "coder", slot_json(config.coder).release());

  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create config directory: " +
                             error.message());
  }

  Json credentials = credentials_json(config);
  const auto credentials_path = path.parent_path() / "credentials.json";
  if (json_object_object_length(credentials.get()) > 0) {
    write_atomic(credentials_path,
                 json_object_to_json_string_ext(credentials.get(),
                                                JSON_C_TO_STRING_PRETTY));
  } else if (std::filesystem::exists(credentials_path)) {
    std::filesystem::remove(credentials_path, error);
    if (error) {
      throw std::runtime_error("Could not remove credentials file: " +
                               error.message());
    }
  }

  write_atomic(path, json_object_to_json_string_ext(
                         root.get(), JSON_C_TO_STRING_PRETTY));
}

ModelSlot& find_slot(Config& config, const std::string& name) {
  if (name == "chief") return config.chief;
  if (name == "prompter") return config.prompter;
  if (name == "coder") return config.coder;
  throw std::runtime_error("Unknown role: " + name);
}

const ModelSlot& find_slot(const Config& config, const std::string& name) {
  if (name == "chief") return config.chief;
  if (name == "prompter") return config.prompter;
  if (name == "coder") return config.coder;
  throw std::runtime_error("Unknown role: " + name);
}

}  // namespace mtrxai
