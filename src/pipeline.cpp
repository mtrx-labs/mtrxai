#include "mtrxai/pipeline.hpp"

#include "mtrxai/coder_transcript.hpp"
#include "mtrxai/files.hpp"
#include "mtrxai/provider.hpp"
#include "mtrxai/safety.hpp"
#include "mtrxai/session.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/textutil.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace mtrxai {

ThinkingResult extract_thinking(const std::string& text) {
  ThinkingResult result;
  std::size_t pos = 0;
  const std::string open_tag = "<think>";
  const std::string close_tag = "</think>";

  auto find_ci = [](const std::string& haystack, const std::string& needle,
                    std::size_t start) {
    if (needle.empty() || start >= haystack.size()) return std::string::npos;
    auto it = std::search(
        haystack.begin() + start, haystack.end(), needle.begin(), needle.end(),
        [](char c1, char c2) {
          return std::tolower(static_cast<unsigned char>(c1)) ==
                 std::tolower(static_cast<unsigned char>(c2));
        });
    if (it == haystack.end()) return std::string::npos;
    return static_cast<std::size_t>(std::distance(haystack.begin(), it));
  };

  while (pos < text.size()) {
    const std::size_t open_pos = find_ci(text, open_tag, pos);
    if (open_pos == std::string::npos) {
      result.content.append(text.substr(pos));
      break;
    }
    result.content.append(text.substr(pos, open_pos - pos));
    const std::size_t think_start = open_pos + open_tag.size();
    const std::size_t close_pos = find_ci(text, close_tag, think_start);
    if (close_pos == std::string::npos) {
      const std::string block = text.substr(think_start);
      if (!block.empty()) {
        if (!result.thinking.empty()) result.thinking += "\n\n";
        result.thinking += block;
      }
      pos = text.size();
      break;
    } else {
      const std::string block = text.substr(think_start, close_pos - think_start);
      if (!block.empty()) {
        if (!result.thinking.empty()) result.thinking += "\n\n";
        result.thinking += block;
      }
      pos = close_pos + close_tag.size();
    }
  }

  auto trim = [](std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
  };

  result.content = trim(result.content);
  result.thinking = trim(result.thinking);
  return result;
}

namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch())
          .count() %
      1000;
  std::tm local_time{};
  localtime_r(&time, &local_time);
  std::ostringstream result;
  result << std::put_time(&local_time, "%Y%m%d-%H%M%S") << '-'
         << std::setw(3) << std::setfill('0') << milliseconds;
  return result.str();
}

std::string chief_instructions() {
  return "You are the Chief of Staff in a sequential coding-agent team. Analyze "
         "the user's request, identify its intent, requirements, assumptions, "
         "constraints, and acceptance criteria, and prepare a concise structured "
         "brief for a prompt specialist. Do not write code or claim work is "
         "complete. Keep requirements faithful to the user.";
}

std::string prompter_instructions() {
  return "You are the prompt specialist. Turn the original request and Chief of "
         "Staff brief into a detailed, unambiguous implementation prompt for a "
         "coding AI. Specify deliverables, acceptance criteria, relevant "
         "constraints, and sensible verification. Preserve the user's scope; do "
         "not invent requirements or claim code has been changed.";
}

std::string coder_instructions() {
  return "You are the coding agent for the project in the current working "
         "directory. Work iteratively: inspect files, make focused edits, run "
         "relevant checks, fix failures, and finish only when the request is "
         "implemented and verified. Reply with exactly one JSON object per turn "
         "and no markdown fences. Supported actions:\n"
         "{\"action\":\"list_files\",\"path\":\"relative/directory\"}\n"
         "{\"action\":\"read_file\",\"path\":\"relative/file\",\"offset\":0}\n"
         "{\"action\":\"write_file\",\"path\":\"relative/file\",\"content\":\"complete file contents\"}\n"
         "{\"action\":\"run_command\",\"command\":\"cmake --build build\",\"timeout_seconds\":120}\n"
         "{\"action\":\"finish\",\"summary\":\"what changed and which checks ran\"}\n"
         "Paths are relative to the project. Never read or write .git, .mtrxai, "
         "copilot-worktrees, build, node_modules, vendor, target, or dist. "
         "Read files before changing them. A write is saved only after the user "
         "approves the exact path. Shell commands run only after the user "
         "approves the exact command; use them for builds/tests, never to read "
         "secrets or access unrelated locations. A command is limited to 300 "
         "seconds and its output is truncated. Do not claim a check passed "
         "unless its tool result confirms success. Never claim a change was "
         "saved if approval was declined.";
}

struct CoderAction {
  std::string name;
  std::string path;
  std::string content;
  std::string command;
  std::string summary;
  int timeout_seconds = 120;
  std::uint64_t offset = 0;
};


