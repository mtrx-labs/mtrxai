#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mtrxai {

// One message in the Coder conversation, in the form sent to the model.
// `thinking` is saved only for assistant messages and is not part of the model
// context on later turns.
struct CoderMessage {
  std::string role;  // system, user, or assistant
  std::string content;
  std::string thinking;
  int turn = 0;
};

// Append-only record of the Coder conversation in
// <session>/coder/messages.jsonl. Every message is fsynced, so a crash cannot
// leave a reply on screen that resume does not know about. Command-line tools
// such as `tail -f` can read it; one JSON object is written per line.
class CoderTranscript {
 public:
  explicit CoderTranscript(const std::filesystem::path& path);
  ~CoderTranscript();
  CoderTranscript(const CoderTranscript&) = delete;
  CoderTranscript& operator=(const CoderTranscript&) = delete;

  void append(const CoderMessage& message);

  // Reads every valid message in order. Malformed or unknown-role lines are
  // skipped so a partially written last line does not break resume.
  static std::vector<CoderMessage> load(const std::filesystem::path& path);

 private:
  std::filesystem::path path_;
  int descriptor_ = -1;
};

}  // namespace mtrxai
