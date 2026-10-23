#include "mtrxai/tui.hpp"

#include "mtrxai/pipeline.hpp"

#include <ncurses.h>

#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace mtrxai {
namespace {

void show(const std::string& text) {
  erase();
  mvprintw(0, 0, "mtrxAI | sequential coding agents | Ctrl-D to quit");
  mvhline(1, 0, ACS_HLINE, COLS);
  std::istringstream lines(text);
  std::string line;
  int row = 2;
  while (row < LINES - 1 && std::getline(lines, line)) {
    mvaddnstr(row++, 0, line.c_str(), COLS - 1);
  }
  refresh();
}

void show_roles(const Config& config) {
  std::ostringstream text;
  for (const auto* name : {"chief", "prompter", "coder"}) {
    const std::string role(name);
    const auto& slot = find_slot(config, role);
    text << slot.name << ": " << slot.provider << " / "
         << (slot.model.empty() ? slot.gguf_path : slot.model) << '\n';
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

  Pipeline pipeline(config);
  char buffer[8192];
  while (true) {
    show("Enter a task, or type /help. Completed stage outputs are saved before "
         "the next model starts.");
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
          [](const std::string& proposal) {
            show(proposal);
            mvprintw(LINES - 1, 0, "Apply generated files? [y/N] ");
            clrtoeol();
            refresh();
            char answer[16]{};
            if (getnstr(answer, 15) == ERR) return false;
            return answer[0] == 'y' || answer[0] == 'Y';
          });
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
