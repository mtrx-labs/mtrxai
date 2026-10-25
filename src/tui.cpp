#include "mtrxai/tui.hpp"

#include "mtrxai/pipeline.hpp"
#include "mtrxai/session.hpp"
#include "mtrxai/cancel.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/utf8.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mtrxai {
namespace {

constexpr int kSidebarWidth = 25;

// FTXUI owns the terminal and redraws the whole screen on every frame. Keeping
// text free of control bytes still matters: model output and file contents must
// never be able to emit terminal commands.
std::string safe_terminal_text(const std::string& text) {
  std::string safe;
  safe.reserve(text.size());
  for (unsigned char character : text) {
    if (character == '\n' || character == '\t' || character >= 0x20) {
      if (character != 0x7f) safe.push_back(static_cast<char>(character));
    } else {
      safe.push_back('?');
    }
  }
  return safe;
}

// Break text into terminal-width lines at UTF-8 boundaries. The approval and
// transcript views both need explicit line control: approvals page by line,
// and the transcript always shows the newest lines.
std::vector<std::string> wrap_lines(const std::string& text, int width) {
  std::vector<std::string> result;
  std::istringstream input(safe_terminal_text(text));
  std::string line;
  width = std::max(1, width);
  while (std::getline(input, line)) {
    if (line.empty()) {
      result.emplace_back();
      continue;
    }
    while (static_cast<int>(line.size()) > width) {
      int end = width;
      while (end > 0 &&
             (static_cast<unsigned char>(line[static_cast<std::size_t>(end)]) &
              0xc0) == 0x80) {
        --end;
      }
      if (end == 0) end = width;
      result.push_back(line.substr(0, static_cast<std::size_t>(end)));
      line.erase(0, static_cast<std::size_t>(end));
    }
    result.push_back(std::move(line));
  }
  if (result.empty()) result.emplace_back();
  return result;
}

struct CommandResult {
  std::string text;
  // A non-empty role means the TUI should ask for a hidden API key before
  // doing anything else. The command handler cannot block on input itself.
  std::string secret_role;
};

std::string roles_report(const Config& config) {
  std::ostringstream text;
  for (const auto* name : {"chief", "prompter", "coder"}) {
    const std::string role(name);
    const auto& slot = find_slot(config, role);
    const std::string model =
        slot.model.empty()
            ? (slot.gguf_path.empty() ? "(GGUF path not configured)"
                                      : slot.gguf_path)
            : slot.model;
    text << slot.name << ": " << slot.provider << " / " << model
         << " (ctx: " << slot.context_size
         << ", max_tokens: " << slot.max_tokens << ")\n";
  }
  text << "Pipeline: max turns: " << config.max_turns
       << ", GGUF load timeout: " << config.gguf_load_timeout_seconds << "s\n";
  return text.str();
}

std::string sessions_report() {
  const auto sessions = list_sessions();
  if (sessions.empty()) return "No saved sessions yet.";
  std::ostringstream text;
  text << "Saved sessions (newest first):\n\n";
  const std::size_t shown = std::min<std::size_t>(sessions.size(), 5);
  for (std::size_t i = 0; i < shown; ++i) {
    const auto& session = sessions[i];
    text << session.id << "  "
         << (session.stage.empty() ? "unknown stage" : session.stage);
    if (!session.updated_at.empty()) text << "  " << session.updated_at;
    text << "\n";
    try {
      const std::string request = read_session_request(session.path);
      const std::size_t newline = request.find('\n');
      std::string summary = request.substr(0, newline);
      if (summary.size() > 52) summary = summary.substr(0, 49) + "...";
      text << "  " << summary << "\n";
    } catch (const std::exception&) {
      text << "  (request could not be read)\n";
    }
  }
  if (sessions.size() > shown) {
    text << "\n" << (sessions.size() - shown) << " older session(s) not shown.";
  }
  text << "\n\nResume with: mtrxai --resume <id>";
  return text.str();
}

std::string help_report() {
  return "Commands:\n"
         "  /roles                         show active model assignments & limits\n"
         "  /sessions                      list saved sessions and resume ids\n"
         "  /model ROLE PROVIDER MODEL      hot-swap a role (ollama, openai, anthropic, gguf)\n"
         "  /endpoint ROLE URL              set an API base URL\n"
         "  /keyenv ROLE ENV_NAME           select an API-key environment variable\n"
         "  /apikey ROLE                    enter or clear a saved API key (hidden input)\n"
         "  /gguf ROLE PATH                 set a local GGUF file path\n"
         "  /context ROLE SIZE              set context window size (512-32768)\n"
         "  /maxturns NUMBER                set max Coder turns (1-500)\n"
         "  /gguftimeout SECONDS            set GGUF server load timeout (1-3600)\n"
         "Use Up/Down or PgUp/PgDn to review every proposed file or shell command.\n"
         "  /help                           show this help\n"
         "  /quit                           exit";
}

std::string expand_home(const std::string& value) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; cannot expand model path");
  }
  if (value == "~") return std::filesystem::path(home).string();
  if (value.rfind("~/", 0) == 0) {
    return (std::filesystem::path(home) / value.substr(2)).string();
  }
  return value;
}

