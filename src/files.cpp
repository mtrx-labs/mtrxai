#include "mtrxai/files.hpp"

#include <json-c/json.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace mtrxai {
namespace {

mode_t mode_from_permissions(std::filesystem::perms permissions) {
  mode_t mode = 0;
  if ((permissions & std::filesystem::perms::owner_read) !=
      std::filesystem::perms::none) {
    mode |= S_IRUSR;
  }
  if ((permissions & std::filesystem::perms::owner_write) !=
      std::filesystem::perms::none) {
    mode |= S_IWUSR;
  }
  if ((permissions & std::filesystem::perms::owner_exec) !=
      std::filesystem::perms::none) {
    mode |= S_IXUSR;
  }
  if ((permissions & std::filesystem::perms::group_read) !=
      std::filesystem::perms::none) {
    mode |= S_IRGRP;
  }
  if ((permissions & std::filesystem::perms::group_write) !=
      std::filesystem::perms::none) {
    mode |= S_IWGRP;
  }
  if ((permissions & std::filesystem::perms::group_exec) !=
      std::filesystem::perms::none) {
    mode |= S_IXGRP;
  }
  if ((permissions & std::filesystem::perms::others_read) !=
      std::filesystem::perms::none) {
    mode |= S_IROTH;
  }
  if ((permissions & std::filesystem::perms::others_write) !=
      std::filesystem::perms::none) {
    mode |= S_IWOTH;
  }
  if ((permissions & std::filesystem::perms::others_exec) !=
      std::filesystem::perms::none) {
    mode |= S_IXOTH;
  }
  return mode;
}

}  // namespace

void write_atomic(const std::filesystem::path& path, const std::string& content,
                  std::filesystem::perms permissions) {
  std::string pattern = path.string() + ".tmp.XXXXXX";
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  int descriptor = mkstemp(temporary.data());
  if (descriptor < 0) {
    throw std::runtime_error("Could not create temporary file for " +
                             path.string() + ": " + std::strerror(errno));
  }

  bool renamed = false;
  try {
    if (fchmod(descriptor, mode_from_permissions(permissions)) < 0) {
      throw std::runtime_error("Could not set file permissions: " +
                               std::string(std::strerror(errno)));
    }
    std::size_t written = 0;
    while (written < content.size()) {
      const ssize_t count =
          write(descriptor, content.data() + written, content.size() - written);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) {
        throw std::runtime_error("Could not write " + path.string() + ": " +
                                 std::strerror(errno));
      }
      written += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) < 0) {
      throw std::runtime_error("Could not flush " + path.string() + ": " +
                               std::strerror(errno));
    }
    if (close(descriptor) < 0) {
      descriptor = -1;
      throw std::runtime_error("Could not close temporary file: " +
                               std::string(std::strerror(errno)));
    }
    descriptor = -1;

    std::error_code error;
    std::filesystem::rename(temporary.data(), path, error);
    if (error) {
      throw std::runtime_error("Could not save " + path.string() + ": " +
                               error.message());
    }
    renamed = true;

    const auto parent = path.has_parent_path() ? path.parent_path()
                                                : std::filesystem::path(".");
    const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
    if (directory < 0) {
      throw std::runtime_error("Could not open output directory: " +
                               std::string(std::strerror(errno)));
    }
    const int sync_result = fsync(directory);
    const int sync_error = errno;
    close(directory);
    if (sync_result < 0) {
      throw std::runtime_error("Could not flush output directory: " +
                               std::string(std::strerror(sync_error)));
    }
  } catch (...) {
    if (descriptor >= 0) close(descriptor);
    if (!renamed) unlink(temporary.data());
    throw;
  }
}

namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};
using OwnedJson = std::unique_ptr<json_object, JsonDeleter>;

long long wall_clock_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

RawRecorder::RawRecorder(const std::filesystem::path& path) : path_(path) {
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create raw log directory: " +
                             error.message());
  }
  descriptor_ = open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0600);
  if (descriptor_ < 0) {
    throw std::runtime_error("Could not open raw log file " + path_.string() +
                             ": " + std::strerror(errno));
  }
  open_ = true;
}

