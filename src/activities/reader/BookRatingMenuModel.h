#pragma once

#include <cstddef>
#include <cstdint>

// Option list for the reader menu's rating picker: "Not rated" plus 1..5 stars.
// Header-only and free of device types so the mapping is host-testable.
inline constexpr int kRatingOptionCount = 6;

// Returns the rating an option index selects; 0 means "not rated".
constexpr int ratingForOptionIndex(const int index) {
  if (index < 1 || index >= kRatingOptionCount) return 0;
  return index;
}

// Returns the option index that should start focused for a stored rating.
constexpr int optionIndexForRating(const bool ratingSet, const uint8_t rating) {
  if (!ratingSet || rating < 1 || rating > 5) return 0;
  return static_cast<int>(rating);
}

// Writes an ASCII star bar such as "***..". Empty for "not rated". The reader
// renders this beside the row label, so it must fit whatever buffer it is given.
inline void ratingStarsLabel(const int rating, char* buf, const size_t len) {
  if (buf == nullptr || len == 0) return;
  if (rating < 1 || rating > 5) {
    buf[0] = '\0';
    return;
  }
  size_t written = 0;
  for (int i = 0; i < 5 && written + 1 < len; i++) {
    buf[written++] = (i < rating) ? '*' : '.';
  }
  buf[written] = '\0';
}
