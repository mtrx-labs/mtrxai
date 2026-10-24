#include "mtrxai/pipeline.hpp"

#include "mtrxai/files.hpp"
#include "mtrxai/provider.hpp"

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

bool sensitive_filename(const std::string& name) {
  // Extract base filename in case a relative path was passed
  const std::string filename = std::filesystem::path(name).filename().string();
  std::string lower = filename;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });

  // Exact sensitive names
  if (lower == ".env" || lower == ".netrc" || lower == ".npmrc" ||
      lower == ".pypirc" || lower == "credentials.json") {
    return true;
  }

  // Sensitive prefix patterns (.env.*, id_rsa*, id_ed25519*)
  if (lower.rfind(".env.", 0) == 0 || lower.rfind("id_rsa", 0) == 0 ||
      lower.rfind("id_ed25519", 0) == 0) {
    return true;
  }

  // Sensitive suffix patterns (*.pem, *.key, *.p12, *.pfx)
  for (const auto* suffix : {".pem", ".key", ".p12", ".pfx"}) {
    const std::size_t suffix_len = std::strlen(suffix);
    if (lower.size() >= suffix_len &&
        lower.compare(lower.size() - suffix_len, suffix_len, suffix) == 0) {
      return true;
    }
  }

  return false;
}

namespace {

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};

using Json = std::unique_ptr<json_object, JsonDeleter>;

std::filesystem::path state_root() {
  const char* xdg = std::getenv("XDG_STATE_HOME");
  if (xdg != nullptr && *xdg != '\0') {
    return std::filesystem::path(xdg) / "mtrxai" / "sessions";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; cannot create session artifacts");
  }
  return std::filesystem::path(home) / ".local" / "state" / "mtrxai" / "sessions";
}

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

