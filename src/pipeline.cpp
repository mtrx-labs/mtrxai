#include "mtrxai/pipeline.hpp"

#include "mtrxai/files.hpp"
#include "mtrxai/provider.hpp"

#include <json-c/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace mtrxai {
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
  return "You are the coding model working in the project directory where "
         "mtrxAI was launched. Use the supplied bounded workspace snapshot to "
         "understand existing files. It may be incomplete; do not replace a "
         "file whose contents are not included unless the request clearly calls "
         "for creating that file. Implement the original request and detailed brief "
         "by returning only a JSON object with this shape: "
         "{\"summary\":\"short description\",\"files\":[{\"path\":\"relative/path\","
         "\"content\":\"complete file contents\"}]}. Include every new or changed "
         "file as a complete replacement. Paths must be relative to the project "
         "directory. Do not include commands, markdown fences, or explanations "
         "outside the JSON. Do not claim tests were run. The application will "
         "show the proposed file changes and ask the user before writing them.";
}

bool sensitive_filename(const std::string& name) {
  std::string lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  for (const auto* term : {"secret", "credential", "token", "password", "apikey",
                           "private", "id_rsa", "id_ed25519"}) {
    if (lower.find(term) != std::string::npos) return true;
  }
  for (const auto* suffix : {".env", ".pem", ".key", ".p12", ".pfx", ".crt"}) {
    if (lower.size() >= std::strlen(suffix) &&
        lower.compare(lower.size() - std::strlen(suffix), std::strlen(suffix),
                      suffix) == 0) {
      return true;
    }
  }
  return false;
}

