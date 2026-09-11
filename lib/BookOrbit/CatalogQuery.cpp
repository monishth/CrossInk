#include "CatalogQuery.h"

#include <algorithm>
#include <cstdio>

namespace bookorbit {
namespace {

bool isUnreserved(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

constexpr char kHex[] = "0123456789ABCDEF";

}  // namespace

std::string urlEncodeComponent(const std::string_view value) {
  std::string encoded;
  encoded.reserve(value.size() + value.size() / 4);
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    if (isUnreserved(c)) {
      encoded.push_back(raw);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0x0F]);
    }
  }
  return encoded;
}

void CatalogQuery::set(const std::string_view key, const std::string_view value) {
  if (key.empty()) return;
  if (value.empty()) {
    clear(key);
    return;
  }
  for (auto& param : params_) {
    if (param.first == key) {
      param.second.assign(value);
      return;
    }
  }
  params_.emplace_back(std::string(key), std::string(value));
}

void CatalogQuery::set(const std::string_view key, const long value) {
  // 21 bytes holds any 64-bit decimal with sign and terminator; well under the
  // 256-byte stack guidance.
  char buffer[21];
  const int written = snprintf(buffer, sizeof(buffer), "%ld", value);
  if (written <= 0) return;
  set(key, std::string_view(buffer, static_cast<size_t>(written)));
}

void CatalogQuery::clear(const std::string_view key) {
  for (auto it = params_.begin(); it != params_.end(); ++it) {
    if (it->first == key) {
      params_.erase(it);
      return;
    }
  }
}

std::string CatalogQuery::build(const std::string_view path) const {
  if (params_.empty()) return std::string(path);

  std::vector<const std::pair<std::string, std::string>*> ordered;
  ordered.reserve(params_.size());
  for (const auto& param : params_) ordered.push_back(&param);
  std::sort(ordered.begin(), ordered.end(), [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

  std::string url(path);
  url.reserve(path.size() + ordered.size() * 24);
  char separator = '?';
  for (const auto* param : ordered) {
    url.push_back(separator);
    separator = '&';
    url += urlEncodeComponent(param->first);
    url.push_back('=');
    url += urlEncodeComponent(param->second);
  }
  return url;
}

}  // namespace bookorbit
