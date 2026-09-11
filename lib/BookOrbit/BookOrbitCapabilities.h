#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitError.h"

namespace bookorbit {

enum class Capability {
  Unknown,  // never asked, or the last answer carried no information
  Supported,
  Unsupported,
};

// Caches what the server advertises at GET /koreader/plugin/version.
//
// The tri-state is the point: a 5xx or a dropped connection must leave a
// capability Unknown so it is retried, never cached as a negative. One blip
// would otherwise disable bookmark sync for the life of the session.
class CapabilityCache {
 public:
  // A successful /version response. Names present are Supported; every name
  // absent from an authoritative response is Unsupported.
  void rememberFromVersionResponse(const std::vector<std::string>& names);

  // A failed /version response. Transient failures change nothing at all;
  // a definitive 4xx means the endpoint does not exist on this server.
  void rememberFailure(const Error& error);

  // A confirmed 404 on a capability's own route downgrades just that one.
  void markUnsupported(std::string_view name);

  Capability get(std::string_view name) const;

  void invalidate();

 private:
  bool authoritative = false;
  std::vector<std::string> supported;
  std::vector<std::string> unsupported;

  static bool contains(const std::vector<std::string>& haystack, std::string_view needle);
};

}  // namespace bookorbit