std::string workspace_snapshot(const std::filesystem::path& root) {
  static const std::set<std::string> skipped_directories = {
      ".git", ".mtrxai", "build", "cmake-build-debug", "cmake-build-release",
      "copilot-worktrees", "dist", "node_modules", "target", "vendor"};

  std::vector<std::filesystem::path> paths;
  for (std::filesystem::recursive_directory_iterator it(root), end; it != end;
       ++it) {
    const auto name = it->path().filename().string();
    if (it->is_directory()) {
      if (skipped_directories.count(name) != 0) it.disable_recursion_pending();
      continue;
    }
    if (it->is_symlink() || !it->is_regular_file() || sensitive_filename(name)) {
      continue;
    }
    const auto relative = std::filesystem::relative(it->path(), root);
    paths.push_back(relative);
  }
  std::sort(paths.begin(), paths.end());

  std::ostringstream result;
  result << "Project root: current working directory\nProject file list:\n";
  const std::size_t listed = std::min<std::size_t>(paths.size(), 40);
  for (std::size_t index = 0; index < listed; ++index) {
    result << "  " << paths[index].generic_string() << '\n';
  }
  if (paths.size() > listed) result << "  ... file list truncated\n";

  result << "\nUse read_file to inspect relevant project files before editing.\n";
  return result.str();
}

