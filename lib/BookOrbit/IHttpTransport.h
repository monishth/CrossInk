#pragma once

#include <string>
#include <utility>
#include <vector>

namespace bookorbit {

struct HttpRequest {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  bool transportFailed = false;
  std::string body;
};

// Injected so the BookOrbit core is host-testable. The device implementation
// wraps freeink::SecureHttpClient; tests use a recording fake.
class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  virtual HttpResponse send(const HttpRequest& request) = 0;
};

}  // namespace bookorbit
