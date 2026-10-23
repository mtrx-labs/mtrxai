#include "mtrxai/tui.hpp"

#include "mtrxai/pipeline.hpp"

#include <ncurses.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
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
  ~ScreenGuard() { endwin(); }
};

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
  for (int row = 0; row < content_height &&
                     static_cast<std::size_t>(row) < lines.size();
       ++row) {
    mvaddnstr(content_top + row + 1, left + 2,
              lines[static_cast<std::size_t>(row)].c_str(),
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

    const int key = getch();
    if (key == KEY_RESIZE) continue;
    if (key == ERR) return {};
    if (key == '\n' || key == '\r' || key == KEY_ENTER) return value;
    if (key == 27) return {};
    if (key == KEY_LEFT) {
      if (cursor > 0) --cursor;
    } else if (key == KEY_RIGHT) {
      if (cursor < value.size()) ++cursor;
    } else if (key == KEY_HOME || key == 1) {
      cursor = 0;
    } else if (key == KEY_END || key == 5) {
      cursor = value.size();
    } else if (key == KEY_BACKSPACE || key == 127 || key == 8) {
      if (cursor > 0) {
        value.erase(cursor - 1, 1);
        --cursor;
      }
    } else if (key == KEY_DC) {
      if (cursor < value.size()) value.erase(cursor, 1);
    } else if (key >= 0 && key <= 255 &&
               std::isprint(static_cast<unsigned char>(key)) &&
               value.size() < 8191) {
      value.insert(cursor, 1, static_cast<char>(key));
      ++cursor;
    }
  }
}

bool review(const std::string& text) {
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
    refresh();

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
         << model << '\n';
  }
  show(text.str());
}

void command_help() {
  show("Commands:\n"
       "  /roles                         show the active model assignments\n"
       "  /model ROLE PROVIDER MODEL      hot-swap a role (ollama, openai, anthropic, gguf)\n"
       "  /endpoint ROLE URL              set an API base URL\n"
       "  /keyenv ROLE ENV_NAME           select an API-key environment variable\n"
       "  /apikey ROLE                    enter or clear a saved API key (hidden input)\n"
       "  /gguf ROLE PATH                 set a local GGUF file path\n"
       "Use Up/Down or PgUp/PgDn to review every proposed file or shell command.\n"
       "  /help                           show this help\n"
       "  /quit                           exit");
}

std::optional<std::string> read_api_key(const std::string& role) {
  show("API key for " + role +
       " (input is hidden; Enter saves, Esc cancels).");
  std::string key;
  while (true) {
    const int character = getch();
    if (character == ERR || character == 27) return std::nullopt;
    if (character == '\n' || character == KEY_ENTER) return key;
    if (character == KEY_BACKSPACE || character == 127 || character == 8) {
      if (!key.empty()) key.pop_back();
      show("API key for " + role + "\n" + std::string(key.size(), '*'));
      continue;
    }
    if (character >= 0 && character <= 255 &&
        std::isprint(static_cast<unsigned char>(character)) &&
        key.size() < 4096) {
      key.push_back(static_cast<char>(character));
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
  std::string role;
  command >> role;
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

int run_tui(Config& config, const std::filesystem::path& config_file) {
  if (initscr() == nullptr) throw std::runtime_error("Could not initialize terminal UI");
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
  while (true) {
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
    try {
      pipeline.run(
          input, [](const std::string& status) { show(status); },
          [](const std::string& proposal) { return review(proposal); });
      show("Pipeline complete. Press any key to enter another task.");
    } catch (const std::exception& error) {
      show(std::string("Pipeline stopped: ") + error.what() +
           "\nCompleted stage outputs remain saved on disk.");
    }
    getch();
  }

  return 0;
}

}  // namespace mtrxai