RawRecorder::~RawRecorder() {
  // A final sync runs even when the caller forgot finish(); on error paths we
  // still want everything we have on stable storage.
  if (open_) {
    flush(true);
    close(descriptor_);
  }
}

void RawRecorder::flush(bool force) {
  if (!open_) return;
  const long long now = wall_clock_ms();
  if (!force && last_sync_ms_ != 0 && now - last_sync_ms_ < 800) return;
  if (fsync(descriptor_) == 0) last_sync_ms_ = now;
  // A failed fsync is not fatal by itself: the data is already in the page
  // cache and the next attempt will retry after ~1 s.
}

void RawRecorder::append(const std::string& text, int turn) {
  OwnedJson line(json_object_new_object());
  json_object_object_add(line.get(), "type",
                         json_object_new_string("token"));
  json_object_object_add(line.get(), "turn", json_object_new_int(turn));
  json_object_object_add(line.get(), "t", json_object_new_int64(wall_clock_ms()));
  json_object_object_add(line.get(), "text",
                         json_object_new_string(text.c_str()));
  std::string serialized =
      json_object_to_json_string_ext(line.get(), JSON_C_TO_STRING_PLAIN) +
      std::string("\n");
  std::size_t written = 0;
  while (written < serialized.size()) {
    const ssize_t count = write(descriptor_, serialized.data() + written,
                                serialized.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      throw std::runtime_error("Could not append to raw log " + path_.string() +
                               ": " + std::strerror(errno));
    }
    written += static_cast<std::size_t>(count);
  }
  flush(false);
}

void RawRecorder::begin_group(int turn) {
  OwnedJson line(json_object_new_object());
  json_object_object_add(line.get(), "type",
                         json_object_new_string("start"));
  json_object_object_add(line.get(), "turn", json_object_new_int(turn));
  json_object_object_add(line.get(), "t", json_object_new_int64(wall_clock_ms()));
  const std::string serialized =
      std::string(json_object_to_json_string_ext(line.get(),
                                                 JSON_C_TO_STRING_PLAIN)) + "\n";
  const ssize_t count = write(descriptor_, serialized.data(), serialized.size());
  if (count < 0) {
    throw std::runtime_error("Could not append to raw log " + path_.string() +
                             ": " + std::strerror(errno));
  }
  flush(false);
}

std::string read_raw_text(const std::filesystem::path& path, int turn) {
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) return {};
  const int descriptor = open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    throw std::runtime_error("Could not read raw log " + path.string() + ": " +
                             std::strerror(errno));
  }
  std::string buffer;
  char chunk[8192];
  while (true) {
    const ssize_t count = read(descriptor, chunk, sizeof(chunk));
    if (count < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      throw std::runtime_error("Could not read raw log " + path.string() + ": " +
                               std::strerror(errno));
    }
    if (count == 0) break;
    buffer.append(chunk, static_cast<std::size_t>(count));
  }
  close(descriptor);

  std::string text;
  std::size_t position = 0;
  while (position < buffer.size()) {
    const std::size_t newline = buffer.find('\n', position);
    const std::size_t end = newline == std::string::npos ? buffer.size() : newline;
    const std::string line = buffer.substr(position, end - position);
    position = newline == std::string::npos ? buffer.size() : newline + 1;
    if (line.empty()) continue;
    OwnedJson parsed(json_tokener_parse(line.c_str()));
    if (!parsed) continue;
    json_object* record_type = nullptr;
    if (json_object_object_get_ex(parsed.get(), "type", &record_type) &&
        std::string(json_object_get_string(record_type)) != "token") {
      continue;
    }
    if (turn >= 0) {
      json_object* line_turn = nullptr;
      if (json_object_object_get_ex(parsed.get(), "turn", &line_turn) &&
          json_object_get_int(line_turn) != turn) {
        continue;
      }
    }
    json_object* value = nullptr;
    if (json_object_object_get_ex(parsed.get(), "text", &value) &&
        json_object_is_type(value, json_type_string)) {
      text += json_object_get_string(value);
    }
  }
  return text;
}

}  // namespace mtrxai
