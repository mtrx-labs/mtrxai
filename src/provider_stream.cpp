#include "mtrxai/provider_stream.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

namespace mtrxai {
namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

std::string field(json_object* object, const char* key) {
  json_object* value = nullptr;
  if (object == nullptr || !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return {};
  }
  return json_object_get_string(value);
}

std::string trim(std::string value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value[0]))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string sse_data(const std::string& line) {
  if (line.compare(0, 5, "data:") != 0) return {};
  return trim(line.substr(5));
}

std::string json_error(json_object* parsed) {
  json_object* error = nullptr;
  if (!json_object_object_get_ex(parsed, "error", &error)) return {};
  if (json_object_is_type(error, json_type_string)) {
    return json_object_get_string(error);
  }
  return field(error, "message");
}

std::size_t find_ci(const std::string& value, const std::string& needle) {
  for (std::size_t start = 0; start + needle.size() <= value.size(); ++start) {
    bool matches = true;
    for (std::size_t index = 0; index < needle.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(value[start + index])) !=
          std::tolower(static_cast<unsigned char>(needle[index]))) {
        matches = false;
        break;
      }
    }
    if (matches) return start;
  }
  return std::string::npos;
}

}  // namespace

ProviderKind provider_kind(const std::string& name) {
  if (name == "ollama") return ProviderKind::Ollama;
  if (name == "anthropic") return ProviderKind::Anthropic;
  if (name == "gguf") return ProviderKind::GGUF;
  return ProviderKind::OpenAI;
}

std::string provider_kind_name(ProviderKind kind) {
  switch (kind) {
    case ProviderKind::Ollama: return "ollama";
    case ProviderKind::OpenAI: return "openai";
    case ProviderKind::Anthropic: return "anthropic";
    case ProviderKind::GGUF: return "gguf";
  }
  return "openai";
}

ReplayParser::ReplayParser(ProviderKind kind) : kind_(kind) {}

void ReplayParser::emit(StreamChunk::Kind kind, const std::string& text,
                        const ChunkSink& on_chunk) {
  if (kind == StreamChunk::Kind::Content) {
    tagged_content(text, false, on_chunk);
  } else {
    emit_decoded(kind, text, on_chunk);
  }
}

void ReplayParser::emit_decoded(StreamChunk::Kind kind, const std::string& text,
                                const ChunkSink& on_chunk) {
  if (text.empty()) return;
  if (kind == StreamChunk::Kind::Content) {
    result_.content += text;
  } else {
    result_.reasoning += text;
  }
  if (on_chunk) on_chunk(StreamChunk{kind, text});
}

void ReplayParser::tagged_content(const std::string& text, bool final,
                                  const ChunkSink& on_chunk) {
  text_pending_ += text;
  const std::string open = "<think>";
  const std::string close = "</think>";
  const std::string legacy_open = " thinking";
  while (!text_pending_.empty()) {
    if (inside_think_) {
      const std::size_t end = find_ci(text_pending_, close);
      if (end != std::string::npos) {
        emit_decoded(StreamChunk::Kind::Reasoning,
                     text_pending_.substr(0, end), on_chunk);
        text_pending_.erase(0, end + close.size());
        inside_think_ = false;
        continue;
      }
      if (final) {
        emit_decoded(StreamChunk::Kind::Reasoning, text_pending_, on_chunk);
        text_pending_.clear();
        continue;
      }
      const std::size_t safe = text_pending_.size() >= close.size()
                                   ? text_pending_.size() - close.size() + 1
                                   : 0;
      if (safe != 0) {
        emit_decoded(StreamChunk::Kind::Reasoning,
                     text_pending_.substr(0, safe), on_chunk);
        text_pending_.erase(0, safe);
      }
      return;
    }

    std::size_t open_at = find_ci(text_pending_, open);
    std::size_t open_size = open.size();
    const std::size_t legacy_at = find_ci(text_pending_, legacy_open);
    if (legacy_at == 0 && (open_at == std::string::npos || legacy_at < open_at)) {
      open_at = legacy_at;
      open_size = legacy_open.size();
    }
    if (open_at != std::string::npos) {
      emit_decoded(StreamChunk::Kind::Content,
                   text_pending_.substr(0, open_at), on_chunk);
      text_pending_.erase(0, open_at + open_size);
      inside_think_ = true;
      continue;
    }

    if (final) {
      emit_decoded(StreamChunk::Kind::Content, text_pending_, on_chunk);
      text_pending_.clear();
      return;
    }
    const std::size_t keep =
        std::max({open.size(), close.size(), legacy_open.size()}) - 1;
    if (text_pending_.size() > keep) {
      const std::size_t safe = text_pending_.size() - keep;
      emit_decoded(StreamChunk::Kind::Content,
                   text_pending_.substr(0, safe), on_chunk);
      text_pending_.erase(0, safe);
    }
    return;
  }
}

