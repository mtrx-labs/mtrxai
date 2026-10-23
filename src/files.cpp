#include "mtrxai/files.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
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

}  // namespace mtrxai
