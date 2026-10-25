#pragma once

#include <filesystem>
#include <string>

namespace mtrxai {

// Returns true when the base filename is a credential or private-key file that
// the Coder must never list, read, or write. Matching is intentionally based on
// exact names and narrow patterns so ordinary source files remain accessible.
bool sensitive_filename(const std::string& name);

// Resolves a Coder-supplied path against the project root and refuses absolute
// paths, traversal, protected directories, symlinks, and non-file replacements.
// The path is not created; callers decide how to use it.
std::filesystem::path checked_target(const std::filesystem::path& root,
                                     const std::filesystem::path& relative);

}  // namespace mtrxai
