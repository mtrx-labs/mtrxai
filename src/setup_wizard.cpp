#include "mtrxai/config.hpp"
#include "mtrxai/endpoint.hpp"

#include "mtrxai/files.hpp"

#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace mtrxai {
namespace {

enum class ProviderChoice { gguf = 1, ollama, openai, anthropic };

class EchoGuard {
 public:
  explicit EchoGuard(bool hidden) : descriptor_(STDIN_FILENO) {
    if (!hidden || !isatty(descriptor_)) return;
    if (tcgetattr(descriptor_, &original_) != 0) {
      throw std::runtime_error("Could not read terminal settings for secret input");
    }
    termios hidden_settings = original_;
    hidden_settings.c_lflag &= static_cast<tcflag_t>(~ECHO);
    if (tcsetattr(descriptor_, TCSANOW, &hidden_settings) != 0) {
      throw std::runtime_error("Could not hide secret input");
    }
    active_ = true;
  }

  ~EchoGuard() {
    if (active_) tcsetattr(descriptor_, TCSANOW, &original_);
  }

  EchoGuard(const EchoGuard&) = delete;
  EchoGuard& operator=(const EchoGuard&) = delete;

 private:
  int descriptor_;
  termios original_{};
  bool active_ = false;
};

std::string trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
    return std::isspace(c);
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
    return std::isspace(c);
  }).base();
  if (first >= last) return {};
  return std::string(first, last);
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string prompt_line(const std::string& label, const std::string& fallback = {}) {
  while (true) {
    std::cout << label;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": " << std::flush;
    std::string value;
    if (!std::getline(std::cin, value)) {
      throw std::runtime_error("Setup cancelled before configuration was saved");
    }
    value = trim(std::move(value));
    if (value.empty() && !fallback.empty()) return fallback;
    if (!value.empty()) return value;
    std::cout << "A value is required.\n";
  }
}

std::string prompt_secret(const std::string& label) {
  while (true) {
    std::string value;
    {
      EchoGuard guard(true);
      std::cout << label << ": " << std::flush;
      if (!std::getline(std::cin, value)) {
        std::cout << '\n';
        throw std::runtime_error("Setup cancelled before configuration was saved");
      }
    }
    std::cout << '\n';
    if (!value.empty()) return value;
    std::cout << "An API key is required.\n";
  }
}

std::string prompt_url(const std::string& label, const std::string& fallback) {
  while (true) {
    auto value = prompt_line(label, fallback);
    try {
      validate_provider_endpoint(value);
      return value;
    } catch (const std::exception& error) {
      std::cout << error.what() << '\n';
    }
  }
}

ProviderChoice prompt_provider(const std::string& role) {
  std::cout << "\n" << role << "\n"
            << "  1. GGUF       Local model file via llama.cpp\n"
            << "  2. Ollama     Local Ollama model\n"
            << "  3. OpenAI     OpenAI API\n"
            << "  4. Anthropic  Anthropic Messages API\n";
  while (true) {
    const auto choice = lower(prompt_line("Choose a provider (1-4 or name)"));
    if (choice == "1" || choice == "gguf") return ProviderChoice::gguf;
    if (choice == "2" || choice == "ollama") return ProviderChoice::ollama;
    if (choice == "3" || choice == "openai") return ProviderChoice::openai;
    if (choice == "4" || choice == "anthropic") return ProviderChoice::anthropic;
    std::cout << "Choose GGUF, Ollama, OpenAI, or Anthropic.\n";
  }
}

std::string expand_home(const std::string& value) {
  if (value == "~") {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
      throw std::runtime_error("HOME is not set; cannot expand model path");
    }
    return home;
  }
  if (value.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
      throw std::runtime_error("HOME is not set; cannot expand model path");
    }
    return (std::filesystem::path(home) / value.substr(2)).string();
  }
  return value;
}

ModelSlot configure_role(const std::string& name, const std::string& title,
                         int context_size) {
  ModelSlot slot;
  slot.name = name;
  slot.context_size = context_size;
  switch (prompt_provider(title)) {
    case ProviderChoice::gguf: {
      slot.provider = "gguf";
      slot.base_url = "http://127.0.0.1";
      while (true) {
        slot.gguf_path = expand_home(prompt_line("Path to GGUF model"));
        const auto path = std::filesystem::path(slot.gguf_path);
        if (std::filesystem::is_regular_file(path) &&
            access(path.c_str(), R_OK) == 0) {
          break;
        }
        std::cout << "GGUF model file is not readable: " << slot.gguf_path
                  << "\n";
      }
      break;
    }
    case ProviderChoice::ollama:
      slot.provider = "ollama";
      slot.base_url = prompt_url("Ollama server URL", "http://localhost:11434");
      slot.model = prompt_line("Ollama model name (include its tag)");
      break;
    case ProviderChoice::openai:
      slot.provider = "openai";
      slot.base_url = prompt_url("OpenAI-compatible API base URL",
                                 "https://api.openai.com/v1");
      slot.model = prompt_line("OpenAI model name");
      slot.api_key = prompt_secret("OpenAI API key (input is hidden)");
      break;
    case ProviderChoice::anthropic:
      slot.provider = "anthropic";
      slot.base_url =
          prompt_url("Anthropic API base URL", "https://api.anthropic.com");
      slot.model = prompt_line("Anthropic model name");
      slot.api_key = prompt_secret("Anthropic API key (input is hidden)");
      break;
  }
  return slot;
}

}  // namespace

void run_setup_wizard(const std::filesystem::path& path) {
  if (!isatty(STDIN_FILENO)) {
    throw std::runtime_error(
        "The setup wizard needs an interactive terminal. Run `mtrxai --init` "
        "from a terminal.");
  }

  std::cout << "mtrxAI setup\n"
            << "Configure one provider for each role. Local roles run one at a "
               "time to conserve memory.\n"
            << "API keys are entered without echo and saved separately with "
               "owner-only permissions.\n"
            << "Press Ctrl-D at any prompt to cancel without saving.\n";

  Config config;
  config.chief =
      configure_role("chief", "1 / 3 — Chief of Staff", 4096);
  config.prompter =
      configure_role("prompter", "2 / 3 — Prompter", 4096);
  config.coder = configure_role("coder", "3 / 3 — Coder", 2048);

  save_config(config, path);
  std::cout << "\nSetup complete.\nConfig file: " << path
            << '\n';
  if (!config.chief.api_key.empty() || !config.prompter.api_key.empty() ||
      !config.coder.api_key.empty()) {
    std::cout << "Credentials file: "
              << path.parent_path() / "credentials.json" << '\n';
  }
  std::cout << "Run `mtrxai` to start your session.\n";
}

}  // namespace mtrxai
