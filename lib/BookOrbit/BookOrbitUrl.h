#pragma once

#include <string>
#include <string_view>

namespace bookorbit {

// Normalizes a user-entered server URL to the BookOrbit API base, which always
// ends in "/api/v1". Returns "" when the input is empty or whitespace only.
std::string normalizeServerUrl(std::string_view input);

// Joins an absolute path onto a normalized base, collapsing the seam slash.
std::string joinPath(std::string_view base, std::string_view path);

}  // namespace bookorbit
