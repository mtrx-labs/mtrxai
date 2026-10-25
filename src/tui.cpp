#include "mtrxai/tui.hpp"

#include "mtrxai/pipeline.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/utf8.hpp"

#include <ncurses.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace mtrxai {
namespace {

constexpr int kAccent = 1;
constexpr int kAccentSoft = 2;
constexpr int kMuted = 3;
constexpr int kPanel = 4;
constexpr int kSidebarWidth = 25;
const Config* screen_config = nullptr;

struct ScreenGuard {
  ~ScreenGuard() {
    mtrxai::signals::set_curses_active(false);
    endwin();
  }
};

// Read one key through ncursesw's get_wch so a typed character arrives as a
// single wchar_t regardless of how many bytes its UTF-8 form needs. Function
// and arrow keys are reported as key codes instead.
struct KeyEvent {
  bool is_code = true;  // true when this is a KEY_* code, not a character
  int code = ERR;       // only meaningful when is_code is true
  wchar_t character = 0;
};

KeyEvent next_key() {
  wint_t value = 0;
  const int status = get_wch(&value);
  if (status == ERR) return {true, ERR, 0};
  if (status == KEY_CODE_YES) return {true, static_cast<int>(value), 0};
  return {false, 0, static_cast<wchar_t>(value)};
}

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

void draw_header() {
  const std::string brand = " mtrxAI ";
  attron(A_BOLD | COLOR_PAIR(kAccent));
  mvaddnstr(0, 1, brand.c_str(), COLS - 2);
  attroff(A_BOLD | COLOR_PAIR(kAccent));
  const std::string tagline = "THREE ROLES  /  ONE MODEL IN MEMORY";
  if (COLS > static_cast<int>(brand.size()) + 8) {
    attron(A_DIM | COLOR_PAIR(kMuted));
    mvaddnstr(0, std::max(1, COLS - static_cast<int>(tagline.size()) - 2),
              tagline.c_str(), COLS - 2);
    attroff(A_DIM | COLOR_PAIR(kMuted));
  }
  attron(COLOR_PAIR(kAccentSoft));
  mvhline(1, 0, ACS_HLINE, COLS);
  attroff(COLOR_PAIR(kAccentSoft));
}

void draw_panel(int left, int right, int top, int bottom,
                const std::string& title) {
  if (bottom <= top || top < 0 || bottom >= LINES || right <= left ||
      left < 0 || right >= COLS) {
    return;
  }
  attron(COLOR_PAIR(kPanel));
  mvaddch(top, left, ACS_ULCORNER);
  mvaddch(top, right, ACS_URCORNER);
  mvaddch(bottom, left, ACS_LLCORNER);
  mvaddch(bottom, right, ACS_LRCORNER);
  mvhline(top, left + 1, ACS_HLINE, std::max(0, right - left - 1));
  mvhline(bottom, left + 1, ACS_HLINE, std::max(0, right - left - 1));
  for (int row = top + 1; row < bottom; ++row) {
    mvaddch(row, left, ACS_VLINE);
    mvaddch(row, right, ACS_VLINE);
  }
  attroff(COLOR_PAIR(kPanel));
  if (!title.empty() && right - left > 6) {
    attron(A_BOLD | COLOR_PAIR(kAccent));
    mvaddnstr(top, left + 2, (" " + title + " ").c_str(), right - left - 2);
    attroff(A_BOLD | COLOR_PAIR(kAccent));
  }
}

std::vector<std::string> wrap_lines(const std::string& text, int width);

int content_left() { return COLS >= 90 ? kSidebarWidth + 1 : 0; }

// A decision screen is rare and human-paced, so it is always written back in
// full. ncurses normally rewrites only cells whose bytes changed, so two
// identical reviews in a row would add nothing to the terminal stream — a
// user would not notice, but a screen reader or test replaying the byte
// stream would see an empty screen. redrawwin tells ncurses the physical
// screen is unknown so every cell is re-emitted on the next refresh.
void repaint_fully() {
  redrawwin(stdscr);
  refresh();
}

void draw_sidebar(int bottom) {
  if (COLS < 90 || screen_config == nullptr || bottom < 5) return;
  draw_panel(0, kSidebarWidth, 3, bottom, "AGENTS");
  const ModelSlot* slots[] = {&screen_config->chief, &screen_config->prompter,
                              &screen_config->coder};
  const char* names[] = {"Chief of Staff", "Prompter", "Coder"};
  int row = 5;
  for (std::size_t i = 0; i < 3 && row + 2 < bottom; ++i) {
    attron(A_BOLD | COLOR_PAIR(kAccentSoft));
    mvaddnstr(row++, 2, ("0" + std::to_string(i + 1) + "  " + names[i]).c_str(),
              kSidebarWidth - 3);
    attroff(A_BOLD | COLOR_PAIR(kAccentSoft));
    attron(A_DIM | COLOR_PAIR(kMuted));
    mvaddnstr(row++, 5, slots[i]->provider.c_str(), kSidebarWidth - 6);
    std::string model = slots[i]->model.empty()
                            ? std::filesystem::path(slots[i]->gguf_path).filename().string()
                            : slots[i]->model;
    if (model.empty()) model = "model not configured";
    mvaddnstr(row++, 5, model.c_str(), kSidebarWidth - 6);
    attroff(A_DIM | COLOR_PAIR(kMuted));
    ++row;
  }
  if (row + 2 < bottom) {
    attron(A_DIM | COLOR_PAIR(kMuted));
    mvaddnstr(bottom - 2, 2, "SEQUENTIAL / LOW RAM", kSidebarWidth - 3);
    attroff(A_DIM | COLOR_PAIR(kMuted));
  }
}

void draw_content(const std::string& text, const std::string& panel_title) {
  erase();
  draw_header();
  const int content_top = 3;
  const int content_bottom = std::max(content_top + 1, LINES - 5);
  const int right = COLS - 1;
  const int left = content_left();
  draw_sidebar(LINES - 2);
  draw_panel(left, right, content_top,
             std::min(content_bottom, LINES - 2), panel_title);
  const int content_height = std::max(
      0, std::min(content_bottom, LINES - 2) - content_top - 1);
  const auto lines = wrap_lines(text, std::max(1, right - left - 4));
  // Show the newest lines: the transcript grows as work streams in, and on a
  // small terminal the messages that matter (status, completion) would end up
  // past the first screenful and never be seen.
  const int page = std::min<int>(content_height,
                                 static_cast<int>(lines.size()));
  const std::size_t first = lines.size() - static_cast<std::size_t>(page);
  for (int row = 0; row < page; ++row) {
    mvaddnstr(content_top + row + 1, left + 2,
              lines[first + static_cast<std::size_t>(row)].c_str(),
              std::max(1, right - left - 2));
  }
  if (LINES >= 8) draw_panel(left, right, LINES - 4, LINES - 2, "MESSAGE");
  attron(A_DIM | COLOR_PAIR(kMuted));
  mvaddnstr(LINES - 1, left + 2, "Enter to send   /help commands   /quit exit",
            std::max(1, right - left - 2));
  attroff(A_DIM | COLOR_PAIR(kMuted));
  refresh();
}

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

void show(const std::string& text) { draw_content(text, "mtrxAI"); }

std::string input_line(const std::string& initial = {}) {
  std::string value = initial;
  std::size_t cursor = value.size();
  while (true) {
    draw_content("", "mtrxAI");
    if (LINES >= 8) {
      const int left = content_left();
      attron(COLOR_PAIR(kMuted));
      mvaddstr(LINES - 3, left + 2, "> ");
      attroff(COLOR_PAIR(kMuted));
      const int width = std::max(1, COLS - left - 7);
      const std::size_t start = cursor > static_cast<std::size_t>(width - 1)
                                    ? cursor - static_cast<std::size_t>(width - 1)
                                    : 0;
      const std::string shown = value.substr(start, static_cast<std::size_t>(width));
      mvaddnstr(LINES - 3, left + 4, shown.c_str(), width);
      move(LINES - 3, left + 4 + static_cast<int>(cursor - start));
      curs_set(1);
      refresh();
    }

    const KeyEvent key = next_key();
    if (key.is_code && key.code == KEY_RESIZE) continue;
    if (key.is_code && key.code == ERR) return {};
    // Enter can arrive as a character code (0) or, rarely, as KEY_ENTER.
    if ((key.is_code && (key.code == '\n' || key.code == '\r')) ||
        (!key.is_code && (key.character == L'\n' || key.character == L'\r' ||
                          key.character == KEY_ENTER))) {
      return value;
    }
    if (!key.is_code && key.character == 27) return {};
    if (key.is_code && key.code == KEY_LEFT) {
      if (cursor > 0) cursor = mtrxai::utf8_prev_char_start(value, cursor);
    } else if (key.is_code && key.code == KEY_RIGHT) {
      if (cursor < value.size()) cursor = mtrxai::utf8_next_char_end(value, cursor);
    } else if (key.is_code && (key.code == KEY_HOME || key.code == 1)) {
      cursor = 0;
    } else if (key.is_code && (key.code == KEY_END || key.code == 5)) {
      cursor = value.size();
    } else if (key.is_code &&
               (key.code == KEY_BACKSPACE || key.code == 127 || key.code == 8)) {
      if (cursor > 0) {
        const std::size_t start = mtrxai::utf8_prev_char_start(value, cursor);
        value.erase(start, cursor - start);
        cursor = start;
      }
    } else if (key.is_code && key.code == KEY_DC) {
      if (cursor < value.size()) {
        const std::size_t end = mtrxai::utf8_next_char_end(value, cursor);
        value.erase(cursor, end - cursor);
      }
    } else if (!key.is_code && std::iswprint(key.character) &&
               value.size() < 8191) {
      const std::string encoded = mtrxai::utf8_encode(key.character);
      value.insert(cursor, encoded);
      cursor += encoded.size();
    }
  }
}

bool review(const std::string& text) {
  timeout(-1);  // decisions need blocking input; the caller re-arms the pump
  const auto lines = wrap_lines(text, std::max(1, COLS - 2));
  std::size_t offset = 0;
  const std::size_t page = static_cast<std::size_t>(std::max(1, LINES - 4));
  while (true) {
    erase();
    attron(A_BOLD | COLOR_PAIR(kAccent));
    mvaddnstr(0, 1, " REVIEW REQUEST", COLS - 2);
    attroff(A_BOLD | COLOR_PAIR(kAccent));
    attron(COLOR_PAIR(kAccentSoft));
    mvhline(1, 0, ACS_HLINE, COLS);
    attroff(COLOR_PAIR(kAccentSoft));
    for (std::size_t row = 0; row < page && offset + row < lines.size(); ++row) {
      mvaddnstr(static_cast<int>(row) + 2, 1, lines[offset + row].c_str(),
                COLS - 2);
    }
    mvhline(LINES - 2, 0, ACS_HLINE, COLS);
    const std::string footer =
        " ↑/↓ or PgUp/PgDn to review  |  y approve  |  n decline";
    attron(COLOR_PAIR(kAccent));
    mvaddnstr(LINES - 1, 1, footer.c_str(), COLS - 2);
    attroff(COLOR_PAIR(kAccent));
    repaint_fully();

    const int key = getch();
    if (key == 'y' || key == 'Y') return true;
    if (key == 'n' || key == 'N' || key == 'q' || key == 'Q' || key == 27 ||
        key == ERR) {
      return false;
    }
    if (key == KEY_DOWN && offset + page < lines.size()) ++offset;
    if (key == KEY_UP && offset > 0) --offset;
    if (key == KEY_NPAGE) {
      offset = std::min(lines.size() - 1, offset + page);
    }
    if (key == KEY_PPAGE) {
      offset = offset > page ? offset - page : 0;
    }
  }
}

void show_roles(const Config& config) {
  std::ostringstream text;
  for (const auto* name : {"chief", "prompter", "coder"}) {
    const std::string role(name);
    const auto& slot = find_slot(config, role);
    const std::string model =
        slot.model.empty() ? (slot.gguf_path.empty() ? "(GGUF path not configured)"
                                                     : slot.gguf_path)
                           : slot.model;
    text << slot.name << ": " << slot.provider << " / "
         << model << " (ctx: " << slot.context_size << ", max_tokens: " << slot.max_tokens << ")\n";
  }
  text << "Pipeline: max turns: " << config.max_turns
       << ", GGUF load timeout: " << config.gguf_load_timeout_seconds << "s\n";
  show(text.str());
}

void command_help() {
  show("Commands:\n"
       "  /roles                         show active model assignments & limits\n"
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
       "  /quit                           exit");
}

std::optional<std::string> read_api_key(const std::string& role) {
  show("API key for " + role +
       " (input is hidden; Enter saves, Esc cancels).");
  std::string key;
  while (true) {
    const KeyEvent event = next_key();
    if ((event.is_code && event.code == ERR) ||
        (!event.is_code && event.character == 27)) {
      return std::nullopt;
    }
    if ((!event.is_code &&
         (event.character == L'\n' || event.character == L'\r')) ||
        (event.is_code && (event.code == '\n' || event.code == '\r' ||
                           event.code == KEY_ENTER))) {
      return key;
    }
    if (event.is_code &&
        (event.code == KEY_BACKSPACE || event.code == 127 || event.code == 8)) {
      if (!key.empty()) key.erase(mtrxai::utf8_prev_char_start(key, key.size()));
      show("API key for " + role + "\n" + std::string(key.size(), '*'));
      continue;
    }
    if (!event.is_code && std::iswprint(event.character) &&
        key.size() < 4096) {
      key += mtrxai::utf8_encode(event.character);
      show("API key for " + role + "\n" + std::string(key.size(), '*'));
    }
  }
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

void handle_command(Config& config, const std::filesystem::path& config_file,
                    const std::string& input) {
  std::istringstream command(input);
  std::string name;
  command >> name;
  if (name == "/roles") {
    show_roles(config);
    return;
  }
  if (name == "/help") {
    command_help();
    return;
  }
  if (name == "/maxturns") {
    std::string value;
    command >> value;
    int turns = 0;
    try {
      turns = std::stoi(value);
    } catch (...) {
      show("Usage: /maxturns NUMBER (1-500)");
      return;
    }
    if (turns < 1 || turns > 500) {
      show("max_turns must be between 1 and 500.");
      return;
    }
    config.max_turns = turns;
    save_config(config, config_file);
    show("Saved max turns: " + std::to_string(turns) + ".");
    return;
  }
  if (name == "/gguftimeout") {
    std::string value;
    command >> value;
    int timeout = 0;
    try {
      timeout = std::stoi(value);
    } catch (...) {
      show("Usage: /gguftimeout SECONDS (1-3600)");
      return;
    }
    if (timeout < 1 || timeout > 3600) {
      show("gguf_load_timeout_seconds must be between 1 and 3600.");
      return;
    }
    config.gguf_load_timeout_seconds = timeout;
    config.chief.gguf_load_timeout_seconds = timeout;
    config.prompter.gguf_load_timeout_seconds = timeout;
    config.coder.gguf_load_timeout_seconds = timeout;
    save_config(config, config_file);
    show("Saved GGUF load timeout: " + std::to_string(timeout) + " seconds.");
    return;
  }
  std::string role;
  command >> role;
  if (name == "/context") {
    std::string value;
    command >> value;
    if (role.empty() || value.empty()) {
      show("Usage: /context ROLE SIZE (512-32768)");
      return;
    }
    int size = 0;
    try {
      size = std::stoi(value);
    } catch (...) {
      show("Invalid context size.");
      return;
    }
    if (size < 512 || size > 32768) {
      show("context_size must be between 512 and 32768.");
      return;
    }
    auto& slot = find_slot(config, role);
    slot.context_size = size;
    save_config(config, config_file);
    show("Saved context size " + std::to_string(size) + " for " + role + ".");
    return;
  }
  if (name == "/apikey") {
    if (role.empty()) {
      show("Usage: /apikey ROLE");
      return;
    }
    auto& slot = find_slot(config, role);
    if (slot.provider != "openai" && slot.provider != "anthropic") {
      show("Select an OpenAI or Anthropic provider before setting an API key.");
      return;
    }
    auto key = read_api_key(role);
    if (!key) {
      show("API key entry cancelled.");
      return;
    }
    slot.api_key = std::move(*key);
    save_config(config, config_file);
    show(slot.api_key.empty() ? "Saved API key cleared for " + role + "."
                              : "Saved API key for " + role + ".");
    return;
  }
  if (name == "/endpoint" || name == "/keyenv" || name == "/gguf") {
    std::string value;
    command >> value;
    if (role.empty() || value.empty()) {
      show("Usage: " + name + " ROLE VALUE");
      return;
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
    show("Saved " + name + " for " + role + ".");
    return;
  }
  if (name == "/model") {
    std::string provider;
    std::string model;
    command >> provider >> model;
    if (role.empty() || provider.empty() || model.empty()) {
      show("Usage: /model ROLE PROVIDER MODEL");
      return;
    }
    if (provider != "ollama" && provider != "openai" &&
        provider != "anthropic" && provider != "gguf") {
      show("Provider must be ollama, openai, anthropic, or gguf.");
      return;
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
    show("Saved " + role + " model assignment.");
    return;
  }
  show("Unknown command. Type /help.");
}

}  // namespace

// Worker thread -> UI thread communication. Only the UI thread touches
// ncurses; the worker only appends strings under the mutex.
struct UiChannel {
  std::mutex mutex;
  std::deque<std::string> messages;   // status and progress lines
  std::deque<std::string> tokens;     // streamed text chunks
  bool review_pending = false;
  std::string review_text;
  std::promise<bool>* review_promise = nullptr;
  bool done = false;
};

void push_message(UiChannel& channel, const std::string& line) {
  std::lock_guard<std::mutex> guard(channel.mutex);
  channel.messages.push_back(line);
}

void push_token(UiChannel& channel, const std::string& text) {
  std::lock_guard<std::mutex> guard(channel.mutex);
  channel.tokens.push_back(text);
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
  const bool outcome = answered.get();  // wait for the user's decision
  {
    std::lock_guard<std::mutex> guard(channel.mutex);
    channel.review_pending = false;
    channel.review_promise = nullptr;
    channel.review_text.clear();
  }
  return outcome;
}

int run_tui(Config& config, const std::filesystem::path& config_file) {
  if (initscr() == nullptr) throw std::runtime_error("Could not initialize terminal UI");
  mtrxai::signals::set_curses_active(true);
  ScreenGuard screen_guard;
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  intrflush(stdscr, FALSE);
  set_escdelay(25);
  curs_set(1);
  if (has_colors()) {
    start_color();
    use_default_colors();
    short accent = COLOR_GREEN;
    short accent_soft = COLOR_GREEN;
    short muted = COLOR_CYAN;
    if (can_change_color() && COLORS > 10) {
      init_color(8, 150, 670, 27);
      init_color(9, 90, 430, 18);
      init_color(10, 500, 540, 500);
      accent = 8;
      accent_soft = 9;
      muted = 10;
    }
    init_pair(kAccent, accent, -1);
    init_pair(kAccentSoft, accent_soft, -1);
    init_pair(kMuted, muted, -1);
    init_pair(kPanel, muted, -1);
  }

  screen_config = &config;
  Pipeline pipeline(config);
  UiChannel channel;
  std::thread worker;
  bool running = false;
  std::deque<std::string> transcript;
  std::string live;

  // Merge whatever the worker sent since the last redraw.
  const auto pump = [&]() {
    bool changed = false;
    std::lock_guard<std::mutex> guard(channel.mutex);
    while (!channel.messages.empty()) {
      if (!live.empty()) {
        transcript.push_back(live);
        live.clear();
      }
      transcript.push_back(channel.messages.front());
      channel.messages.pop_front();
      changed = true;
    }
    while (!channel.tokens.empty()) {
      live += channel.tokens.front();
      channel.tokens.pop_front();
      changed = true;
    }
    while (transcript.size() > 500) transcript.pop_front();
    return changed;
  };
  const auto redraw = [&]() {
    std::string text;
    for (const auto& line : transcript) text += line + "\n";
    text += live;
    if (live.empty()) text += "\n";
    draw_content(text, "mtrxAI / working");
    bool finished = false;
    {
      std::lock_guard<std::mutex> guard(channel.mutex);
      finished = channel.done;
    }
    attron(A_DIM | COLOR_PAIR(kMuted));
    mvaddnstr(LINES - 1, content_left() + 2,
              finished ? "done — press Enter to start another task"
                       : "working — Esc cancels the current job",
              COLS - 4);
    attroff(A_DIM | COLOR_PAIR(kMuted));
    refresh();
  };

  while (true) {
    if (!running) {
      std::string welcome =
          "Your coding team is ready.\n\n"
          "Describe what you want to build. The Chief of Staff frames the task, "
          "the Prompter creates a detailed implementation brief, and the Coder "
          "turns it into reviewed changes.\n\n"
          "Roles run sequentially to reduce memory use. Handoffs and progress "
          "are saved to disk as the work proceeds.\n\n"
          "Type a task below, or enter /help to see commands.";
      if (config.coder.provider == "gguf" && config.coder.gguf_path.empty()) {
        welcome += "\nCoder GGUF path is unset. Use /gguf coder /path/to/your/model.gguf "
                   "to configure your local model.";
      }
      show(welcome);
      const std::string input = input_line();
      if (input.empty()) continue;
      if (input == "/quit") break;
      if (input[0] == '/') {
        try {
          handle_command(config, config_file, input);
        } catch (const std::exception& error) {
          show(std::string("Error: ") + error.what());
        }
        getch();
        continue;
      }
      // Start the pipeline on a worker thread so the UI stays responsive.
      running = true;
      transcript.clear();
      live.clear();
      transcript.push_back("Working on: " + input);
      worker = std::thread(
          [&pipeline, &channel, input]() {
            try {
              pipeline.run(
                  input,
                  [&channel](const std::string& status_text) {
                    push_message(channel, status_text);
                  },
                  [&channel](const std::string& review_text) {
                    return ask_review(channel, review_text);
                  },
                  [&channel](const std::string& token) {
                    push_token(channel, token);
                  });
              push_message(channel, "Finished.");
            } catch (const Cancelled&) {
              push_message(channel,
                           "Cancelled. Completed stage outputs remain saved on disk.");
            } catch (const std::exception& error) {
              push_message(channel, std::string("Pipeline stopped: ") +
                                        error.what() +
                                        "\nCompleted stage outputs remain saved on disk.");
            }
            std::lock_guard<std::mutex> guard(channel.mutex);
            channel.done = true;
          });
      timeout(100);  // pump roughly ten times a second
    }

    // Running mode: pump worker output, serve approvals, and watch for Esc.
    timeout(100);  // pump roughly ten times a second
    const bool changed = pump();
    std::promise<bool>* pending = nullptr;
    {
      std::lock_guard<std::mutex> guard(channel.mutex);
      if (channel.review_pending && channel.review_promise != nullptr) {
        pending = channel.review_promise;
        channel.review_pending = false;  // only one review at a time
      }
    }
    if (pending != nullptr) {
      const std::string review_text = [&]() {
        std::lock_guard<std::mutex> guard(channel.mutex);
        return channel.review_text;
      }();
      const bool approved = review(review_text);
      pending->set_value(approved);
      continue;
    }
    const int key = getch();
    if (key == ERR) {
      if (changed) redraw();
      continue;
    }
    bool worker_done = false;
    {
      std::lock_guard<std::mutex> guard(channel.mutex);
      worker_done = channel.done;
    }
    if (key == 27 && !worker_done) {
      // Esc: stop the run cleanly; the current model request aborts and the
      // pipeline throws Cancelled at the next checkpoint.
      pipeline.request_cancel();
      push_message(channel, "Esc: cancelling after the current request...");
      redraw();
      continue;
    }
    if (worker_done && (key == '\n' || key == '\r' || key == KEY_ENTER)) {
      worker.join();
      worker = std::thread();
      running = false;
      continue;
    }
    if (changed) redraw();
  }

  if (worker.joinable()) {
    // Only reachable when quitting mid-run; ask for a clean stop first.
    pipeline.request_cancel();
    worker.join();
  }
  return 0;
}

}  // namespace mtrxai