std::string required_string(json_object* root, const char* key) {
  json_object* value = nullptr;
  if (!json_object_object_get_ex(root, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    throw std::runtime_error(std::string("Coder action requires string field: ") + key);
  }
  return json_object_get_string(value);
}

CoderAction parse_action(const std::string& output) {
  const std::string json = extract_json(output);
  Json root(json_tokener_parse(json.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("Coder response must be a JSON action. See coder.md.");
  }
  CoderAction action;
  action.name = required_string(root.get(), "action");
  auto optional_string = [&root](const char* key) {
    json_object* value = nullptr;
    if (!json_object_object_get_ex(root.get(), key, &value)) return std::string();
    if (!json_object_is_type(value, json_type_string)) {
      throw std::runtime_error(std::string("Coder action field must be a string: ") +
                               key);
    }
    return std::string(json_object_get_string(value));
  };
  action.path = optional_string("path");
  action.content = optional_string("content");
  action.command = optional_string("command");
  action.summary = optional_string("summary");
  json_object* timeout = nullptr;
  if (json_object_object_get_ex(root.get(), "timeout_seconds", &timeout)) {
    if (!json_object_is_type(timeout, json_type_int)) {
      throw std::runtime_error("timeout_seconds must be an integer");
    }
    action.timeout_seconds = json_object_get_int(timeout);
  }
  json_object* offset = nullptr;
  if (json_object_object_get_ex(root.get(), "offset", &offset)) {
    if (!json_object_is_type(offset, json_type_int) ||
        json_object_get_int64(offset) < 0) {
      throw std::runtime_error("offset must be a non-negative integer");
    }
    action.offset = static_cast<std::uint64_t>(json_object_get_int64(offset));
  }
  return action;
}

bool supported_action(const std::string& name) {
  return name == "list_files" || name == "read_file" ||
         name == "write_file" || name == "run_command" || name == "finish";
}

// An empty result means the reply is a usable action. Otherwise the text is a
// short reason that can be shown to the user and sent back to the model.
std::string action_rejection(const std::string& output) {
  try {
    const CoderAction action = parse_action(output);
    if (!supported_action(action.name)) {
      return "Unsupported coder action: " + action.name;
    }
    return {};
  } catch (const std::exception& error) {
    return error.what();
  }
}

std::string rejection_correction(const std::string& rejection) {
  return "Your previous reply was rejected: " + rejection +
         "\n\nReply with exactly one JSON object and nothing else. Use only "
         "the supported actions from the system prompt, with no markdown "
         "fences or extra text.";
}

std::string list_files(const std::filesystem::path& root,
                       const std::filesystem::path& relative) {
  const auto directory = checked_target(root, relative);
  if (!std::filesystem::is_directory(directory)) {
    throw std::runtime_error("Not a project directory: " + relative.string());
  }
  std::vector<std::string> paths;
  static const std::set<std::string> skipped = {
      ".git", ".mtrxai", "copilot-worktrees", "build", "node_modules", "vendor",
      "target", "dist"};
  for (std::filesystem::recursive_directory_iterator it(directory), end; it != end;
       ++it) {
    const auto name = it->path().filename().string();
    if (it->is_directory() && skipped.count(name) != 0) {
      it.disable_recursion_pending();
      continue;
    }
    if (it->is_symlink()) {
      if (it->is_directory()) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file() || sensitive_filename(name)) continue;
    paths.push_back(std::filesystem::relative(it->path(), root).generic_string());
  }
  std::sort(paths.begin(), paths.end());
  std::ostringstream output;
  const std::size_t limit = std::min<std::size_t>(paths.size(), 500);
  for (std::size_t index = 0; index < limit; ++index) output << paths[index] << '\n';
  if (paths.size() > limit) output << "... file list truncated at 500 entries\n";
  return output.str().empty() ? "(no project files)\n" : output.str();
}

std::string read_file(const std::filesystem::path& root,
                      const std::filesystem::path& relative,
                      std::uint64_t offset) {
  if (sensitive_filename(relative.filename().string())) {
    throw std::runtime_error("Refusing to read a sensitive file: " +
                             relative.string());
  }
  const auto target = checked_target(root, relative);
  if (!std::filesystem::is_regular_file(target)) {
    throw std::runtime_error("Not a regular project file: " + relative.string());
  }
  const auto size = std::filesystem::file_size(target);
  if (size > 2 * 1024 * 1024) {
    throw std::runtime_error("File exceeds the 2 MiB read limit: " +
                             relative.string());
  }
  if (offset > size) {
    throw std::runtime_error("Read offset is past end of file: " + relative.string());
  }
  std::ifstream file(target, std::ios::binary);
  if (!file) throw std::runtime_error("Could not read " + relative.string());
  constexpr std::size_t chunk_size = 4000;
  const auto count = static_cast<std::size_t>(
      std::min<std::uint64_t>(chunk_size, size - offset));
  file.seekg(static_cast<std::streamoff>(offset));
  std::string content(count, '\0');
  file.read(content.data(), static_cast<std::streamsize>(count));
  if (file.gcount() != static_cast<std::streamsize>(count)) {
    throw std::runtime_error("Could not read requested file range: " +
                             relative.string());
  }
  if (content.find('\0') != std::string::npos) {
    throw std::runtime_error("Refusing to read a binary file: " + relative.string());
  }
  const auto end = offset + count;
  return "[bytes " + std::to_string(offset) + "-" + std::to_string(end) +
         " of " + std::to_string(size) + "; next_offset=" +
         std::to_string(end) + "]\n" + content;
}

std::string run_command(const std::filesystem::path& root,
                        const std::string& command, int timeout_seconds) {
  if (command.empty() || command.size() > 4096) {
    throw std::runtime_error("Command must be between 1 and 4096 characters.");
  }
  timeout_seconds = std::clamp(timeout_seconds, 1, 300);
  int output_pipe[2];
  if (pipe(output_pipe) != 0) {
    throw std::runtime_error("Could not create command output pipe.");
  }
  static const std::vector<std::string> allowed_environment = {
      "HOME", "PATH", "LANG", "LC_ALL", "TERM", "TMPDIR", "CC", "CXX",
      "CFLAGS", "CXXFLAGS", "LDFLAGS", "PKG_CONFIG_PATH", "CMAKE_PREFIX_PATH",
      "CMAKE_GENERATOR", "VIRTUAL_ENV", "CONDA_PREFIX"};
  std::vector<std::string> environment;
  for (const auto& name : allowed_environment) {
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) environment.push_back(name + "=" + value);
  }
  if (std::none_of(environment.begin(), environment.end(),
                   [](const std::string& entry) {
                     return entry.rfind("PATH=", 0) == 0;
                   })) {
    environment.emplace_back("PATH=/usr/local/bin:/usr/bin:/bin");
  }
  std::vector<char*> environment_pointers;
  environment_pointers.reserve(environment.size() + 1);
  for (auto& entry : environment) environment_pointers.push_back(entry.data());
  environment_pointers.push_back(nullptr);
  char shell[] = "sh";
  char option[] = "-c";
  char* arguments[] = {shell, option, const_cast<char*>(command.c_str()), nullptr};
  const pid_t child = fork();
  if (child < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    throw std::runtime_error("Could not start the approved command.");
  }
  if (child > 0) {
    mtrxai::signals::set_active_child(child);
  }
  if (child == 0) {
    setpgid(0, 0);
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) _exit(126);
    if (getppid() == 1) _exit(126);
    close(output_pipe[0]);
    dup2(output_pipe[1], STDOUT_FILENO);
    dup2(output_pipe[1], STDERR_FILENO);
    close(output_pipe[1]);
    if (chdir(root.c_str()) != 0) _exit(126);
    execve("/bin/sh", arguments, environment_pointers.data());
    _exit(127);
  }
  setpgid(child, child);
  close(output_pipe[1]);
  const int flags = fcntl(output_pipe[0], F_GETFL, 0);
  fcntl(output_pipe[0], F_SETFL, flags | O_NONBLOCK);
  std::string output;
  constexpr std::size_t output_limit = 16 * 1024;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
  bool exited = false;
  bool eof = false;
  int status = 0;
  bool timed_out = false;
  bool truncated = false;
  char buffer[4096];
  while (!exited || !eof) {
    const auto now = std::chrono::steady_clock::now();
    if (!timed_out && now >= deadline && (!exited || !eof)) {
      timed_out = true;
      kill(-child, SIGTERM);
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      kill(-child, SIGKILL);
    }
    pollfd descriptor{output_pipe[0], POLLIN | POLLHUP, 0};
    poll(&descriptor, 1, 100);
    while (true) {
      const ssize_t count = read(output_pipe[0], buffer, sizeof(buffer));
      if (count > 0) {
        const std::size_t remaining =
            output_limit > output.size() ? output_limit - output.size() : 0;
        output.append(buffer, std::min<std::size_t>(count, remaining));
        if (static_cast<std::size_t>(count) > remaining) truncated = true;
        continue;
      }
      if (count == 0) eof = true;
      if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        eof = true;
      }
      break;
    }
    if (!exited) {
      const pid_t result = waitpid(child, &status, WNOHANG);
      exited = result == child;
      if (result < 0 && errno != EINTR) {
        close(output_pipe[0]);
        throw std::runtime_error("Could not collect command result.");
      }
    }
    if (truncated && !exited) kill(-child, SIGTERM);
  }
  mtrxai::signals::set_active_child(0);
  close(output_pipe[0]);
  if (timed_out) output += "\n[command timed out]\n";
  if (truncated) output += "\n[output truncated at 16 KiB]\n";
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                          : 128 + WTERMSIG(status);
  return "exit_code=" + std::to_string(exit_code) + "\n" + output;
}

