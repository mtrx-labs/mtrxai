#pragma once

#include "mtrxai/events.hpp"

#include <functional>
#include <string>

namespace mtrxai {

enum class ProviderKind { Ollama, OpenAI, Anthropic, GGUF };

ProviderKind provider_kind(const std::string& name);
std::string provider_kind_name(ProviderKind kind);

struct ReplayResult {
  std::string content;
  std::string reasoning;
};

// Incremental decoder for the exact response-body bytes saved by the caller.
// replay() uses this same decoder for completed files.
class ReplayParser {
 public:
  using ChunkSink = std::function<void(const StreamChunk&)>;

  explicit ReplayParser(ProviderKind kind);
  void append(const std::string& bytes, const ChunkSink& on_chunk);
  void finish(const ChunkSink& on_chunk);
  const ReplayResult& result() const noexcept { return result_; }
  const std::string& error() const noexcept { return error_; }

 private:
  void line(const std::string& line, const ChunkSink& on_chunk);
  void emit(StreamChunk::Kind kind, const std::string& text,
            const ChunkSink& on_chunk);
  void emit_decoded(StreamChunk::Kind kind, const std::string& text,
                    const ChunkSink& on_chunk);
  void tagged_content(const std::string& text, bool final,
                     const ChunkSink& on_chunk);

  ProviderKind kind_;
  std::string pending_;
  std::string event_;
  std::string text_pending_;
  ReplayResult result_;
  std::string error_;
  bool inside_think_ = false;
  bool finished_ = false;
};

ReplayResult replay(const std::string& raw_bytes, ProviderKind kind);

}  // namespace mtrxai
