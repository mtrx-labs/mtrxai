#include "mtrxai/update.hpp"

#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef MTRXAI_VERSION
#define MTRXAI_VERSION "0.0.0"
#endif

namespace mtrxai {
namespace {

constexpr char kReleaseUrl[] =
    "https://api.github.com/repos/mtrx-labs/mtrxai/releases/latest";
constexpr std::size_t kMetadataLimit = 2 * 1024 * 1024;
constexpr std::uint64_t kBinaryLimit = 256ULL * 1024 * 1024;

struct JsonDeleter {
  void operator()(json_object* value) const {
    if (value != nullptr) json_object_put(value);
  }
};
using Json = std::unique_ptr<json_object, JsonDeleter>;

struct Buffer {
  std::string value;
  std::size_t limit = kMetadataLimit;
  long status = 0;
};

std::size_t receive_buffer(char* data, std::size_t size, std::size_t count,
                           void* userdata) {
  const std::size_t bytes = size * count;
  auto& buffer = *static_cast<Buffer*>(userdata);
  if (bytes > buffer.limit - buffer.value.size()) return 0;
  buffer.value.append(data, bytes);
  return bytes;
}

struct Download {
  int fd = -1;
  EVP_MD_CTX* digest = nullptr;
  std::uint64_t bytes = 0;
  std::string error;
};

void restrict_to_https(CURL* curl) {
#if LIBCURL_VERSION_NUM >= 0x075500
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
}

std::size_t write_download(char* data, std::size_t size, std::size_t count,
                           void* userdata) {
  auto& download = *static_cast<Download*>(userdata);
  const std::size_t bytes = size * count;
  if (bytes > kBinaryLimit - download.bytes) {
    download.error = "Release binary exceeds the 256 MiB safety limit";
    return 0;
  }
  std::size_t written = 0;
  while (written < bytes) {
    const ssize_t result =
        write(download.fd, data + written, bytes - written);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) {
      download.error = "Could not write downloaded update: " +
                       std::string(std::strerror(errno));
      return 0;
    }
    written += static_cast<std::size_t>(result);
  }
  if (EVP_DigestUpdate(download.digest, data, bytes) != 1) {
    download.error = "Could not calculate update checksum";
    return 0;
  }
  download.bytes += bytes;
  return bytes;
}

Buffer get_https(const std::string& url, bool allow_not_found = false) {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("Could not create update request");
  Buffer response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "mtrxAI-updater");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  restrict_to_https(curl);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_buffer);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("Update check failed: ") +
                             curl_easy_strerror(result));
  }
  response.status = status;
  if ((status < 200 || status >= 300) &&
      !(allow_not_found && status == 404)) {
    throw std::runtime_error("Update service returned HTTP " +
                             std::to_string(status));
  }
  return response;
}

std::string json_string(json_object* object, const char* key) {
  json_object* value = nullptr;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string)) {
    return {};
  }
  return json_object_get_string(value);
}

std::optional<std::array<unsigned int, 3>> parse_version(std::string_view text) {
  if (text.size() > 0 && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  std::array<unsigned int, 3> version{};
  for (std::size_t i = 0; i < version.size(); ++i) {
    const auto separator = text.find('.');
    const auto part = text.substr(0, separator);
    if (part.empty() || part.size() > 9 ||
        !std::all_of(part.begin(), part.end(), [](unsigned char c) {
          return std::isdigit(c) != 0;
        })) {
      return std::nullopt;
    }
    try {
      version[i] = static_cast<unsigned int>(std::stoul(std::string(part)));
    } catch (const std::exception&) {
      return std::nullopt;
    }
    if (i < 2) {
      if (separator == std::string_view::npos) return std::nullopt;
      text.remove_prefix(separator + 1);
    } else if (separator != std::string_view::npos) {
      return std::nullopt;
    }
  }
  return version;
}

std::filesystem::path user_update_path() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') {
    throw std::runtime_error("HOME is not set; cannot install a user update");
  }
  return std::filesystem::path(home) / ".local" / "bin" / "mtrxai";
}

