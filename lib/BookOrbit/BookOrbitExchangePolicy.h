#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// Six hours, matching BookOrbitAnnotations.EXCHANGE_MAX_AGE. The exchange is
// the only channel that delivers server-created annotations, so an unchanged
// book may skip cheaply but never indefinitely.
inline constexpr uint32_t kExchangeMaxAgeSeconds = 6 * 3600;

// True when this book's local set is byte-identical to the set last fully
// exchanged, and that exchange is recent enough to trust.
bool canSkipExchange(const char* storedSignature, uint32_t exchangedAt, std::string_view signature, uint32_t nowUnix);

// Records a complete exchange. Returns false — writing nothing — when the
// signature does not fit the destination field, because a truncated signature
// would compare equal to a different set and park the book forever.
bool rememberExchanged(char* storedSignature, size_t capacity, uint32_t& exchangedAt, std::string_view signature,
                       uint32_t nowUnix);

}  // namespace bookorbit
