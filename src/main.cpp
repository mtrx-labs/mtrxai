#include "mtrxai/config.hpp"
#include "mtrxai/session.hpp"
#include "mtrxai/signals.hpp"
#include "mtrxai/tui.hpp"
#include "mtrxai/update.hpp"

#include <curl/curl.h>

#include <clocale>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef MTRXAI_VERSION
#define MTRXAI_VERSION "0.0.0"
#endif

namespace {

void usage(const char* program) {
  std::cout << "Usage: " << program
            << " [--init | --resume [SESSION-ID] | --no-update-check | --help | --version]\n"
            << "Run mtrxAI from the project directory to start the TUI.\n"
            << "  --resume [SESSION-ID]  continue a saved session "
               "(the latest unfinished one by default)\n";
}

}  // namespace

int main(int argc, char** argv) {
  mtrxai::signals::install();
  // Pick up the user's locale so wide ncurses can decode UTF-8 input; without
  // this ncurses stays in the C locale and non-ASCII typing does not work.
  std::setlocale(LC_ALL, "");
  try {
    bool skip_update_check = false;
    std::optional<std::string> resume_id;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--help" || argument == "-h") {
        usage(argv[0]);
        return 0;
      }
      if (argument == "--version") {
        std::cout << "mtrxAI " << MTRXAI_VERSION << '\n';
        return 0;
      }
      if (argument == "--init") {
        const std::filesystem::path path = mtrxai::config_path();
        if (std::filesystem::exists(path)) {
          throw std::runtime_error("Configuration already exists: " +
                                   path.string());
        }
        mtrxai::run_setup_wizard(path);
        return 0;
      }
      if (argument == "--no-update-check") {
        skip_update_check = true;
        continue;
      }
      if (argument == "--resume") {
        // The id is optional. A following flag (for example
        // --no-update-check) is not an id.
        if (index + 1 < argc && argv[index + 1][0] != '-') {
          resume_id = std::string(argv[++index]);
        } else {
          resume_id = std::string();
        }
        continue;
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

    std::filesystem::path resume_session;
    if (resume_id) {
      std::optional<mtrxai::SessionInfo> session;
      if (resume_id->empty()) {
        session = mtrxai::latest_unfinished_session();
        if (!session) {
          std::cout << "All saved sessions are complete; nothing to resume.\n";
          return 0;
        }
      } else {
        session = mtrxai::find_session(*resume_id);
        if (!session) {
          std::cerr << "No saved session matches \"" << *resume_id << "\".\n";
          return 1;
        }
      }
      if (mtrxai::session_finished(*session)) {
        std::cout << "Session " << session->id << " is already complete.\n";
        return 0;
      }
      resume_session = session->path;
    }

    const CURLcode curl_result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (curl_result != CURLE_OK) {
      throw std::runtime_error(std::string("Could not initialize HTTP client: ") +
                               curl_easy_strerror(curl_result));
    }
    if (!skip_update_check && isatty(STDIN_FILENO)) {
      mtrxai::check_and_prompt_for_update(argc, argv);
    }
    auto config = mtrxai::load_config(config_file);
    const int result = mtrxai::run_tui(config, config_file, resume_session);
    curl_global_cleanup();
    return result;
  } catch (const std::exception& error) {
    std::cerr << "mtrxAI: " << error.what() << '\n';
    return 1;
  }
}
