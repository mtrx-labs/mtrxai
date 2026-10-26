#include "mtrxai/endpoint.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <stdexcept>

namespace mtrxai {
namespace {
struct UrlDeleter {
  void operator()(CURLU* url) const { curl_url_cleanup(url); }
};
using Url = std::unique_ptr<CURLU, UrlDeleter>;

std::string part(CURLU* url, CURLUPart which) {
  char* value = nullptr;
  if (curl_url_get(url, which, &value, 0) != CURLUE_OK) return {};
  std::string result(value);
  curl_free(value);
  return result;
}

bool loopback(const std::string& host) {
  std::string normalized = host;
  if (normalized.size() > 2 && normalized.front() == '[' &&
      normalized.back() == ']') normalized = normalized.substr(1, normalized.size() - 2);
  if (!normalized.empty() && normalized.back() == '.') normalized.pop_back();
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized == "localhost") return true;
  in_addr ipv4{};
  if (inet_pton(AF_INET, normalized.c_str(), &ipv4) == 1)
    return (ntohl(ipv4.s_addr) >> 24) == 127;
  in6_addr ipv6{};
  return inet_pton(AF_INET6, normalized.c_str(), &ipv6) == 1 &&
         IN6_IS_ADDR_LOOPBACK(&ipv6);
}
}  // namespace

void validate_provider_endpoint(const std::string& value) {
  Url url(curl_url());
  if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(),
                           CURLU_DISALLOW_USER) != CURLUE_OK) {
    throw std::runtime_error("Invalid provider endpoint URL. Use a valid HTTPS URL, or HTTP only for a loopback service.");
  }
  const std::string scheme = part(url.get(), CURLUPART_SCHEME);
  const std::string host = part(url.get(), CURLUPART_HOST);
  const std::size_t separator = value.find("://");
  if (separator == std::string::npos || separator != scheme.size() ||
      separator + 3 >= value.size() || value[separator + 3] == '/') {
    throw std::runtime_error("Invalid provider endpoint URL. Use a valid HTTPS URL, or HTTP only for a loopback service.");
  }
  if (host.empty() || (!part(url.get(), CURLUPART_USER).empty()) ||
      (!part(url.get(), CURLUPART_PASSWORD).empty())) {
    throw std::runtime_error("Provider endpoint must include a host and must not contain a username or password.");
  }
  if (scheme == "https") return;
  if (scheme == "http" && loopback(host)) return;
  if (scheme == "http") {
    throw std::runtime_error("Remote HTTP provider endpoints are blocked because prompts and credentials would be sent in plaintext. Use HTTPS or a loopback URL such as http://127.0.0.1.");
  }
  throw std::runtime_error("Unsupported provider endpoint scheme. Use HTTPS, or HTTP only for a loopback service.");
}
}  // namespace mtrxai
