#pragma once

#include <string>
#include <string_view>

namespace bookorbit {

// Redirect hop cap for catalog transfers. Five is enough for a server behind a
// path rewrite and a signed-URL handoff, and bounded enough that a redirect
// loop cannot hold the device's only socket open.
constexpr int kMaxRedirectHops = 5;

// "scheme://host:port" with the scheme and host lowercased and the port always
// explicit, so a default port compares equal to a written-out one. Returns ""
// for anything that is not an absolute http(s) URL.
std::string originOf(std::string_view url);

// True when both URLs resolve to the same origin. A scheme downgrade is never
// the same origin: following one would put the x-auth-key header on the wire
// in clear.
bool isSameOrigin(std::string_view a, std::string_view b);

}  // namespace bookorbit
