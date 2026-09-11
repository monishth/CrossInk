#include "BookOrbitUrl.h"

namespace bookorbit {
namespace {

constexpr char kApiSuffix[] = "/api/v1";
constexpr char kLegacySuffix[] = "/api/v1/koreader";

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string normalizeServerUrl(const std::string_view input) {
  std::string_view url = trim(input);
  if (url.empty()) return {};

  while (!url.empty() && url.back() == '/') {
    url.remove_suffix(1);
  }
  if (url.empty()) return {};

  if (endsWith(url, kLegacySuffix)) {
    url.remove_suffix(sizeof(kLegacySuffix) - 1 - (sizeof(kApiSuffix) - 1));
    return std::string(url);
  }
  if (endsWith(url, kApiSuffix)) {
    return std::string(url);
  }
  return std::string(url) + kApiSuffix;
}

std::string joinPath(const std::string_view base, const std::string_view path) {
  std::string joined(base);
  if (!path.empty() && path.front() != '/') {
    joined += '/';
  }
  joined.append(path);
  return joined;
}

}  // namespace bookorbit