// Parses and applies a slash command. Commands that only need to display text
// return it instead of drawing, because drawing belongs to the UI thread.
CommandResult execute_command(Config& config,
                              const std::filesystem::path& config_file,
                              const std::string& input) {
  std::istringstream command(input);
  std::string name;
  command >> name;
  if (name == "/roles") return {roles_report(config), {}};
  if (name == "/sessions") return {sessions_report(), {}};
  if (name == "/help") return {help_report(), {}};
  if (name == "/maxturns") {
    std::string value;
    command >> value;
    int turns = 0;
    try {
      turns = std::stoi(value);
    } catch (...) {
      return {"Usage: /maxturns NUMBER (1-500)", {}};
    }
    if (turns < 1 || turns > 500) {
      return {"max_turns must be between 1 and 500.", {}};
    }
    config.max_turns = turns;
    save_config(config, config_file);
    return {"Saved max turns: " + std::to_string(turns) + ".", {}};
  }
  if (name == "/gguftimeout") {
    std::string value;
    command >> value;
    int timeout = 0;
    try {
      timeout = std::stoi(value);
    } catch (...) {
      return {"Usage: /gguftimeout SECONDS (1-3600)", {}};
    }
    if (timeout < 1 || timeout > 3600) {
      return {"gguf_load_timeout_seconds must be between 1 and 3600.", {}};
    }
    config.gguf_load_timeout_seconds = timeout;
    config.chief.gguf_load_timeout_seconds = timeout;
    config.prompter.gguf_load_timeout_seconds = timeout;
    config.coder.gguf_load_timeout_seconds = timeout;
    save_config(config, config_file);
    return {"Saved GGUF load timeout: " + std::to_string(timeout) +
                " seconds.",
            {}};
  }
  std::string role;
  command >> role;
  if (name == "/context") {
    std::string value;
    command >> value;
    if (role.empty() || value.empty()) {
      return {"Usage: /context ROLE SIZE (512-32768)", {}};
    }
    int size = 0;
    try {
      size = std::stoi(value);
    } catch (...) {
      return {"Invalid context size.", {}};
    }
    if (size < 512 || size > 32768) {
      return {"context_size must be between 512 and 32768.", {}};
    }
    auto& slot = find_slot(config, role);
    slot.context_size = size;
    save_config(config, config_file);
    return {"Saved context size " + std::to_string(size) + " for " + role + ".",
            {}};
  }
  if (name == "/apikey") {
    if (role.empty()) return {"Usage: /apikey ROLE", {}};
    auto& slot = find_slot(config, role);
    if (slot.provider != "openai" && slot.provider != "anthropic") {
      return {"Select an OpenAI or Anthropic provider before setting an API key.",
              {}};
    }
    return {"", role};
  }
  if (name == "/endpoint" || name == "/keyenv" || name == "/gguf") {
    std::string value;
    command >> value;
    if (role.empty() || value.empty()) {
      return {"Usage: " + name + " ROLE VALUE", {}};
    }
    auto& slot = find_slot(config, role);
    if (name == "/endpoint") slot.base_url = value;
    if (name == "/keyenv") slot.api_key_env = value;
    if (name == "/gguf") {
      slot.provider = "gguf";
      slot.gguf_path = expand_home(value);
      slot.model.clear();
    }
    save_config(config, config_file);
    return {"Saved " + name + " for " + role + ".", {}};
  }
  if (name == "/model") {
    std::string provider;
    std::string model;
    command >> provider >> model;
    if (role.empty() || provider.empty() || model.empty()) {
      return {"Usage: /model ROLE PROVIDER MODEL", {}};
    }
    if (provider != "ollama" && provider != "openai" &&
        provider != "anthropic" && provider != "gguf") {
      return {"Provider must be ollama, openai, anthropic, or gguf.", {}};
    }
    auto& slot = find_slot(config, role);
    if (slot.provider != provider) slot.api_key.clear();
    slot.provider = provider;
    if (provider == "gguf") {
      slot.gguf_path = expand_home(model);
      slot.model.clear();
    } else {
      slot.model = model;
    }
    save_config(config, config_file);
    return {"Saved " + role + " model assignment.", {}};
  }
  return {"Unknown command. Type /help.", {}};
}