std::string workspace_snapshot(const std::filesystem::path& root) {
  static const std::set<std::string> skipped_directories = {
      ".git", ".mtrxai", "build", "cmake-build-debug", "cmake-build-release",
      "copilot-worktrees", "dist", "node_modules", "target", "vendor"};
  static const std::set<std::string> source_extensions = {
      ".c",   ".cc",  ".cpp", ".cs",   ".css", ".go",   ".h",   ".hh",
      ".hpp", ".html", ".java", ".js", ".jsx", ".md",   ".py",  ".rs",
      ".sh",  ".sql", ".ts",  ".tsx",  ".txt"};

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
  const std::size_t listed = std::min<std::size_t>(paths.size(), 200);
  for (std::size_t index = 0; index < listed; ++index) {
    result << "  " << paths[index].generic_string() << '\n';
  }
  if (paths.size() > listed) result << "  ... file list truncated\n";

  result << "\nSelected source and documentation contents "
            "(maximum 8 KiB total; may be incomplete):\n";
  std::size_t included = 0;
  for (const auto& relative : paths) {
    const std::string filename = relative.filename().string();
    const std::string extension = relative.extension().string();
    if (filename != "CMakeLists.txt" && filename != "Makefile" &&
        source_extensions.count(extension) == 0) {
      continue;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(root / relative, error);
    if (error || size > 8192 || included >= 8192) continue;
    std::ifstream file(root / relative, std::ios::binary);
    if (!file) continue;
    std::string contents((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    if (contents.find('\0') != std::string::npos) continue;
    const auto remaining = 8192 - included;
    if (contents.size() > remaining) contents.resize(remaining);
    result << "\n--- " << relative.generic_string() << " ---\n" << contents;
    if (contents.empty() || contents.back() != '\n') result << '\n';
    included += contents.size();
  }
  return result.str();
}

struct ProposedFile {
  std::filesystem::path relative_path;
  std::string content;
};

std::string extract_json(const std::string& output) {
  const std::size_t fence = output.find("```");
  if (fence == std::string::npos) return output;
  const std::size_t first_line = output.find('\n', fence);
  const std::size_t last_fence = output.rfind("```");
  if (first_line == std::string::npos || last_fence <= first_line) return output;
  return output.substr(first_line + 1, last_fence - first_line - 1);
}

std::vector<ProposedFile> parse_files(const std::string& output,
                                     std::string& summary) {
  const std::string json = extract_json(output);
  Json root(json_tokener_parse(json.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("Coder output was not a JSON file proposal. See coder.md.");
  }
  json_object* summary_object = nullptr;
  if (json_object_object_get_ex(root.get(), "summary", &summary_object) &&
      json_object_is_type(summary_object, json_type_string)) {
    summary = json_object_get_string(summary_object);
  }
  json_object* files = nullptr;
  if (!json_object_object_get_ex(root.get(), "files", &files) ||
      !json_object_is_type(files, json_type_array)) {
    throw std::runtime_error("Coder JSON must contain a files array. See coder.md.");
  }
  if (json_object_array_length(files) > 100) {
    throw std::runtime_error("Coder proposed more than 100 files; refusing to apply.");
  }

  std::vector<ProposedFile> result;
  std::size_t total_size = 0;
  for (std::size_t index = 0; index < json_object_array_length(files); ++index) {
    json_object* file = json_object_array_get_idx(files, index);
    json_object* path = nullptr;
    json_object* content = nullptr;
    if (!json_object_is_type(file, json_type_object) ||
        !json_object_object_get_ex(file, "path", &path) ||
        !json_object_is_type(path, json_type_string) ||
        !json_object_object_get_ex(file, "content", &content) ||
        !json_object_is_type(content, json_type_string)) {
      throw std::runtime_error("Each coder file requires string path and content.");
    }
    const std::filesystem::path relative(json_object_get_string(path));
    if (relative.empty() || relative.is_absolute()) {
      throw std::runtime_error("Coder proposed an invalid relative file path.");
    }
    for (const auto& component : relative) {
      if (component == ".." || component == "." || component.empty()) {
        throw std::runtime_error("Coder proposed a path outside the project.");
      }
    }
    const std::string contents = json_object_get_string(content);
    total_size += contents.size();
    if (total_size > 2 * 1024 * 1024) {
      throw std::runtime_error("Coder proposed more than 2 MiB of files.");
    }
    for (const auto& prior : result) {
      if (prior.relative_path == relative) {
        throw std::runtime_error("Coder proposed a duplicate file path.");
      }
    }
    result.push_back({relative, contents});
  }
  return result;
}

std::filesystem::path checked_target(const std::filesystem::path& root,
                                     const std::filesystem::path& relative) {
  const auto target = root / relative;
  for (const auto& component : relative) {
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

void apply_files(const std::filesystem::path& root,
                 const std::filesystem::path& session,
                 const std::vector<ProposedFile>& files) {
  std::filesystem::create_directories(session / "backups");
  struct Write {
    std::filesystem::path target;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool existed;
  };
  std::vector<Write> writes;
  writes.reserve(files.size());

  for (std::size_t index = 0; index < files.size(); ++index) {
    const auto target = checked_target(root, files[index].relative_path);
    std::filesystem::create_directories(target.parent_path());
    const bool existed = std::filesystem::exists(target);
    const auto backup = session / "backups" / (std::to_string(index) + ".bak");
    if (existed) {
      std::filesystem::copy_file(target, backup,
                                 std::filesystem::copy_options::overwrite_existing);
    }
    const auto temporary =
        target.string() + ".mtrxai-" + std::to_string(index) + ".tmp";
    const auto permissions = existed
                                 ? std::filesystem::status(target).permissions()
                                 : std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write |
                                       std::filesystem::perms::group_read |
                                       std::filesystem::perms::others_read;
    write_atomic(temporary, files[index].content, permissions);
    writes.push_back({target, temporary, backup, existed});
  }

  std::size_t applied = 0;
  try {
    for (const auto& write : writes) {
      std::error_code error;
      std::filesystem::rename(write.temporary, write.target, error);
      if (error) {
        throw std::runtime_error("Could not apply " + write.target.string() +
                                 ": " + error.message());
      }
      ++applied;
    }
  } catch (...) {
    while (applied > 0) {
      const auto& write = writes[--applied];
      std::error_code error;
      if (write.existed) {
        std::filesystem::copy_file(
            write.backup, write.target,
            std::filesystem::copy_options::overwrite_existing, error);
      } else {
        std::filesystem::remove(write.target, error);
      }
    }
    for (const auto& write : writes) std::filesystem::remove(write.temporary);
    throw;
  }
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
  const std::string chief = generate(
      config_.chief,
      {{"system", chief_instructions()}, {"user", request}}, status);
  write_atomic(session / "chief.md", chief);
  save_manifest(session, "chief");

  const std::string prompt_input = "Original user request:\n" + request +
                                   "\n\nChief of Staff brief:\n" + chief;
  status("Prompter: " + config_.prompter.model);
  const std::string prompt = generate(
      config_.prompter,
      {{"system", prompter_instructions()}, {"user", prompt_input}}, status);
  write_atomic(session / "prompt.md", prompt);
  save_manifest(session, "prompter");

  const std::string coding_input =
      "Original user request:\n" + request + "\n\nDetailed implementation prompt:\n" +
      prompt + "\n\n" + workspace_snapshot(root);
  status("Coder: " + (config_.coder.model.empty() ? config_.coder.gguf_path
                                                   : config_.coder.model));
  const std::string coder = generate(
      config_.coder,
      {{"system", coder_instructions()}, {"user", coding_input}}, status);
  write_atomic(session / "coder.md", coder);
  save_manifest(session, "coder");

  std::string summary;
  const auto files = parse_files(coder, summary);
  if (files.empty()) {
    status("No files proposed. Full response saved in " +
           (session / "coder.md").string());
    return;
  }

  const auto workspace = root;
  std::ostringstream proposal;
  proposal << (summary.empty() ? "Coder proposed these files:" : summary) << "\n";
  for (const auto& file : files) proposal << "  " << file.relative_path.string() << '\n';
  proposal << "Session artifacts: " << session.string()
           << "\nApply these files to the project? [y/N]";
  status(proposal.str());
  if (confirm(proposal.str())) {
    apply_files(workspace, session, files);
    write_atomic(session / "applied.txt", proposal.str());
    save_manifest(session, "applied");
    status("Applied " + std::to_string(files.size()) + " files. Backups: " +
           (session / "backups").string());
  } else {
    save_manifest(session, "declined");
    status("Files not applied. Proposal saved in " +
           (session / "coder.md").string());
  }
}

}  // namespace mtrxai