std::string write_file(const std::filesystem::path& root,
                       const std::filesystem::path& session,
                       const std::filesystem::path& relative,
                       const std::string& content, int turn) {
  if (content.size() > 2 * 1024 * 1024) {
    throw std::runtime_error("File exceeds the 2 MiB write limit.");
  }
  if (sensitive_filename(relative.filename().string())) {
    throw std::runtime_error("Refusing to write a sensitive file path: " +
                             relative.string());
  }
  const auto target = checked_target(root, relative);
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create parent directory for " +
                             relative.string() + ": " + error.message());
  }
  const bool existed = std::filesystem::exists(target);
  if (existed) {
    const auto backup_dir = session / "backups";
    std::filesystem::create_directories(backup_dir, error);
    if (error) {
      throw std::runtime_error("Could not create session backup directory: " +
                               error.message());
    }
    const auto backup = backup_dir / (std::to_string(turn) + ".bak");
    std::filesystem::copy_file(target, backup,
                               std::filesystem::copy_options::none, error);
    if (error) {
      throw std::runtime_error("Could not back up " + relative.string() + ": " +
                               error.message());
    }
  }
  const auto permissions = existed
                               ? std::filesystem::status(target).permissions()
                               : std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read;
  write_atomic(target, content, permissions);
  return "Saved " + relative.generic_string() + " (" +
         std::to_string(content.size()) + " bytes)." +
         (existed ? " Original backed up to session/backups/" +
                        std::to_string(turn) + ".bak."
                  : "");
}

void save_manifest(const std::filesystem::path& session,
                  const std::string& stage) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "completed_stage",
                         json_object_new_string(stage.c_str()));
  json_object_object_add(root.get(), "updated_at",
                         json_object_new_string(timestamp().c_str()));
  write_atomic(session / "manifest.json",
               json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY));
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

// Two failed replies for one turn can arrive within the same millisecond, and
// a same-named archive would otherwise silently replace the earlier evidence.
std::filesystem::path unused_archive_path(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error) && !error) return path;
  for (int suffix = 1; suffix < 1000; ++suffix) {
    const auto candidate =
        path.parent_path() /
        (path.filename().string() + "-" + std::to_string(suffix));
    if (!std::filesystem::exists(candidate, error) && !error) return candidate;
  }
  throw std::runtime_error("Could not find a free archive name for " +
                           path.string());
}

// A resumed run may have to ask a model again. Keeping the previous raw log
// (instead of appending to it) is essential: read_raw_text would otherwise
// concatenate the abandoned partial answer with the new one.
void move_aside(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error) || error) return;
  const auto aside = unused_archive_path(
      path.parent_path() /
      (path.filename().string() + ".before-resume-" + timestamp()));
  std::filesystem::rename(path, aside, error);
  if (error) {
    throw std::runtime_error("Could not set aside " + path.string() + ": " +
                             error.message());
  }
}

// Keeps a reply that the model should not have sent. The transcript records
// only usable actions, so the archive is the only copy of the raw reply (the
// processing stream is also moved aside). It stays readable for debugging.
std::filesystem::path archive_rejected_reply(
    const std::filesystem::path& session, int turn,
    const std::string& rejection, const std::string& content,
    const std::string& thinking) {
  const auto directory = session / "coder" / "rejected";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    throw std::runtime_error("Could not create rejected reply directory: " +
                             error.message());
  }
  const std::string label =
      rejection.rfind("Unsupported coder action:", 0) == 0 ? "unsupported"
                                                           : "malformed";
  const auto path = unused_archive_path(
      directory / ("turn-" + std::to_string(turn) + "-" + label + "-" +
                   timestamp() + ".md"));
  std::ostringstream archive;
  archive << "# Rejected Coder reply\n\n"
          << "Turn: " << turn << "\n"
          << "Reason: " << rejection << "\n\n";
  if (!thinking.empty()) {
    archive << "## Thinking\n\n" << thinking << "\n\n";
  }
  archive << "## Reply\n\n" << content << "\n";
  write_atomic(path, archive.str());
  return path;
}

// Stages only move forward. A manifest from an older version may be missing,
// so an unknown stage ranks below "request" and the run starts over.
int stage_rank(const std::string& stage) {
  if (stage == "finished") return 6;
  if (stage == "turn-limit") return 5;
  if (stage.rfind("coder-turn-", 0) == 0) return 4;
  if (stage == "prompter") return 3;
  if (stage == "chief") return 2;
  if (stage == "request") return 1;
  return 0;
}

// Keeps Pipeline::active_session_ pointing at the live model only while a
// stage is generating. Clearing it from the destructor means both normal
// completion and exceptions (cancel, network failure) leave no dangling
// pointer behind for request_cancel() to touch.
class ActiveSessionGuard {
 public:
  ActiveSessionGuard(std::atomic<InferenceSession*>& slot,
                     InferenceSession& session)
      : slot_(slot) {
    slot_.store(&session);
  }
  ~ActiveSessionGuard() { slot_.store(nullptr); }

  ActiveSessionGuard(const ActiveSessionGuard&) = delete;
  ActiveSessionGuard& operator=(const ActiveSessionGuard&) = delete;

 private:
  std::atomic<InferenceSession*>& slot_;
};

}  // namespace

Pipeline::Pipeline(Config& config) : config_(config) {}

