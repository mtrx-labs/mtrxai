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

// Record of the Coder conversation in <session>/coder/messages.jsonl. Messages
// are appended and fsynced, so a crash cannot leave a reply on screen that
// resume does not know about. A resume that drops an unusable saved reply
// rewrites the file atomically. Command-line tools such as `tail -f` can read
// it; one JSON object is written per line.
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

  // Rewrites the whole transcript atomically. Resume uses this when it has to
  // drop a saved reply, so a crash during the repair cannot leave the old
  // conversation behind. Do not call it while a CoderTranscript for the same
  // path is open: that descriptor would still point at the replaced file.
  static void save(const std::filesystem::path& path,
                   const std::vector<CoderMessage>& messages);

 private:
  std::filesystem::path path_;
  int descriptor_ = -1;
};

}  // namespace mtrxai
