#include "SameOrigin.h"

namespace bookorbit {
namespace {

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

std::string toLower(const std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) out.push_back(lower(c));
  return out;
}

}  // namespace

std::string originOf(const std::string_view url) {
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return {};

  const std::string scheme = toLower(url.substr(0, schemeEnd));
  const char* defaultPort = nullptr;
  if (scheme == "https") {
    defaultPort = "443";
  } else if (scheme == "http") {
    defaultPort = "80";
  } else {
    return {};
  }

  const std::string_view rest = url.substr(schemeEnd + 3);
  const auto pathStart = rest.find('/');
  const std::string_view authority = pathStart == std::string_view::npos ? rest : rest.substr(0, pathStart);
  if (authority.empty()) return {};

  const auto portStart = authority.rfind(':');
  std::string_view host = authority;
  std::string_view port(defaultPort);
  if (portStart != std::string_view::npos && portStart + 1 < authority.size()) {
    host = authority.substr(0, portStart);
    port = authority.substr(portStart + 1);
  }
  if (host.empty()) return {};

  std::string origin = scheme;
  origin += "://";
  origin += toLower(host);
  origin += ':';
  origin.append(port);
  return origin;
}

bool isSameOrigin(const std::string_view a, const std::string_view b) {
  const std::string left = originOf(a);
  if (left.empty()) return false;
  return left == originOf(b);
}

}  // namespace bookorbit