void Pipeline::request_cancel() {
  cancelled_.store(true, std::memory_order_relaxed);
  // The current model request also aborts mid-generation instead of finishing
  // its remaining seconds.
  if (auto* session = active_session_.load(std::memory_order_relaxed)) {
    session->request_cancel();
  }
}

void Pipeline::run(const std::string& request, const Status& status,
                   const Confirm& confirm,
                   const InferenceSession::TokenSink& tokens) {
  run_impl({}, request, status, confirm, tokens);
}

void Pipeline::resume(const std::filesystem::path& session,
                      const Status& status, const Confirm& confirm,
                      const InferenceSession::TokenSink& tokens) {
  run_impl(session, {}, status, confirm, tokens);
}

void Pipeline::run_impl(const std::filesystem::path& resume_session,
                        std::string request, const Status& status,
                        const Confirm& confirm,
                        const InferenceSession::TokenSink& tokens) {
  // Each run starts clean; without this, one Esc press would cancel every
  // later run for the rest of the session.
  cancelled_.store(false, std::memory_order_relaxed);
  auto throw_if_cancelled = [this](const std::string& point) {
    if (cancelled_.load(std::memory_order_relaxed)) {
      throw Cancelled("Cancelled at " + point + ".");
    }
  };
  auto wrap_tokens = [&tokens](RawRecorder& recorder, int turn) {
    return [&, turn](const std::string& chunk) {
      recorder.append(chunk, turn);
      if (tokens) tokens(chunk);
    };
  };
  const auto root = std::filesystem::canonical(std::filesystem::current_path());
  const bool resuming = !resume_session.empty();
  std::filesystem::path session;
  std::string completed_stage;
  std::error_code error;

  if (!resuming) {
    const auto sessions = sessions_root();
    std::filesystem::create_directories(sessions, error);
    if (error) {
      throw std::runtime_error("Could not create session directory: " +
                               error.message());
    }
    for (int attempt = 0; attempt < 1000; ++attempt) {
      session = sessions / timestamp();
      if (attempt != 0) session += "-" + std::to_string(attempt);
      error.clear();
      if (std::filesystem::create_directory(session, error)) break;
      if (error) {
        throw std::runtime_error("Could not create session directory: " +
                                 error.message());
      }
      session.clear();
    }
    if (session.empty()) {
      throw std::runtime_error("Could not allocate a unique session directory");
    }
    std::filesystem::permissions(session, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);
    if (error) {
      throw std::runtime_error("Could not protect session directory: " +
                               error.message());
    }
    write_atomic(session / "request.md", request);
    // Remember which project this session belongs to. Resuming from another
    // directory would otherwise apply the saved prompt to the wrong files.
    write_atomic(session / "project-root.txt", root.string() + "\n");
    save_manifest(session, "request");
    completed_stage = "request";
  } else {
    session = resume_session;
    if (!std::filesystem::is_directory(session, error) || error) {
      throw std::runtime_error("Session directory does not exist: " +
                               session.string());
    }
    request = read_session_request(session);
    std::string saved_root = read_text_file(session / "project-root.txt");
    while (!saved_root.empty() &&
           (saved_root.back() == '\n' || saved_root.back() == '\r')) {
      saved_root.pop_back();
    }
    if (!saved_root.empty() && saved_root != root.string()) {
      throw std::runtime_error("This session belongs to " + saved_root +
                               ". Resume it from that directory.");
    }
    completed_stage =
        parse_manifest_stage(read_text_file(session / "manifest.json"));
    if (completed_stage == "finished") {
      throw std::runtime_error("Session is already complete: " +
                               session.filename().string());
    }
    if (stage_rank(completed_stage) == 0) {
      // A truncated manifest must not strand the session. Keep the request and
      // rebuild every derived stage.
      save_manifest(session, "request");
      completed_stage = "request";
    }
    status("Resuming session " + session.filename().string() +
           ", continuing after saved stage " + completed_stage + ".");
  }
  throw_if_cancelled("the start");

  bool chief_saved = stage_rank(completed_stage) >= stage_rank("chief");
  std::string chief_content;
  if (chief_saved) {
    chief_content = read_text_file(session / "chief.md");
    chief_saved = !chief_content.empty();
  }
  if (chief_saved) {
    status("Chief of Staff: saved brief, skipping the model call.");
  } else {
    status("Chief of Staff: " + config_.chief.model);
    // The raw stream is on disk before the stage is considered done, so a
    // resume can rebuild it; chief.md is derived from that recording.
    const auto chief_raw_path = session / "chief" / "raw.jsonl";
    if (resuming) move_aside(chief_raw_path);
    {
      RawRecorder recorder(chief_raw_path);
      InferenceSession model(config_.chief, status);
      ActiveSessionGuard active(active_session_, model);
      model.generate({{"system", chief_instructions()}, {"user", request}},
                     wrap_tokens(recorder, 0));
      model.release();
    }
    const std::string chief_raw = read_raw_text(chief_raw_path, 0);
    const auto chief = extract_thinking(chief_raw);
    if (!chief.thinking.empty()) {
      write_atomic(session / "chief_thinking.md", chief.thinking);
    }
    write_atomic(session / "chief.md", chief.content);
    save_manifest(session, "chief");
    chief_content = chief.content;
  }

  // A saved prompt is only valid while the saved Chief brief is the one it was
  // built from. If the Chief had to run again, the Prompter must do so too.
  bool prompter_saved =
      chief_saved && stage_rank(completed_stage) >= stage_rank("prompter");
  std::string prompt_content;
  if (prompter_saved) {
    prompt_content = read_text_file(session / "prompt.md");
    prompter_saved = !prompt_content.empty();
  }
  if (prompter_saved) {
    status("Prompter: saved brief, skipping the model call.");
  } else {
    const std::string prompt_input = "Original user request:\n" + request +
                                     "\n\nChief of Staff brief:\n" +
                                     chief_content;
    status("Prompter: " + config_.prompter.model);
    const auto prompt_raw_path = session / "prompt" / "raw.jsonl";
    if (resuming) move_aside(prompt_raw_path);
    {
      RawRecorder recorder(prompt_raw_path);
      InferenceSession model(config_.prompter, status);
      ActiveSessionGuard active(active_session_, model);
      model.generate(
          {{"system", prompter_instructions()}, {"user", prompt_input}},
          wrap_tokens(recorder, 0));
      model.release();
    }
    const std::string prompt_raw = read_raw_text(prompt_raw_path, 0);
    const auto prompt = extract_thinking(prompt_raw);
    if (!prompt.thinking.empty()) {
      write_atomic(session / "prompt_thinking.md", prompt.thinking);
    }
    write_atomic(session / "prompt.md", prompt.content);
    save_manifest(session, "prompter");
    prompt_content = prompt.content;
  }

  // The Coder conversation is persisted message by message, including the
  // exact user wrapper around each tool result. A manifest stage alone cannot
  // tell whether an assistant reply was already produced, because the process
  // can die between the model reply and the tool result; the transcript can.
  const auto coder_raw_path = session / "coder" / "raw.jsonl";
  const auto coder_transcript_path = session / "coder" / "messages.jsonl";
  std::vector<CoderMessage> saved_messages =
      CoderTranscript::load(coder_transcript_path);

  // Reuse a transcript only for the plan it was generated from. If the Chief
  // or the Prompter had to run again (for example after a damaged manifest),
  // or the session predates transcripts, set the old Coder attempt aside and
  // start over. The old files remain for inspection.
  bool reuse_transcript =
      resuming && !saved_messages.empty() && chief_saved && prompter_saved;
  if (reuse_transcript &&
      (saved_messages.size() < 2 || saved_messages[0].role != "system" ||
       saved_messages[1].role != "user")) {
    reuse_transcript = false;
  }
  if (resuming && !reuse_transcript) {
    status("Coder: no saved conversation to continue; starting the Coder "
           "from the saved plan.");
    move_aside(session / "coder");
    move_aside(session / "coder.md");
    saved_messages.clear();
  }

  // A saved assistant message means the model reply was durable but its action
  // was not recorded as a tool result. That action is processed again, and any
  // write or command still goes through the normal approval prompt. A saved
  // finish just completes the session without loading a model at all.
  bool pending_assistant =
      reuse_transcript && saved_messages.back().role == "assistant";
  int assistant_count = static_cast<int>(std::count_if(
      saved_messages.begin(), saved_messages.end(),
      [](const CoderMessage& message) {
        return message.role == "assistant";
      }));
  std::string response_content;
  int response_turn = 0;
  if (pending_assistant) {
    response_content = saved_messages.back().content;
    // Older transcripts may not have a turn field; the last assistant reply
    // belongs to the most recent generated turn.
    response_turn = saved_messages.back().turn > 0
                        ? saved_messages.back().turn
                        : std::max(1, assistant_count);
  }

  // A transcript written before parse-before-persist could end with a reply
  // that was never usable. Resume must not ask the user to approve it. Archive
  // the reply, replace it with a correction, and rewrite the transcript before
  // any model call. The discarded reply counts as this turn's one retry, so a
  // fresh bad reply aborts immediately instead of allowing two more.
  int rejected_attempts = 0;
  std::string recovered_log;
  if (pending_assistant) {
    const std::string rejection = action_rejection(response_content);
    if (!rejection.empty()) {
      const auto archived = archive_rejected_reply(
          session, response_turn, rejection, response_content,
          saved_messages.back().thinking);
      rejected_attempts = 1;
      saved_messages.pop_back();
      --assistant_count;
      pending_assistant = false;
      const std::string correction = rejection_correction(rejection);
      CoderMessage correction_message;
      correction_message.role = "user";
      correction_message.content = correction;
      correction_message.turn = response_turn;
      saved_messages.push_back(correction_message);
      // Rewrite the whole file atomically: a crash during this repair must not
      // bring the discarded reply back on the next resume.
      CoderTranscript::save(coder_transcript_path, saved_messages);
      recovered_log = "## Recovered\n\nThe saved reply for turn " +
                      std::to_string(response_turn) + " was rejected: " +
                      rejection + "\n\nIt was saved at coder/rejected/" +
                      archived.filename().string() +
                      " and replaced with a correction.\n\n";
      status("Coder: saved reply for turn " + std::to_string(response_turn) +
             " was not a valid action; discarded it and will ask again.");
      response_content.clear();
      response_turn = 0;
    }
  }

  CoderTranscript transcript(coder_transcript_path);
  auto persist_message = [&transcript](const std::string& role,
                                       const std::string& content,
                                       const std::string& thinking, int turn) {
    CoderMessage message;
    message.role = role;
    message.content = content;
    message.thinking = thinking;
    message.turn = turn;
    transcript.append(message);
  };

  Messages coder_messages;
  std::string coder_log;
  if (reuse_transcript) {
    for (const auto& message : saved_messages) {
      coder_messages.emplace_back(message.role, message.content);
    }
    coder_log = read_text_file(session / "coder.md");
    if (coder_log.empty()) coder_log = "# Coder session\n\n";
    // Keep the human-readable log continuous, and make the resume visible.
    coder_log += "\n## Resumed\n\n";
    if (!recovered_log.empty()) coder_log += recovered_log;
    write_atomic(session / "coder.md", coder_log);
    status("Coder: loaded " + std::to_string(saved_messages.size()) +
           " saved messages; continuing at turn " +
           std::to_string(assistant_count + 1) + ".");
  } else {
    const std::string coding_input =
        "Original user request:\n" + request +
        "\n\nDetailed implementation prompt:\n" + prompt_content + "\n\n" +
        workspace_snapshot(root);
    coder_messages = {{"system", coder_instructions()},
                      {"user", coding_input}};
    persist_message("system", coder_instructions(), "", 0);
    persist_message("user", coding_input, "", 0);
    coder_log = "# Coder session\n\n";
    write_atomic(session / "coder.md", coder_log);
  }

  if (pending_assistant) {
    status("Coder: saved turn " + std::to_string(response_turn) +
           " has an action that was not applied yet; asking again.");
    // The assistant reply is durable but the tool result is not. There is no
    // unfinished generation to protect, so keep raw.jsonl; the next generated
    // turn gets a new turn number.
  } else if (reuse_transcript) {
    // The last saved message is a user message, so a generation may have been
    // interrupted before its assistant reply was recorded. RawRecorder appends
    // and read_raw_text selects by turn, so the abandoned partial answer would
    // otherwise be glued to the regenerated one.
    move_aside(coder_raw_path);
  }

  const int max_turns = std::clamp(config_.max_turns, 1, 500);

  auto execute_action = [&](const CoderAction& action,
                            int turn) -> std::string {
    if (action.name == "list_files") {
      const auto relative =
          action.path.empty() ? std::filesystem::path(".")
                              : std::filesystem::path(action.path);
      return "Project files:\n" + list_files(root, relative);
    }
    if (action.name == "read_file") {
      if (action.path.empty()) {
        throw std::runtime_error("read_file requires a relative path.");
      }
      return "Contents of " + action.path + ":\n" +
             read_file(root, action.path, action.offset);
    }
    if (action.name == "write_file") {
      if (action.path.empty()) {
        throw std::runtime_error("write_file requires a relative path.");
      }
      const std::filesystem::path relative(action.path);
      if (action.content.size() > 2 * 1024 * 1024) {
        throw std::runtime_error("File exceeds the 2 MiB write limit.");
      }
      if (sensitive_filename(relative.filename().string())) {
        throw std::runtime_error("Refusing to write a sensitive file path: " +
                                 action.path);
      }
      const auto target = checked_target(root, relative);
      if (std::filesystem::exists(target) &&
          !std::filesystem::is_regular_file(target)) {
        throw std::runtime_error("Refusing to replace a non-file: " +
                                 action.path);
      }
      std::ostringstream review;
      review << "Coder requests this file write:\nPath: " << action.path
             << "\nSize: " << action.content.size() << " bytes\n\n"
             << action.content
             << "\n\nApprove this exact file content?";
      if (confirm(review.str())) {
        return write_file(root, session, action.path, action.content, turn);
      }
      return "User declined this file write. The file was not changed.";
    }
    if (action.name == "run_command") {
      if (action.command.empty()) {
        throw std::runtime_error("run_command requires a command.");
      }
      const int timeout = std::clamp(action.timeout_seconds, 1, 300);
      std::ostringstream review;
      review << "Coder requests shell execution:\nCommand: " << action.command
             << "\nTimeout: " << timeout << " seconds\n"
             << "Working directory: project root\n\n"
             << "Approve this exact command?";
      if (confirm(review.str())) {
        return run_command(root, action.command, timeout);
      }
      return "User declined this command. It was not executed.";
    }
    throw std::runtime_error("Unsupported coder action: " + action.name);
  };

  // The model is loaded when the first reply must be generated, not when a
  // saved turn still needs an action. In particular, a pending finish resumes
  // without loading a model at all.
  std::unique_ptr<InferenceSession> coder_model;
  std::unique_ptr<ActiveSessionGuard> active;
  auto ensure_model = [&]() -> InferenceSession& {
    if (!coder_model) {
      status("Coder: " + (config_.coder.model.empty() ? config_.coder.gguf_path
                                                       : config_.coder.model));
      coder_model = std::make_unique<InferenceSession>(config_.coder, status);
      active =
          std::make_unique<ActiveSessionGuard>(active_session_, *coder_model);
    }
    return *coder_model;
  };
  std::unique_ptr<RawRecorder> coder_recorder;
  auto ensure_recorder = [&]() -> RawRecorder& {
    if (!coder_recorder) {
      coder_recorder = std::make_unique<RawRecorder>(coder_raw_path);
    }
    return *coder_recorder;
  };

  int next_turn = assistant_count + 1;
  while (true) {
    throw_if_cancelled("coder turn " +
                       std::to_string(pending_assistant ? response_turn
                                                        : next_turn));

    if (!pending_assistant) {
      if (next_turn > max_turns) {
        save_manifest(session, "turn-limit");
        throw std::runtime_error(
            "Coder reached the " + std::to_string(max_turns) +
            "-turn limit. Its work log is saved at " +
            (session / "coder.md").string());
      }
      // Keep the system prompt and the initial request, then the most recent
      // six messages. Both a fresh run and a resumed one are trimmed here, so
      // small context windows get the same window either way.
      while (coder_messages.size() > 8) {
        coder_messages.erase(coder_messages.begin() + 2,
                             coder_messages.begin() + 4);
      }
      status("Coder turn " + std::to_string(next_turn) + " of " +
             std::to_string(max_turns));
      RawRecorder& recorder = ensure_recorder();
      recorder.begin_group(next_turn);
      InferenceSession& model = ensure_model();
      model.generate(coder_messages, wrap_tokens(recorder, next_turn));

      const std::string response_raw =
          read_raw_text(coder_raw_path, next_turn);
      const auto response = extract_thinking(response_raw);
      response_content = response.content;
      response_turn = next_turn;

      // Validate before saving. The transcript is what resume trusts, so a
      // reply that cannot be acted on must never enter it; the archive keeps
      // the original text (and any reasoning) for inspection.
      const std::string rejection = action_rejection(response_content);
      if (!rejection.empty()) {
        const auto archived = archive_rejected_reply(
            session, response_turn, rejection, response.content,
            response.thinking);
        ++rejected_attempts;
        // The raw log holds this reply under the current turn. Close it before
        // moving it aside so the retry starts a new stream; read_raw_text would
        // otherwise concatenate both replies.
        coder_recorder.reset();
        move_aside(coder_raw_path);
        if (rejected_attempts >= 2) {
          throw std::runtime_error(
              "Coder produced two replies that were not valid actions for "
              "turn " + std::to_string(response_turn) +
              ". The rejected replies are saved in " +
              (session / "coder" / "rejected").string() + ".");
        }
        const std::string correction = rejection_correction(rejection);
        persist_message("user", correction, "", response_turn);
        coder_messages.emplace_back("user", correction);
        coder_log += "## Turn " + std::to_string(response_turn) +
                     " (rejected reply)\n\n";
        coder_log += "### Rejected Coder reply\n\n" + rejection +
                     "\n\nThe reply was saved at coder/rejected/" +
                     archived.filename().string() + ".\n\n";
        coder_log += "### Correction\n\n" + correction + "\n\n";
        write_atomic(session / "coder.md", coder_log);
        status("Coder: turn " + std::to_string(response_turn) +
               " reply rejected (" + rejection + "); asking once more.");
        continue;
      }

      // The reply is a usable action. This append is the commit point. If the
      // process dies after it, resume knows the reply exists and will not ask
      // the model for it again.
      persist_message("assistant", response.content, response.thinking,
                      response_turn);
      coder_messages.emplace_back("assistant", response.content);
      coder_log += "## Turn " + std::to_string(response_turn) + "\n\n";
      if (!response.thinking.empty()) {
        coder_log += "### Thinking\n\n" + response.thinking + "\n\n";
        write_atomic(session / ("coder_turn_" +
                                std::to_string(response_turn) + "_thinking.md"),
                     response.thinking);
      }
      coder_log += "### Coder response\n\n" + response.content + "\n\n";
      write_atomic(session / "coder.md", coder_log);
      save_manifest(session, "coder-turn-" +
                                 std::to_string(response_turn));
      pending_assistant = true;
      ++next_turn;
      rejected_attempts = 0;
    }

    // A fresh reply and one loaded from disk take exactly the same path from
    // here: parse the stored action, ask before side effects, record the
    // result. A resumed approval is a new approval, never a replay.
    std::string tool_result;
    try {
      const CoderAction action = parse_action(response_content);
      if (action.name == "finish") {
        tool_result =
            action.summary.empty() ? "Coder finished." : action.summary;
        coder_log += "### Finished\n\n" + tool_result + "\n";
        write_atomic(session / "coder.md", coder_log);
        save_manifest(session, "finished");
        if (coder_model) coder_model->release();
        status("Finished: " + tool_result + "\nSession artifacts: " +
               session.string());
        return;
      }
      tool_result = execute_action(action, response_turn);
    } catch (const std::exception& error) {
      tool_result = std::string("Tool error: ") + error.what();
    }

    // coder.md keeps the full result for a human; the model context gets a
    // bounded copy, and the transcript stores exactly what the model saw.
    coder_log += "### Tool result\n\n" + tool_result + "\n\n";
    std::string context_result = tool_result;
    constexpr std::size_t context_result_limit = 6000;
    if (context_result.size() > context_result_limit) {
      context_result.resize(context_result_limit);
      context_result +=
          "\n[tool output truncated for model context; full output is in session log]";
    }
    const std::string result_message =
        "Tool result:\n" + context_result +
        "\nContinue the task. Use another action or finish.";
    persist_message("user", result_message, "", response_turn);
    coder_messages.emplace_back("user", result_message);
    write_atomic(session / "coder.md", coder_log);
    save_manifest(session, "coder-turn-" + std::to_string(response_turn));
    pending_assistant = false;
  }
}

}  // namespace mtrxai
