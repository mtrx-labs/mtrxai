#pragma once

#include <filesystem>
#include <string>

namespace mtrxai {

void write_atomic(const std::filesystem::path& path, const std::string& content,
                  std::filesystem::perms permissions =
                      std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write);

// Appends streamed tokens to an NDJSON file exactly as they arrive, so a
// later resume can rebuild the stage output even if the run is killed. Every
// chunk is flushed at once; the file is fsynced at most about once a second to
// bound the syscall cost. Lines look like
//   {"type":"token","turn":0,"t":123456789,"text":"..."}
// with an optional {"type":"start","turn":N} marker line before a group.
class RawRecorder {
 public:
  explicit RawRecorder(const std::filesystem::path& path);
  ~RawRecorder();
  RawRecorder(const RawRecorder&) = delete;
  RawRecorder& operator=(const RawRecorder&) = delete;

  // Write one token chunk; throws so a broken disk aborts the generation.
  void append(const std::string& text, int turn = 0);
  // Mark the start of a group (for example, a Coder turn).
  void begin_group(int turn);

 private:
  void flush(bool force);

  std::filesystem::path path_;
  int descriptor_ = -1;
  bool open_ = false;
  long long last_sync_ms_ = 0;
};

// Decode a raw.jsonl file: concatenation of every token line's text. When a
// turn is given, only the lines recorded for that group are used.
std::string read_raw_text(const std::filesystem::path& path, int turn = -1);

}  // namespace mtrxai
