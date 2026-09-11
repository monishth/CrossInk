#include "BookOrbitCapabilities.h"

#include <algorithm>

namespace bookorbit {

bool CapabilityCache::contains(const std::vector<std::string>& haystack, const std::string_view needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

void CapabilityCache::rememberFromVersionResponse(const std::vector<std::string>& names) {
  supported = names;
  unsupported.clear();
  authoritative = true;
}

void CapabilityCache::rememberFailure(const Error& error) {
  if (isTransient(error)) {
    // Carries no information. Leave prior state exactly as it was.
    return;
  }
  supported.clear();
  unsupported.clear();
  authoritative = true;
}

void CapabilityCache::markUnsupported(const std::string_view name) {
  supported.erase(std::remove(supported.begin(), supported.end(), name), supported.end());
  if (!contains(unsupported, name)) {
    unsupported.emplace_back(name);
  }
}

Capability CapabilityCache::get(const std::string_view name) const {
  if (contains(supported, name)) return Capability::Supported;
  if (contains(unsupported, name)) return Capability::Unsupported;
  return authoritative ? Capability::Unsupported : Capability::Unknown;
}

void CapabilityCache::invalidate() {
  supported.clear();
  unsupported.clear();
  authoritative = false;
}

}  // namespace bookorbit
