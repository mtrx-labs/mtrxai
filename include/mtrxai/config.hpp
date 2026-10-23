#pragma once

#include <filesystem>
#include <string>

namespace mtrxai {

struct ModelSlot {
  std::string name;
  std::string provider;
  std::string model;
  std::string base_url;
  std::string api_key_env;
  std::string gguf_path;
  int context_size = 2048;
};

struct Config {
  ModelSlot chief;
  ModelSlot prompter;
  ModelSlot coder;
};

std::filesystem::path config_path();
Config default_config();
Config load_config(const std::filesystem::path& path);
void save_config(const Config& config, const std::filesystem::path& path);
ModelSlot& find_slot(Config& config, const std::string& name);
const ModelSlot& find_slot(const Config& config, const std::string& name);

}  // namespace mtrxai
