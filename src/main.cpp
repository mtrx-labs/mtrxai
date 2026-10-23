#include "mtrxai/config.hpp"
#include "mtrxai/tui.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>

namespace {

void usage(const char* program) {
  std::cout << "Usage: " << program << " [--init | --help | --version]\n"
            << "Run mtrxAI from the project directory to start the TUI.\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 1) {
      const std::string argument = argv[1];
      if (argument == "--help" || argument == "-h") {
        usage(argv[0]);
        return 0;
      }
      if (argument == "--version") {
        std::cout << "mtrxAI 0.1.0\n";
        return 0;
      }
      if (argument == "--init") {
        const std::filesystem::path path = mtrxai::config_path();
        if (std::filesystem::exists(path)) {
          throw std::runtime_error("Configuration already exists: " +
                                   path.string());
        }
        mtrxai::save_config(mtrxai::default_config(), path);
        std::cout << "Created " << path << '\n'
                  << "Set the coder GGUF path if needed, then run mtrxai.\n";
        return 0;
      }
      usage(argv[0]);
      return 2;
    }

    if (prctl(PR_SET_DUMPABLE, 0) != 0) {
      throw std::runtime_error(
          "Could not protect process environment from child commands");
    }
    const std::filesystem::path config_file = mtrxai::config_path();
    if (!std::filesystem::exists(config_file)) {
      std::cerr << "No configuration found. Run `" << argv[0]
                << " --init` to create " << config_file << ".\n";
      return 1;
    }

    const CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_result != CURLE_OK) {
      throw std::runtime_error(std::string("Could not initialize HTTP client: ") +
                               curl_easy_strerror(curl_result));
    }
    auto config = mtrxai::load_config(config_file);
    const int result = mtrxai::run_tui(config, config_file);
    curl_global_cleanup();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "mtrxAI: " << error.what() << '\n';
    return 1;
  }
}
