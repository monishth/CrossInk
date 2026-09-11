#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// BookOrbit tracks per-field modification at date granularity only. Storing
// three integers rather than a string keeps the persisted record fixed-size.
struct DateOnly {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool valid() const { return year != 0 && month != 0 && day != 0; }
};

// Accepts "YYYY-MM-DD" and any longer string whose first ten characters are a
// valid date (the server sends full timestamps in ratingUpdatedAt).
bool parseDateOnly(std::string_view text, DateOnly& out);

// Writes "YYYY-MM-DD", or "" when the date is invalid. buf must hold 11 bytes.
void formatDateOnly(const DateOnly& date, char* buf, size_t len);

// < 0, 0, > 0 like strcmp.
int compareDateOnly(const DateOnly& lhs, const DateOnly& rhs);

enum class Winner : uint8_t { Local, Server };

// The side with the newer date wins. A tie prefers the server, and so does the
// case where neither side carries a usable date.
Winner resolveByDate(const DateOnly& local, const DateOnly& server);

}  // namespace bookorbit