std::string store_api_key(Config& config,
                          const std::filesystem::path& config_file,
                          const std::string& role, std::string key) {
  auto& slot = find_slot(config, role);
  slot.api_key = std::move(key);
  save_config(config, config_file);
  return slot.api_key.empty() ? "Saved API key cleared for " + role + "."
                              : "Saved API key for " + role + ".";
}

// Worker thread -> UI thread communication. Only the UI thread touches FTXUI
// state; the worker appends under the mutex and posts a wake-up event.
struct UiChannel {
  std::mutex mutex;
  std::deque<std::string> messages;  // status and progress lines
  std::deque<std::string> tokens;    // streamed text chunks
  bool review_pending = false;
  std::string review_text;
  std::promise<bool>* review_promise = nullptr;
  bool done = false;
  ftxui::App* screen = nullptr;
};

void wake_ui(UiChannel& channel) {
  if (channel.screen != nullptr) {
    channel.screen->PostEvent(ftxui::Event::Custom);
  }
}

void push_message(UiChannel& channel, const std::string& line) {
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    channel.messages.push_back(safe_terminal_text(line));
  }
  wake_ui(channel);
}

void push_token(UiChannel& channel, const std::string& text) {
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    channel.tokens.push_back(safe_terminal_text(text));
  }
  wake_ui(channel);
}

// Mark the job done and deliver its final line in one step. The UI can then
// trust that seeing `done` also means the final message is already queued, so
// an immediate Enter can never return to the prompt before the message lands.
void push_final_message(UiChannel& channel, const std::string& line) {
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    channel.done = true;
    channel.messages.push_back(safe_terminal_text(line));
  }
  wake_ui(channel);
}

// Runs on the worker thread: parks the proposal with the UI thread and waits
// for the rendered decision.
bool ask_review(UiChannel& channel, const std::string& text) {
  std::promise<bool> promise;
  std::future<bool> answered = promise.get_future();
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    if (channel.review_pending) {
      return false;  // defensive; never two at once
    }
    channel.review_pending = true;
    channel.review_text = text;
    channel.review_promise = &promise;
  }
  wake_ui(channel);
  const bool outcome = answered.get();  // wait for the user's decision
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    channel.review_pending = false;
    channel.review_promise = nullptr;
    channel.review_text.clear();
  }
  return outcome;
}

class Ui {
 public:
  Ui(Config& config, const std::filesystem::path& config_file)
      : config_(config),
        config_file_(config_file),
        pipeline_(config) {
    channel_.screen = &screen_;
  }

  ~Ui() { stop_worker(); }

