#pragma once

#include <filesystem>
#include <string>

namespace mtrxai {

void write_atomic(const std::filesystem::path& path, const std::string& content,
                  std::filesystem::perms permissions =
                      std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write);

// Appends HTTP response body bytes without decoding or adding framing.
class RawByteRecorder {
 public:
  explicit RawByteRecorder(const std::filesystem::path& path);
  ~RawByteRecorder();
  RawByteRecorder(const RawByteRecorder&) = delete;
  RawByteRecorder& operator=(const RawByteRecorder&) = delete;

  void append(const std::string& bytes);

 private:
  std::filesystem::path path_;
  int descriptor_ = -1;
  long long last_sync_ms_ = 0;
};

std::string read_raw_bytes(const std::filesystem::path& path);

}  // namespace mtrxai
