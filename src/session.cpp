#include "mtrxai/session.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace mtrxai {
namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("Could not read " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

// Reads the small manifest without trusting it: a killed process can leave a
// truncated or hand-edited file, and that must not make listing sessions fail.
SessionInfo inspect_session(const std::filesystem::path& directory,
                            const std::string& id) {
  SessionInfo info;
  info.path = directory;
  info.id = id;
  std::error_code error;
  const auto manifest = directory / "manifest.json";
  if (!std::filesystem::is_regular_file(manifest, error) || error) {
    return info;
  }
  try {
    const std::string text = read_file(manifest);
    info.stage = parse_manifest_stage(text);

    Json root(json_tokener_parse(text.c_str()));
    if (root == nullptr) return info;
    json_object* updated = nullptr;
    if (json_object_object_get_ex(root.get(), "updated_at", &updated) &&
        json_object_is_type(updated, json_type_string)) {
      info.updated_at = json_object_get_string(updated);
    }
  } catch (const std::exception&) {
    // An unreadable manifest still leaves a resumable request. Keep the
    // session visible with an empty stage.
  }
  return info;
}

std::vector<SessionInfo> collect_sessions(const std::filesystem::path& root) {
  std::vector<SessionInfo> sessions;
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error) {
    return sessions;
  }
  std::filesystem::directory_iterator iterator(root, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const std::filesystem::directory_entry entry = *iterator;
    iterator.increment(error);
    std::error_code type_error;
    if (!entry.is_directory(type_error) || type_error) continue;

    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(entry.path() / "request.md",
                                          exists_error) ||
        exists_error) {
      continue;
    }
    sessions.push_back(
        inspect_session(entry.path(), entry.path().filename().string()));
  }
  // Session ids are zero-padded timestamps, so a plain string sort is
  // newest-first. Sorting by name also keeps ordering stable when two
  // manifests were written in the same second.
  std::sort(sessions.begin(), sessions.end(),
            [](const SessionInfo& left, const SessionInfo& right) {
              return left.id > right.id;
            });
  return sessions;
}

}  // namespace

std::filesystem::path sessions_root() {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "mtrxai" / "sessions";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; cannot locate sessions");
  }
  return std::filesystem::path(home) / ".local" / "state" / "mtrxai" /
         "sessions";
}

std::vector<SessionInfo> list_sessions() { return collect_sessions(sessions_root()); }

std::vector<SessionInfo> list_sessions(const std::filesystem::path& root) {
  return collect_sessions(root);
}

std::optional<SessionInfo> find_session(const std::string& id_or_path) {
  if (id_or_path.empty()) return std::nullopt;
  // A slash means the caller copied a session path from the status message.
  // Anything else is matched against directory names only, never joined into a
  // path, so a value like "../../etc" cannot escape the sessions directory.
  if (id_or_path.find('/') != std::string::npos) {
    const std::filesystem::path directory(id_or_path);
    std::error_code error;
    if (!std::filesystem::is_regular_file(directory / "request.md", error) ||
        error) {
      return std::nullopt;
    }
    return inspect_session(directory, directory.filename().string());
  }
  for (const auto& session : list_sessions()) {
    if (session.id == id_or_path) return session;
  }
  return std::nullopt;
}

std::optional<SessionInfo> latest_unfinished_session() {
  return latest_unfinished_session(sessions_root());
}

std::optional<SessionInfo> latest_unfinished_session(
    const std::filesystem::path& root) {
  for (const auto& session : list_sessions(root)) {
    if (!session_finished(session)) return session;
  }
  return std::nullopt;
}

bool session_finished(const SessionInfo& session) {
  return session.stage == "finished";
}

std::string read_session_request(const std::filesystem::path& session) {
  const std::string request = read_file(session / "request.md");
  if (request.empty()) {
    throw std::runtime_error("Session request is empty: " +
                             (session / "request.md").string());
  }
  return request;
}

std::string parse_manifest_stage(const std::string& json) {
  Json root(json_tokener_parse(json.c_str()));
  if (root == nullptr) return {};
  json_object* stage = nullptr;
  if (!json_object_object_get_ex(root.get(), "completed_stage", &stage) ||
      !json_object_is_type(stage, json_type_string)) {
    return {};
  }
  return json_object_get_string(stage);
}

}  // namespace mtrxai