std::string platform_asset_name() {
  utsname platform{};
  if (uname(&platform) != 0) {
    throw std::runtime_error("Could not determine system architecture");
  }
  const std::string machine(platform.machine);
  if (machine == "x86_64") return "mtrxai-linux-x86_64";
  if (machine == "aarch64" || machine == "arm64") {
    return "mtrxai-linux-aarch64";
  }
  throw std::runtime_error("Automatic updates are not available for " + machine);
}

std::filesystem::path current_executable() {
  std::array<char, 4096> path{};
  const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length < 0) {
    throw std::runtime_error("Could not locate the running mtrxAI executable");
  }
  path[static_cast<std::size_t>(length)] = '\0';
  return std::filesystem::path(path.data());
}

std::filesystem::path installation_path() {
  const auto running = current_executable();
  if (access(running.c_str(), W_OK) == 0) return running;
  return user_update_path();
}

std::filesystem::path temporary_file_near(const std::filesystem::path& target,
                                          int& fd) {
  std::error_code error;
  std::filesystem::create_directories(target.parent_path(), error);
  if (error) {
    throw std::runtime_error("Could not create update directory: " +
                             error.message());
  }
  const std::string pattern = (target.parent_path() / ".mtrxai-update-XXXXXX").string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  fd = mkstemp(buffer.data());
  if (fd < 0) {
    throw std::runtime_error("Could not create temporary update file: " +
                             std::string(std::strerror(errno)));
  }
  if (fchmod(fd, S_IRUSR | S_IWUSR) < 0) {
    const int error = errno;
    close(fd);
    unlink(buffer.data());
    throw std::runtime_error("Could not protect temporary update file: " +
                             std::string(std::strerror(error)));
  }
  return buffer.data();
}

std::string digest_hex(const unsigned char* digest, unsigned int length) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(length * 2);
  for (unsigned int i = 0; i < length; ++i) {
    result.push_back(hex[digest[i] >> 4]);
    result.push_back(hex[digest[i] & 0x0f]);
  }
  return result;
}

void install_asset(const std::string& url, const std::string& expected_digest,
                   const std::filesystem::path& target) {
  constexpr std::string_view prefix = "sha256:";
  if (expected_digest.rfind(prefix, 0) != 0 ||
      expected_digest.size() != prefix.size() + 64) {
    throw std::runtime_error(
        "Release asset has no valid SHA-256 digest; refusing to install it");
  }

  int fd = -1;
  const auto temporary = temporary_file_near(target, fd);
  EVP_MD_CTX* digest = EVP_MD_CTX_new();
  if (digest == nullptr || EVP_DigestInit_ex(digest, EVP_sha256(), nullptr) != 1) {
    if (digest != nullptr) EVP_MD_CTX_free(digest);
    close(fd);
    unlink(temporary.c_str());
    throw std::runtime_error("Could not initialize update checksum");
  }
  Download download{fd, digest, 0, {}};
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    EVP_MD_CTX_free(digest);
    close(fd);
    unlink(temporary.c_str());
    throw std::runtime_error("Could not create update download");
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "mtrxAI-updater");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  restrict_to_https(curl);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  if (result == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  unsigned char digest_bytes[EVP_MAX_MD_SIZE];
  unsigned int digest_length = 0;
  const bool finalized =
      EVP_DigestFinal_ex(digest, digest_bytes, &digest_length) == 1;
  EVP_MD_CTX_free(digest);
  const int sync_result = fsync(fd);
  const int sync_error = errno;
  const int close_result = close(fd);
  if (result != CURLE_OK || status < 200 || status >= 300 ||
      !download.error.empty() || !finalized || sync_result < 0 ||
      close_result < 0) {
    unlink(temporary.c_str());
    if (!download.error.empty()) throw std::runtime_error(download.error);
    if (result != CURLE_OK) {
      throw std::runtime_error(std::string("Update download failed: ") +
                               curl_easy_strerror(result));
    }
    if (sync_result < 0) {
      throw std::runtime_error("Could not flush downloaded update: " +
                               std::string(std::strerror(sync_error)));
    }
    throw std::runtime_error("Update download was incomplete or could not be saved");
  }
  const std::string actual_digest =
      "sha256:" + digest_hex(digest_bytes, digest_length);
  if (actual_digest != expected_digest) {
    unlink(temporary.c_str());
    throw std::runtime_error("Update checksum mismatch; refusing to install it");
  }
  if (download.bytes == 0 || chmod(temporary.c_str(), S_IRUSR | S_IWUSR |
                                                        S_IXUSR | S_IRGRP |
                                                        S_IXGRP | S_IROTH |
                                                        S_IXOTH) < 0) {
    unlink(temporary.c_str());
    throw std::runtime_error("Downloaded update is empty or could not be made executable");
  }
  std::error_code error;
  std::filesystem::rename(temporary, target, error);
  if (error) {
    unlink(temporary.c_str());
    throw std::runtime_error("Could not install update at " + target.string() +
                             ": " + error.message());
  }
}

