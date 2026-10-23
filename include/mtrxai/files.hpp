#pragma once

#include <filesystem>
#include <string>

namespace mtrxai {

void write_atomic(const std::filesystem::path& path, const std::string& content,
                  std::filesystem::perms permissions =
                      std::filesystem::perms::owner_read |
                      std::filesystem::perms::owner_write);

}  // namespace mtrxai
