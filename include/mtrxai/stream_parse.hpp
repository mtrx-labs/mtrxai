#pragma once

// Pure line parsers for the three streaming protocols mtrxAI consumes.
// Ollama native chat streams NDJSON; OpenAI-compatible endpoints and Anthropic
// stream SSE. Kept header-only so unit tests can exercise them without a
// running server.

#include <json-c/json.h>

#include <cctype>
#include <string>

namespace mtrxai {
namespace stream {

// The special value "" means "this payload contributes no text"; callers decide
// whether that is normal (keepalives, stats lines) or an error.

inline std::string trim(const std::string& value) {
  std::size_t begin = 0;
  while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
    --end;
  }
  return value.substr(begin, end - begin);
}

// An SSE line looks like "data: {...}"; comments ("data:") and other lines
// (event:, : ping) carry no payload.
inline std::string sse_payload(const std::string& line) {
  if (line.rfind("data:", 0) != 0) return "";
  return trim(line.substr(5));
}

// Field from a parsed JSON object, or "" when absent or not a string.
inline std::string field_string(json_object* object, const char* key) {
  json_object* value = nullptr;
  if (object == nullptr || !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return "";
  }
  return json_object_get_string(value);
}

// Ollama chat NDJSON line -> message.content delta ("" on the final stats line).
inline std::string ollama_delta_text(const std::string& line) {
  json_object* parsed = json_tokener_parse(line.c_str());
  if (parsed == nullptr) return "";
  std::string delta;
  json_object* message = nullptr;
  if (json_object_object_get_ex(parsed, "message", &message) &&
      json_object_is_type(message, json_type_object)) {
    delta = field_string(message, "content");
  }
  json_object_put(parsed);
  return delta;
}

// OpenAI streaming chunk -> choices[0].delta.content.
inline std::string openai_delta_text(const std::string& payload) {
  json_object* parsed = json_tokener_parse(payload.c_str());
  if (parsed == nullptr) return "";
  json_object* choices = nullptr;
  std::string delta;
  if (json_object_object_get_ex(parsed, "choices", &choices) &&
      json_object_is_type(choices, json_type_array) &&
      json_object_array_length(choices) > 0) {
    json_object* choice = json_object_array_get_idx(choices, 0);
    json_object* delta_object = nullptr;
    if (choice != nullptr &&
        json_object_object_get_ex(choice, "delta", &delta_object) &&
        json_object_is_type(delta_object, json_type_object)) {
      delta = field_string(delta_object, "content");
    }
  }
  json_object_put(parsed);
  return delta;
}

// Anthropic streaming event -> delta text from content_block_delta events.
inline std::string anthropic_delta_text(const std::string& payload) {
  json_object* parsed = json_tokener_parse(payload.c_str());
  if (parsed == nullptr) return "";
  std::string text;
  if (field_string(parsed, "type") == "content_block_delta") {
    json_object* delta = nullptr;
    if (json_object_object_get_ex(parsed, "delta", &delta) &&
        json_object_is_type(delta, json_type_object) &&
        field_string(delta, "type") == "text_delta") {
      text = field_string(delta, "text");
    }
  }
  json_object_put(parsed);
  return text;
}

// Error text embedded in a stream payload, or "" when there is none. Works for
// Ollama {"error": ...}, OpenAI {"error": {..., "message": ...}} and Anthropic
// {"type": "error", "error": {..., "message": ...}} shapes.
inline std::string error_message(const std::string& payload) {
  json_object* parsed = json_tokener_parse(payload.c_str());
  if (parsed == nullptr) return "";
  std::string message;
  json_object* error = nullptr;
  if (json_object_object_get_ex(parsed, "error", &error)) {
    if (json_object_is_type(error, json_type_string)) {
      message = json_object_get_string(error);
    } else if (json_object_is_type(error, json_type_object)) {
      message = field_string(error, "message");
    }
  }
  json_object_put(parsed);
  return message;
}

}  // namespace stream
}  // namespace mtrxai