  void run(const std::filesystem::path& resume_session) {
    if (!resume_session.empty()) start_resume(resume_session);
    auto root = ftxui::CatchEvent(
        ftxui::Renderer([this] { return render(); }),
        [this](ftxui::Event event) { return on_event(std::move(event)); });
    // The signal handler only sets a token. Poll it from a helper thread so the
    // UI loop wakes up and can stop the worker, unload the model, and restore
    // the terminal through the normal shutdown path.
    std::atomic<bool> monitor_stop{false};
    std::thread monitor([this, &monitor_stop]() {
      while (!monitor_stop.load(std::memory_order_relaxed)) {
        if (process_cancel_token().cancelled()) {
          screen_.PostEvent(ftxui::Event::Custom);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
    screen_.Loop(root);
    monitor_stop.store(true, std::memory_order_relaxed);
    if (monitor.joinable()) monitor.join();
    stop_worker();
  }

 private:
  enum class Mode { prompt, notice, secret };

  std::string welcome_text() const {
    std::string welcome =
        "Your coding team is ready.\n\n"
        "Describe what you want to build. The Chief of Staff frames the task, "
        "the Prompter creates a detailed implementation brief, and the Coder "
        "turns it into reviewed changes.\n\n"
        "Roles run sequentially to reduce memory use. Handoffs and progress "
        "are saved to disk as the work proceeds.\n\n"
        "Type a task below, or enter /help to see commands.";
    if (config_.coder.provider == "gguf" &&
        config_.coder.gguf_path.empty()) {
      welcome +=
          "\nCoder GGUF path is unset. Use /gguf coder "
          "/path/to/your/model.gguf to configure your local model.";
    }
    return welcome;
  }

  std::string transcript_text() const {
    std::string text;
    for (const auto& line : transcript_) {
      text += line;
      text += '\n';
    }
    text += live_;
    return text;
  }

  std::string content_text() const {
    if (mode_ == Mode::notice || mode_ == Mode::secret) return notice_;
    if (running_) return transcript_text();
    if (transcript_.empty()) return welcome_text();
    return transcript_text();
  }

  std::string footer_text() const {
    if (running_) {
      return done_ ? "done - press Enter to start another task"
                   : "working - Esc cancels the current job";
    }
    if (mode_ == Mode::notice) return "press any key to continue";
    if (mode_ == Mode::secret) return "Enter saves   Esc cancels";
    return "Enter to send   /help commands   /quit exit";
  }

  ftxui::Element render_sidebar() const {
    using namespace ftxui;
    const ModelSlot* slots[] = {&config_.chief, &config_.prompter,
                                &config_.coder};
    const char* names[] = {"Chief of Staff", "Prompter", "Coder"};
    Elements rows;
    for (std::size_t i = 0; i < 3; ++i) {
      rows.push_back(text("0" + std::to_string(i + 1) + "  " + names[i]) |
                     bold | color(Color::GreenLight));
      rows.push_back(text("     " + slots[i]->provider) | dim |
                     color(Color::GrayDark));
      std::string model = slots[i]->model.empty()
                              ? std::filesystem::path(slots[i]->gguf_path)
                                    .filename()
                                    .string()
                              : slots[i]->model;
      if (model.empty()) model = "model not configured";
      rows.push_back(text("     " + model) | dim | color(Color::GrayDark));
      rows.push_back(text(""));
    }
    rows.push_back(filler());
    rows.push_back(text("SEQUENTIAL / LOW RAM") | dim | color(Color::GrayDark));
    return window(text("AGENTS") | bold | color(Color::Green),
                  vbox(std::move(rows))) |
           size(WIDTH, EQUAL, kSidebarWidth);
  }

  ftxui::Element render_input_text() const {
    using namespace ftxui;
    if (running_ || mode_ == Mode::notice) return text("");
    const bool hidden = mode_ == Mode::secret;
    const std::string& value = hidden ? secret_ : input_;
    const std::string shown =
        hidden ? std::string(value.size(), '*') : value;
    const std::size_t cursor = hidden ? shown.size() : cursor_;
    const std::size_t before_end = std::min(cursor, shown.size());
    const std::size_t after_start =
        before_end < shown.size() ? utf8_next_char_end(shown, before_end)
                                  : shown.size();
    const std::string before = shown.substr(0, before_end);
    const std::string at =
        before_end < shown.size()
            ? shown.substr(before_end, after_start - before_end)
            : " ";
    const std::string after = shown.substr(after_start);
    return hbox({
               text(before),
               text(at) | inverted | focus,
               text(after),
           }) |
           xframe;
  }

  ftxui::Element render_input_panel() const {
    using namespace ftxui;
    return window(text("MESSAGE") | bold | color(Color::Green),
                  hbox({text("> ") | color(Color::GrayDark),
                        render_input_text()})) |
           size(HEIGHT, EQUAL, 3);
  }

  ftxui::Element render_content() const {
    using namespace ftxui;
    const auto terminal = Terminal::Size();
    const bool sidebar = terminal.dimx >= 90;
    const int inner_width =
        std::max(1, terminal.dimx - (sidebar ? kSidebarWidth + 3 : 2));
    const int inner_height = std::max(1, terminal.dimy - 8);
    const auto lines = wrap_lines(content_text(), inner_width);
    const std::size_t page = std::min<std::size_t>(
        static_cast<std::size_t>(inner_height), lines.size());
    const std::size_t first = lines.size() - page;
    Elements visible;
    for (std::size_t row = 0; row < page; ++row) {
      const std::string& line = lines[first + row];
      visible.push_back(text(line.empty() ? " " : line));
    }
    const std::string title = running_ ? "mtrxAI / working" : "mtrxAI";
    return window(text(title) | bold | color(Color::Green),
                  vbox(std::move(visible)) | flex) |
           flex;
  }

  // Last transcript lines shown above an approval. They keep the reason for
  // the request in view, such as which saved session is being resumed.
  ftxui::Elements history_context() const {
    using namespace ftxui;
    Elements rows;
    const int width = std::max(1, Terminal::Size().dimx - 2);
    if (!task_context_.empty()) {
      rows.push_back(text(task_context_) | color(Color::GreenLight));
    }
    const std::size_t first =
        transcript_.size() > 2 ? transcript_.size() - 2 : 0;
    for (std::size_t i = first; i < transcript_.size(); ++i) {
      if (!task_context_.empty() && transcript_[i] == task_context_) continue;
      const auto wrapped = wrap_lines(transcript_[i], width);
      if (!wrapped.empty()) {
        rows.push_back(text(wrapped.front()) | dim | color(Color::GrayDark));
      }
    }
    return rows;
  }

  int review_page() const {
    int context =
        static_cast<int>(std::min<std::size_t>(2, transcript_.size()));
    if (!task_context_.empty()) ++context;
    return std::max(1, ftxui::Terminal::Size().dimy - 4 - context);
  }

  ftxui::Element render_review() {
    using namespace ftxui;
    const auto terminal = Terminal::Size();
    const int width = std::max(1, terminal.dimx - 2);
    const int page = review_page();
    const auto lines = wrap_lines(review_text_, width);
    const std::size_t max_offset =
        lines.size() > static_cast<std::size_t>(page)
            ? lines.size() - static_cast<std::size_t>(page)
            : 0;
    review_offset_ = std::min(review_offset_, max_offset);
    Elements visible;
    for (int row = 0; row < page; ++row) {
      const std::size_t index =
          review_offset_ + static_cast<std::size_t>(row);
      const std::string line =
          index < lines.size() ? lines[index] : std::string();
      visible.push_back(text(line.empty() ? " " : line));
    }
    Elements rows;
    rows.push_back(text(" REVIEW REQUEST") | bold | color(Color::Green));
    for (auto& row : history_context()) rows.push_back(std::move(row));
    rows.push_back(separator() | color(Color::Green));
    rows.push_back(vbox(std::move(visible)) | flex);
    rows.push_back(separator() | color(Color::GrayDark));
    rows.push_back(
        text(" ↑/↓ or PgUp/PgDn to review  |  y approve  |  n decline") |
        color(Color::Green));
    return vbox(std::move(rows));
  }

  ftxui::Element render() {
    using namespace ftxui;
    if (review_active_) return render_review();
    const auto terminal = Terminal::Size();
    Elements rows;
    rows.push_back(hbox({
        text(" mtrxAI ") | bold | color(Color::Green),
        filler(),
        text("THREE ROLES  /  ONE MODEL IN MEMORY") | dim |
            color(Color::GrayDark),
    }));
    rows.push_back(separator() | color(Color::Green));
    if (terminal.dimx >= 90) {
      rows.push_back(hbox({
                         render_sidebar(),
                         separator() | color(Color::GrayDark),
                         render_content() | flex,
                     }) |
                     flex);
    } else {
      rows.push_back(render_content() | flex);
    }
    rows.push_back(render_input_panel());
    rows.push_back(hbox(
        {text(footer_text()) | dim | color(Color::GrayDark)}));
    return vbox(std::move(rows));
  }

  void prepare_worker() {
    {
      std::lock_guard<std::mutex> guard(channel_.mutex);
      channel_.done = false;
      channel_.messages.clear();
      channel_.tokens.clear();
      channel_.review_pending = false;
      channel_.review_text.clear();
      channel_.review_promise = nullptr;
    }
    done_ = false;
    enter_after_done_ = false;
    review_active_ = false;
    review_promise_ = nullptr;
    review_text_.clear();
    review_offset_ = 0;
    mode_ = Mode::prompt;
    notice_.clear();
  }

  void run_job(const std::function<void()>& job) {
    std::string final_message;
    try {
      job();
      final_message = "Finished.";
    } catch (const Cancelled&) {
      final_message =
          "Cancelled. Completed stage outputs remain saved on disk.";
    } catch (const std::exception& error) {
      final_message = std::string("Pipeline stopped: ") + error.what() +
                      "\nCompleted stage outputs remain saved on disk.";
    }
    push_final_message(channel_, final_message);
  }

  void start_worker_common() {
    prepare_worker();
    running_ = true;
  }

  void start_run(std::string input) {
    transcript_.clear();
    live_.clear();
    task_context_ = "Working on: " + input;
    transcript_.push_back(task_context_);
    start_worker_common();
    worker_ = std::thread([this, input = std::move(input)]() mutable {
      run_job([this, &input]() {
        pipeline_.run(
            input,
            [this](const std::string& text) { push_message(channel_, text); },
            [this](const std::string& text) {
              return ask_review(channel_, text);
            },
            [this](const std::string& text) { push_token(channel_, text); });
      });
    });
  }

  void start_resume(const std::filesystem::path& session) {
    try {
      const std::string saved_request = read_session_request(session);
      transcript_.clear();
      live_.clear();
      task_context_ = "Resuming session " + session.filename().string();
      transcript_.push_back(task_context_);
      transcript_.push_back("Working on: " + saved_request);
      start_worker_common();
      worker_ = std::thread([this, session]() {
        run_job([this, &session]() {
          pipeline_.resume(
              session,
              [this](const std::string& text) {
                push_message(channel_, text);
              },
              [this](const std::string& text) {
                return ask_review(channel_, text);
              },
              [this](const std::string& text) {
                push_token(channel_, text);
              });
        });
      });
    } catch (const std::exception& error) {
      running_ = false;
      mode_ = Mode::notice;
      notice_ = "Could not resume saved session: " +
                std::string(error.what());
    }
  }

  void finish_worker() {
    if (worker_.joinable()) worker_.join();
    running_ = false;
    done_ = false;
    enter_after_done_ = false;
    transcript_.clear();
    live_.clear();
    task_context_.clear();
    input_.clear();
    cursor_ = 0;
    mode_ = Mode::prompt;
    notice_.clear();
  }

  void stop_worker() {
    if (!worker_.joinable()) return;
    pipeline_.request_cancel();
    std::promise<bool>* pending = nullptr;
    {
      std::lock_guard<std::mutex> guard(channel_.mutex);
      if (channel_.review_pending && channel_.review_promise != nullptr) {
        pending = channel_.review_promise;
        channel_.review_pending = false;
        channel_.review_promise = nullptr;
      }
    }
    if (pending != nullptr) pending->set_value(false);
    if (review_promise_ != nullptr) {
      review_promise_->set_value(false);
      review_promise_ = nullptr;
      review_active_ = false;
    }
    worker_.join();
  }

  void pump() {
    std::deque<std::string> messages;
    std::deque<std::string> tokens;
    bool done = false;
    std::promise<bool>* pending = nullptr;
    std::string pending_text;
    {
      std::lock_guard<std::mutex> guard(channel_.mutex);
      messages.swap(channel_.messages);
      tokens.swap(channel_.tokens);
      done = channel_.done;
      if (channel_.review_pending && channel_.review_promise != nullptr) {
        pending = channel_.review_promise;
        pending_text = channel_.review_text;
        channel_.review_pending = false;
      }
    }
    for (auto& message : messages) {
      if (!live_.empty()) {
        transcript_.push_back(std::move(live_));
        live_.clear();
      }
      transcript_.push_back(std::move(message));
    }
    for (auto& token : tokens) live_ += token;
    while (transcript_.size() > 500) transcript_.pop_front();
    if (pending != nullptr) {
      review_active_ = true;
      review_text_ = safe_terminal_text(pending_text);
      review_promise_ = pending;
      review_offset_ = 0;
    }
    if (done) done_ = true;
    if (done_ && enter_after_done_) {
      enter_after_done_ = false;
      finish_worker();
    }
  }

  void decide_review(bool approved) {
    review_active_ = false;
    review_offset_ = 0;
    review_text_.clear();
    std::promise<bool>* promise = review_promise_;
    review_promise_ = nullptr;
    if (promise != nullptr) promise->set_value(approved);
  }

  bool handle_review(const ftxui::Event& event) {
    using namespace ftxui;
    if (event == Event::Character("y") || event == Event::Character("Y")) {
      decide_review(true);
      return true;
    }
    if (event == Event::Character("n") || event == Event::Character("N") ||
        event == Event::Character("q") || event == Event::Character("Q") ||
        event == Event::Escape) {
      decide_review(false);
      return true;
    }
    const auto terminal = Terminal::Size();
    const int page = review_page();
    const auto lines =
        wrap_lines(review_text_, std::max(1, terminal.dimx - 2));
    const std::size_t max_offset =
        lines.size() > static_cast<std::size_t>(page)
            ? lines.size() - static_cast<std::size_t>(page)
            : 0;
    if (event == Event::ArrowDown) {
      review_offset_ = std::min(max_offset, review_offset_ + 1);
    } else if (event == Event::ArrowUp) {
      if (review_offset_ > 0) --review_offset_;
    } else if (event == Event::PageDown) {
      review_offset_ =
          std::min(max_offset, review_offset_ + static_cast<std::size_t>(page));
    } else if (event == Event::PageUp) {
      review_offset_ = review_offset_ > static_cast<std::size_t>(page)
                           ? review_offset_ - static_cast<std::size_t>(page)
                           : 0;
    }
    return true;
  }

  void dismiss_notice() {
    mode_ = Mode::prompt;
    notice_.clear();
    transcript_.clear();
    live_.clear();
    input_.clear();
    cursor_ = 0;
  }

  void submit_input() {
    std::string input = input_;
    input_.clear();
    cursor_ = 0;
    if (input.empty()) return;
    if (input == "/quit") {
      screen_.Exit();
      return;
    }
    if (input[0] == '/') {
      try {
        const CommandResult result =
            execute_command(config_, config_file_, input);
        if (!result.secret_role.empty()) {
          mode_ = Mode::secret;
          secret_role_ = result.secret_role;
          secret_.clear();
          notice_ = "API key for " + result.secret_role +
                    " (input is hidden; Enter saves, Esc cancels).";
        } else {
          mode_ = Mode::notice;
          notice_ = result.text;
        }
      } catch (const std::exception& error) {
        mode_ = Mode::notice;
        notice_ = std::string("Error: ") + error.what();
      }
      return;
    }
    start_run(std::move(input));
  }

  bool handle_secret(const ftxui::Event& event) {
    using namespace ftxui;
    if (event == Event::Escape) {
      secret_.clear();
      secret_role_.clear();
      mode_ = Mode::notice;
      notice_ = "API key entry cancelled.";
      return true;
    }
    if (event == Event::Return) {
      std::string key = secret_;
      secret_.clear();
      try {
        notice_ = store_api_key(config_, config_file_, secret_role_,
                                std::move(key));
      } catch (const std::exception& error) {
        notice_ = std::string("Error: ") + error.what();
      }
      secret_role_.clear();
      mode_ = Mode::notice;
      return true;
    }
    if (event == Event::Backspace) {
      if (!secret_.empty()) {
        secret_.erase(utf8_prev_char_start(secret_, secret_.size()));
      }
      return true;
    }
    if (event.is_character()) {
      const std::string character = event.character();
      if (!character.empty() &&
          static_cast<unsigned char>(character[0]) >= 0x20 &&
          secret_.size() + character.size() <= 4096) {
        secret_ += character;
      }
      return true;
    }
    return true;
  }

  bool handle_prompt(const ftxui::Event& event) {
    using namespace ftxui;
    if (event == Event::Return) {
      submit_input();
      return true;
    }
    if (event == Event::Escape) {
      input_.clear();
      cursor_ = 0;
      return true;
    }
    if (event == Event::Backspace) {
      if (cursor_ > 0) {
        const std::size_t start = utf8_prev_char_start(input_, cursor_);
        input_.erase(start, cursor_ - start);
        cursor_ = start;
      }
      return true;
    }
    if (event == Event::Delete) {
      if (cursor_ < input_.size()) {
        const std::size_t end = utf8_next_char_end(input_, cursor_);
        input_.erase(cursor_, end - cursor_);
      }
      return true;
    }
    if (event == Event::ArrowLeft) {
      if (cursor_ > 0) cursor_ = utf8_prev_char_start(input_, cursor_);
      return true;
    }
    if (event == Event::ArrowRight) {
      if (cursor_ < input_.size()) cursor_ = utf8_next_char_end(input_, cursor_);
      return true;
    }
    if (event == Event::Home || event == Event::CtrlA) {
      cursor_ = 0;
      return true;
    }
    if (event == Event::End || event == Event::CtrlE) {
      cursor_ = input_.size();
      return true;
    }
    if (event.is_character()) {
      const std::string character = event.character();
      if (!character.empty() &&
          static_cast<unsigned char>(character[0]) >= 0x20 &&
          input_.size() + character.size() <= 8191) {
        input_.insert(cursor_, character);
        cursor_ += character.size();
      }
      return true;
    }
    return true;
  }

  bool handle_running(const ftxui::Event& event) {
    using namespace ftxui;
    if (event == Event::Escape && !done_) {
      pipeline_.request_cancel();
      if (!live_.empty()) {
        transcript_.push_back(std::move(live_));
        live_.clear();
      }
      transcript_.push_back(
          "Esc: cancelling after the current request...");
      return true;
    }
    if (event == Event::Return) {
      if (done_) {
        finish_worker();
      } else {
        enter_after_done_ = true;
      }
      return true;
    }
    return true;
  }

  bool on_event(ftxui::Event event) {
    using namespace ftxui;
    // SIGINT/SIGTERM only sets the process token. Stop the worker here so its
    // destructor stops any llama-server and unloads Ollama before we exit.
    if (process_cancel_token().cancelled()) {
      stop_worker();
      screen_.Exit();
      return true;
    }
    // Let FTXUI turn Ctrl-C / Ctrl-Z into the signals its terminal cleanup
    // already knows how to handle.
    if (event == Event::CtrlC || event == Event::CtrlZ) return false;
    pump();
    if (review_active_) return handle_review(event);
    if (mode_ == Mode::notice) {
      dismiss_notice();
      return true;
    }
    if (mode_ == Mode::secret) return handle_secret(event);
    if (running_) return handle_running(event);
    return handle_prompt(event);
  }

  Config& config_;
  std::filesystem::path config_file_;
  Pipeline pipeline_;
  ftxui::App screen_ = ftxui::App::Fullscreen();
  UiChannel channel_;
  std::thread worker_;
  bool running_ = false;
  bool done_ = false;
  bool enter_after_done_ = false;
  std::deque<std::string> transcript_;
  std::string live_;
  std::string input_;
  std::size_t cursor_ = 0;
  Mode mode_ = Mode::prompt;
  std::string notice_;
  std::string secret_;
  std::string secret_role_;
  bool review_active_ = false;
  std::string review_text_;
  std::string task_context_;
  std::promise<bool>* review_promise_ = nullptr;
  std::size_t review_offset_ = 0;
};

}  // namespace

int run_tui(Config& config, const std::filesystem::path& config_file,
            const std::filesystem::path& resume_session) {
  Ui ui(config, config_file);
  ui.run(resume_session);
  const int signal = process_signal();
  if (process_cancel_token().cancelled() && signal != 0) {
    return 128 + signal;
  }
  return 0;
}

}  // namespace mtrxai