void ReplayParser::line(const std::string& line_value,
                        const ChunkSink& on_chunk) {
  std::string payload;
  if (kind_ == ProviderKind::Ollama) {
    if (trim(line_value).empty()) return;
    payload = line_value;
  } else {
    payload = sse_data(line_value);
    if (payload.empty() || payload == "[DONE]") return;
  }

  Json parsed(json_tokener_parse(payload.c_str()));
  if (!parsed) return;
  const std::string issue = json_error(parsed.get());
  if (!issue.empty()) {
    if (result_.content.empty()) error_ = issue;
    return;
  }

  if (kind_ == ProviderKind::Ollama) {
    json_object* message = nullptr;
    if (!json_object_object_get_ex(parsed.get(), "message", &message) ||
        !json_object_is_type(message, json_type_object)) {
      return;
    }
    std::string reasoning = field(message, "thinking");
    if (reasoning.empty()) reasoning = field(message, "reasoning");
    emit(StreamChunk::Kind::Reasoning, reasoning, on_chunk);
    emit(StreamChunk::Kind::Content, field(message, "content"), on_chunk);
    return;
  }

  if (kind_ == ProviderKind::Anthropic) {
    if (field(parsed.get(), "type") != "content_block_delta") return;
    json_object* delta = nullptr;
    if (!json_object_object_get_ex(parsed.get(), "delta", &delta) ||
        !json_object_is_type(delta, json_type_object)) {
      return;
    }
    const std::string type = field(delta, "type");
    if (type == "text_delta") {
      emit(StreamChunk::Kind::Content, field(delta, "text"), on_chunk);
    } else if (type == "thinking_delta") {
      emit(StreamChunk::Kind::Reasoning, field(delta, "thinking"), on_chunk);
    }
    return;
  }

  json_object* choices = nullptr;
  if (!json_object_object_get_ex(parsed.get(), "choices", &choices) ||
      !json_object_is_type(choices, json_type_array) ||
      json_object_array_length(choices) == 0) {
    return;
  }
  json_object* choice = json_object_array_get_idx(choices, 0);
  json_object* delta = nullptr;
  if (choice == nullptr ||
      !json_object_object_get_ex(choice, "delta", &delta) ||
      !json_object_is_type(delta, json_type_object)) {
    return;
  }
  std::string reasoning = field(delta, "reasoning_content");
  if (reasoning.empty()) reasoning = field(delta, "reasoning");
  emit(StreamChunk::Kind::Reasoning, reasoning, on_chunk);
  emit(StreamChunk::Kind::Content, field(delta, "content"), on_chunk);
}

void ReplayParser::append(const std::string& bytes, const ChunkSink& on_chunk) {
  if (finished_) throw std::logic_error("Cannot append after stream finish");
  constexpr std::size_t max_stream = 4 * 1024 * 1024;
  if (bytes.size() > max_stream - pending_.size()) {
    throw std::runtime_error("Streaming response exceeded the 4 MiB limit");
  }
  pending_.append(bytes);
  std::size_t newline = pending_.find_first_of("\r\n");
  while (newline != std::string::npos) {
    if (pending_[newline] == '\r' && newline + 1 == pending_.size()) break;
    std::string current = pending_.substr(0, newline);
    const std::size_t delimiter_size =
        pending_[newline] == '\r' && pending_[newline + 1] == '\n' ? 2 : 1;
    pending_.erase(0, newline + delimiter_size);
    line(current, on_chunk);
    newline = pending_.find_first_of("\r\n");
  }
}

void ReplayParser::finish(const ChunkSink& on_chunk) {
  if (finished_) return;
  if (!pending_.empty()) {
    if (pending_.back() == '\r') pending_.pop_back();
    line(pending_, on_chunk);
    pending_.clear();
  }
  tagged_content({}, true, on_chunk);
  finished_ = true;
  if (!error_.empty()) {
    throw std::runtime_error("Model stream reported an error: " + error_);
  }
}

ReplayResult replay(const std::string& raw_bytes, ProviderKind kind) {
  ReplayParser parser(kind);
  parser.append(raw_bytes, {});
  parser.finish({});
  return parser.result();
}

}  // namespace mtrxai
