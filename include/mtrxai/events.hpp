#pragma once

#include <functional>
#include <string>

namespace mtrxai {

enum class Stage { Chief, Prompter, Coder };

enum class StageStatus { Queued, Loading, Running, Saved, Failed };

struct StreamChunk {
  enum class Kind { Content, Reasoning };
  Kind kind;
  std::string text;
};

// The engine talks to the UI through this interface only. Callbacks may run on
// the worker thread; confirm() must block until the user answers.
struct PipelineEvents {
  std::function<void(Stage, StageStatus, const std::string& note)> on_stage;
  std::function<void(Stage, const StreamChunk&)> on_chunk;
  std::function<bool(const std::string& review_text)> confirm;  // blocking
  std::function<void(const std::string&)> log;
};

enum class RunResult { Finished, Failed, Cancelled };

}  // namespace mtrxai