std::string extract_json(const std::string& output) {
  const std::size_t fence = output.find("```");
  std::string candidate = output;
  if (fence != std::string::npos) {
    const std::size_t first_line = output.find('\n', fence);
    const std::size_t last_fence = output.find("```", fence + 3);
    if (first_line != std::string::npos && last_fence > first_line) {
      candidate = output.substr(first_line + 1, last_fence - first_line - 1);
    }
  }
  const auto first = candidate.find_first_not_of(" \t\r\n");
  const auto last = candidate.find_last_not_of(" \t\r\n");
  if (first == std::string::npos) return candidate;
  candidate = candidate.substr(first, last - first + 1);
  if (candidate.rfind("json\n", 0) == 0) candidate.erase(0, 5);
  if (!candidate.empty() && candidate.front() == '{') return candidate;

  const std::size_t object_start = candidate.find('{');
  const std::size_t object_end = candidate.rfind('}');
  if (object_start != std::string::npos && object_end > object_start) {
    return candidate.substr(object_start, object_end - object_start + 1);
  }
  return candidate;
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

std::filesystem::path checked_target(const std::filesystem::path& root,
                                     const std::filesystem::path& relative) {
  if (relative.empty() || relative.is_absolute()) {
    throw std::runtime_error("Path must be relative to the project.");
  }
  const auto target = root / relative;
  for (const auto& component : relative) {
    if (component == "..") {
      throw std::runtime_error("Refusing a path that traverses outside its directory.");
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
    throw std::runtime_error("Refusing to overwrite a symlink: " + relative.string());
  }
  error.clear();
  const auto resolved = std::filesystem::weakly_canonical(target, error);
  if (error) {
    throw std::runtime_error("Could not resolve proposed path " + relative.string() +
                             ": " + error.message());
  }
  const auto within_root = resolved.lexically_relative(root);
  if (within_root.empty() || *within_root.begin() == "..") {
    throw std::runtime_error("Refusing to write outside the project: " +
                             relative.string());
  }
  if (std::filesystem::exists(target) &&
      !std::filesystem::is_regular_file(target)) {
    throw std::runtime_error("Refusing to replace a non-file: " + relative.string());
  }
  return target;
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

}  // namespace

Pipeline::Pipeline(Config& config) : config_(config) {}

void Pipeline::run(const std::string& request, const Status& status,
                   const Confirm& confirm) {
  const auto root = std::filesystem::canonical(std::filesystem::current_path());
  const auto sessions = state_root();
  std::error_code error;
  std::filesystem::create_directories(sessions, error);
  if (error) {
    throw std::runtime_error("Could not create session directory: " +
                             error.message());
  }
  std::filesystem::path session;
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
  save_manifest(session, "request");

  status("Chief of Staff: " + config_.chief.model);
  std::string chief;
  {
    InferenceSession model(config_.chief, status);
    chief = model.generate({{"system", chief_instructions()}, {"user", request}});
    model.release();
  }
  write_atomic(session / "chief.md", chief);
  save_manifest(session, "chief");

  const std::string prompt_input = "Original user request:\n" + request +
                                   "\n\nChief of Staff brief:\n" + chief;
  status("Prompter: " + config_.prompter.model);
  std::string prompt;
  {
    InferenceSession model(config_.prompter, status);
    prompt = model.generate(
        {{"system", prompter_instructions()}, {"user", prompt_input}});
    model.release();
  }
  write_atomic(session / "prompt.md", prompt);
  save_manifest(session, "prompter");

  const std::string coding_input =
      "Original user request:\n" + request + "\n\nDetailed implementation prompt:\n" +
      prompt + "\n\n" + workspace_snapshot(root);
  status("Coder: " + (config_.coder.model.empty() ? config_.coder.gguf_path
                                                   : config_.coder.model));
  Messages coder_messages = {{"system", coder_instructions()},
                             {"user", coding_input}};
  InferenceSession coder_model(config_.coder, status);
  std::string coder_log = "# Coder session\n\n";
  write_atomic(session / "coder.md", coder_log);
  for (int turn = 1; turn <= 40; ++turn) {
    while (coder_messages.size() > 8) {
      coder_messages.erase(coder_messages.begin() + 2,
                           coder_messages.begin() + 4);
    }
    status("Coder turn " + std::to_string(turn) + " of 40");
    const std::string response = coder_model.generate(coder_messages);
    coder_messages.emplace_back("assistant", response);
    coder_log += "## Turn " + std::to_string(turn) + "\n\n";
    coder_log += "### Coder response\n\n" + response + "\n\n";
    write_atomic(session / "coder.md", coder_log);
    save_manifest(session, "coder-turn-" + std::to_string(turn));

    std::string tool_result;
    try {
      const CoderAction action = parse_action(response);
      if (action.name == "finish") {
        tool_result = action.summary.empty() ? "Coder finished." : action.summary;
        coder_log += "### Finished\n\n" + tool_result + "\n";
        write_atomic(session / "coder.md", coder_log);
        save_manifest(session, "finished");
        coder_model.release();
        status("Finished: " + tool_result + "\nSession artifacts: " +
               session.string());
        return;
      }
      if (action.name == "list_files") {
        const auto relative =
            action.path.empty() ? std::filesystem::path(".")
                                : std::filesystem::path(action.path);
        tool_result = "Project files:\n" + list_files(root, relative);
      } else if (action.name == "read_file") {
        if (action.path.empty()) {
          throw std::runtime_error("read_file requires a relative path.");
        }
        tool_result = "Contents of " + action.path + ":\n" +
                      read_file(root, action.path, action.offset);
      } else if (action.name == "write_file") {
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
          tool_result = write_file(root, session, action.path, action.content, turn);
        } else {
          tool_result = "User declined this file write. The file was not changed.";
        }
      } else if (action.name == "run_command") {
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
          tool_result = run_command(root, action.command, timeout);
        } else {
          tool_result = "User declined this command. It was not executed.";
        }
      } else {
        throw std::runtime_error("Unsupported coder action: " + action.name);
      }
    } catch (const std::exception& error) {
      tool_result = std::string("Tool error: ") + error.what();
    }

    constexpr std::size_t context_result_limit = 6000;
    if (tool_result.size() > context_result_limit) {
      tool_result.resize(context_result_limit);
      tool_result += "\n[tool output truncated for model context; full output is in session log]";
    }
    coder_log += "### Tool result\n\n" + tool_result + "\n\n";
    write_atomic(session / "coder.md", coder_log);
    save_manifest(session, "coder-turn-" + std::to_string(turn));
    coder_messages.emplace_back(
        "user", "Tool result:\n" + tool_result +
                    "\nContinue the task. Use another action or finish.");
  }
  save_manifest(session, "turn-limit");
  throw std::runtime_error("Coder reached the 40-turn limit. Its work log is saved at " +
                           (session / "coder.md").string());
}

}  // namespace mtrxai
