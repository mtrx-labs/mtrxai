#include "mtrxai/tui.hpp"

#include "mtrxai/pipeline.hpp"

#include <ncurses.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mtrxai {
namespace {

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

void show(const std::string& text) {
  erase();
  attron(A_BOLD | (has_colors() ? COLOR_PAIR(2) : 0));
  mvprintw(0, 0, " mtrxAI  /  SEQUENTIAL CODING AGENTS");
  attroff(A_BOLD | (has_colors() ? COLOR_PAIR(2) : 0));
  mvhline(1, 0, ACS_HLINE, COLS);
  std::istringstream lines(safe_terminal_text(text));
  std::string line;
  int row = 2;
  while (row < LINES - 2 && std::getline(lines, line)) {
    mvaddnstr(row++, 1, line.c_str(), COLS - 2);
  }
  mvhline(LINES - 2, 0, ACS_HLINE, COLS);
  mvaddnstr(LINES - 1, 1, "mtrxAI  |  /help for commands", COLS - 2);
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

bool review(const std::string& text) {
  const auto lines = wrap_lines(text, std::max(1, COLS - 2));
  std::size_t offset = 0;
  const std::size_t page = static_cast<std::size_t>(std::max(1, LINES - 4));
  while (true) {
    erase();
    attron(A_BOLD | (has_colors() ? COLOR_PAIR(2) : 0));
    mvaddnstr(0, 1, " REVIEW REQUEST", COLS - 2);
    attroff(A_BOLD | (has_colors() ? COLOR_PAIR(2) : 0));
    mvhline(1, 0, ACS_HLINE, COLS);
    for (std::size_t row = 0; row < page && offset + row < lines.size(); ++row) {
      mvaddnstr(static_cast<int>(row) + 2, 1, lines[offset + row].c_str(),
                COLS - 2);
    }
    mvhline(LINES - 2, 0, ACS_HLINE, COLS);
    const std::string footer =
        " ↑/↓ or PgUp/PgDn to review  |  y approve  |  n decline";
    attron(has_colors() ? COLOR_PAIR(1) : 0);
    mvaddnstr(LINES - 1, 1, footer.c_str(), COLS - 2);
    attroff(has_colors() ? COLOR_PAIR(1) : 0);
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
       "  /model ROLE PROVIDER MODEL      hot-swap a role (ollama, openai, gguf)\n"
       "  /endpoint ROLE URL              set an OpenAI-compatible API base URL\n"
       "  /keyenv ROLE ENV_NAME           select an API-key environment variable\n"
       "  /gguf ROLE PATH                 set a local GGUF file path\n"
       "Use Up/Down or PgUp/PgDn to review every proposed file or shell command.\n"
       "  /help                           show this help\n"
       "  /quit                           exit");
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
    if (provider != "ollama" && provider != "openai" && provider != "gguf") {
      show("Provider must be ollama, openai, or gguf.");
      return;
    }
    auto& slot = find_slot(config, role);
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
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  curs_set(1);
  if (has_colors()) {
    start_color();
    use_default_colors();
    init_pair(1, COLOR_GREEN, -1);
    init_pair(2, COLOR_CYAN, -1);
  }

  Pipeline pipeline(config);
  char buffer[8192];
  while (true) {
    std::string welcome =
        "Enter a task, or type /help. Completed stage outputs are saved before "
        "the next model starts. The Coder may read files, request approved edits, "
        "and request approved shell commands.";
    if (config.coder.provider == "gguf" && config.coder.gguf_path.empty()) {
      welcome += "\nCoder GGUF path is unset. Use /gguf coder /path/to/your/model.gguf "
                 "to configure your local model.";
    }
    show(welcome);
    mvprintw(LINES - 1, 0, "> ");
    clrtoeol();
    refresh();
    const int result = getnstr(buffer, static_cast<int>(sizeof(buffer) - 1));
    if (result == ERR) {
      endwin();
      return 0;
    }
    const std::string input(buffer);
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

  endwin();
  return 0;
}

}  // namespace mtrxai
