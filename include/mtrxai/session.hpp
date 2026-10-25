#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mtrxai {

// A saved pipeline run. `stage` is the last completed stage from
// manifest.json: "request", "chief", "prompter", "coder-turn-N" or "finished".
// It is empty when the manifest is missing or unreadable.
struct SessionInfo {
  std::filesystem::path path;
  std::string id;  // directory name, for example 20261025-120000-123
  std::string stage;
  std::string updated_at;
};

// Directory that contains every session directory.
std::filesystem::path sessions_root();

// Newest first. Directories without a request.md are ignored, so unrelated
// entries in the state directory cannot be resumed by accident.
std::vector<SessionInfo> list_sessions();
std::vector<SessionInfo> list_sessions(const std::filesystem::path& root);

// The argument may be a session id (the directory name) or an existing session
// directory. Returns nothing when no session with a request.md matches.
std::optional<SessionInfo> find_session(const std::string& id_or_path);

// The most recent session that has not reached the "finished" stage. A missing
// or unreadable manifest counts as unfinished because the run may still have
// work to do.
std::optional<SessionInfo> latest_unfinished_session();
std::optional<SessionInfo> latest_unfinished_session(
    const std::filesystem::path& root);

bool session_finished(const SessionInfo& session);

// Reads request.md. Throws when the file cannot be read.
std::string read_session_request(const std::filesystem::path& session);

// Parses manifest.json and returns completed_stage; empty when the JSON is
// malformed or the field is absent.
std::string parse_manifest_stage(const std::string& json);

}  // namespace mtrxai
