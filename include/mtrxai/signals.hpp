#pragma once

#include <sys/types.h>
#include <string>

namespace mtrxai {
namespace signals {

void install();
void set_curses_active(bool active);
void set_active_child(pid_t pid);
void set_active_ollama(const std::string& base_url, const std::string& model);
void clear_active_ollama();
void trigger_signal(int signum);

}  // namespace signals
}  // namespace mtrxai
