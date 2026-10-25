#include "mtrxai/safety.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace mtrxai {

bool sensitive_filename(const std::string& name) {
  // Extract base filename in case a relative path was passed.
  const std::string filename = std::filesystem::path(name).filename().string();
  std::string lower = filename;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });

  // Exact sensitive names.
  if (lower == ".env" || lower == ".netrc" || lower == ".npmrc" ||
      lower == ".pypirc" || lower == "credentials.json") {
    return true;
  }

  // Sensitive prefix patterns (.env.*, id_rsa*, id_ed25519*).
  if (lower.rfind(".env.", 0) == 0 || lower.rfind("id_rsa", 0) == 0 ||
      lower.rfind("id_ed25519", 0) == 0) {
    return true;
  }

  // Sensitive suffix patterns (*.pem, *.key, *.p12, *.pfx).
  for (const auto* suffix : {".pem", ".key", ".p12", ".pfx"}) {
    const std::size_t suffix_len = std::strlen(suffix);
    if (lower.size() >= suffix_len &&
        lower.compare(lower.size() - suffix_len, suffix_len, suffix) == 0) {
      return true;
    }
  }

  return false;
}

std::filesystem::path checked_target(const std::filesystem::path& root,
                                     const std::filesystem::path& relative) {
  if (relative.empty() || relative.is_absolute()) {
    throw std::runtime_error("Path must be relative to the project.");
  }
  const auto target = root / relative;
  for (const auto& component : relative) {
    if (component == "..") {
      throw std::runtime_error(
          "Refusing a path that traverses outside its directory.");
    }
    if (component == ".git" || component == ".mtrxai" ||
        component == "copilot-worktrees" || component == "build" ||
        component == "node_modules" || component == "vendor" ||
        component == "target" || component == "dist") {
      throw std::runtime_error("Refusing to modify protected path: " +
                               relative.string());
    }
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(target, error);
  if (!error && std::filesystem::is_symlink(status)) {
    throw std::runtime_error("Refusing to overwrite a symlink: " +
                             relative.string());
  }
  error.clear();
  const auto resolved = std::filesystem::weakly_canonical(target, error);
  if (error) {
    throw std::runtime_error("Could not resolve proposed path " +
                             relative.string() + ": " + error.message());
  }
  const auto within_root = resolved.lexically_relative(root);
  if (within_root.empty() || *within_root.begin() == "..") {
    throw std::runtime_error("Refusing to write outside the project: " +
                             relative.string());
  }
  if (std::filesystem::exists(target) &&
      !std::filesystem::is_regular_file(target)) {
    throw std::runtime_error("Refusing to replace a non-file: " +
                             relative.string());
  }
  return target;
}

}  // namespace mtrxai