void restart_updated(int argc, char** argv, const std::filesystem::path& target) {
  std::vector<char*> arguments;
  arguments.reserve(static_cast<std::size_t>(argc) + 1);
  arguments.push_back(const_cast<char*>(target.c_str()));
  for (int i = 1; i < argc; ++i) arguments.push_back(argv[i]);
  arguments.push_back(nullptr);
  execv(target.c_str(), arguments.data());
  throw std::runtime_error("Update installed, but restart failed: " +
                           std::string(std::strerror(errno)));
}

struct Release {
  std::string version;
  std::string asset_url;
  std::string digest;
};

std::optional<Release> latest_release() {
  const auto response = get_https(kReleaseUrl, true);
  if (response.status == 404) return std::nullopt;
  Json root(json_tokener_parse(response.value.c_str()));
  if (!root || !json_object_is_type(root.get(), json_type_object)) {
    throw std::runtime_error("Update service returned invalid release metadata");
  }
  if (json_object_get_boolean(json_object_object_get(root.get(), "draft")) ||
      json_object_get_boolean(json_object_object_get(root.get(), "prerelease"))) {
    return std::nullopt;
  }
  const std::string tag = json_string(root.get(), "tag_name");
  const auto latest = parse_version(tag);
  const auto current = parse_version(MTRXAI_VERSION);
  if (!latest || !current || *latest <= *current) return std::nullopt;

  json_object* assets = nullptr;
  if (!json_object_object_get_ex(root.get(), "assets", &assets) ||
      !json_object_is_type(assets, json_type_array)) {
    throw std::runtime_error("Release metadata has no assets list");
  }
  for (std::size_t i = 0; i < json_object_array_length(assets); ++i) {
    json_object* asset = json_object_array_get_idx(assets, i);
    if (asset == nullptr) continue;
    if (json_string(asset, "name") == platform_asset_name()) {
      const auto url = json_string(asset, "browser_download_url");
      const auto digest = json_string(asset, "digest");
      if (url.rfind(
              "https://github.com/mtrx-labs/mtrxai/releases/download/", 0) !=
          0) {
        throw std::runtime_error("Release binary URL is not a trusted HTTPS GitHub URL");
      }
      return Release{tag, url, digest};
    }
  }
  throw std::runtime_error("A newer release has no binary for this architecture");
}

}  // namespace

void check_and_prompt_for_update(int argc, char** argv) {
  try {
    const auto release = latest_release();
    if (!release) return;
    std::cout << "\nmtrxAI " << release->version
              << " is available (you have " << MTRXAI_VERSION
              << "). Update and restart now? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) ||
        (answer != "y" && answer != "Y" && answer != "yes" &&
         answer != "YES")) {
      std::cout << "Continuing with the current version.\n";
      return;
    }

    const auto target = installation_path();
    std::cout << "Downloading and verifying the release...\n";
    install_asset(release->asset_url, release->digest, target);
    std::cout << "Update installed. Restarting mtrxAI...\n" << std::flush;
    restart_updated(argc, argv, target);
  } catch (const std::exception& error) {
    std::cerr << "mtrxAI update: " << error.what()
              << "\nContinuing with the current version.\n";
  }
}

}  // namespace mtrxai
