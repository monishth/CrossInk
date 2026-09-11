#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "BookOrbitDate.h"

namespace bookorbit {

// Server-side truncation limit for a review note, matching bookorbit_sidecar.lua.
inline constexpr size_t kReviewNoteMaxBytes = 10000;

// The wire vocabulary is closed: "reading" | "complete" | "abandoned".
enum class BookStatus : uint8_t { Reading, Complete, Abandoned };

const char* statusToString(BookStatus status);

// Returns false and leaves `out` untouched for any string outside the
// vocabulary, including differently-cased spellings.
bool statusFromString(std::string_view text, BookStatus& out);

// Maps CrossInk's BookReadingStats::isCompleted onto the wire status.
// "abandoned" has no CrossInk gesture, so a previously pulled "abandoned"
// survives an un-completed flag instead of being overwritten with "reading".
BookStatus statusFromCompletion(bool isCompleted, BookStatus previous);

// Accepts 1..5 only. Out-of-range values are dropped, not clamped.
bool normalizeRating(int raw, uint8_t& out);

std::string truncateReview(std::string_view note);

// The device's own view of one book's state. `statusModified` covers both the
// status and the rating, exactly as buildStatePayload in the Lua plugin does;
// the review carries its own date.
struct LocalBookState {
  bool statusKnown = false;
  BookStatus status = BookStatus::Reading;
  DateOnly statusModified;
  bool ratingSet = false;
  uint8_t rating = 0;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly reviewModified;

  void setStatus(BookStatus value, const DateOnly& today);
  bool setRating(int value, const DateOnly& today);
  void clearRating(const DateOnly& today);
  void setReview(std::string_view note, const DateOnly& today);
  void clearReview(const DateOnly& today);
};

// Applies CrossInk's "mark as finished" toggle to the synced state. Returns
// false — changing nothing — when the resulting status already matches or the
// date is unusable, so an idle re-open never wins a conflict.
bool applyCompletionToggle(LocalBookState& local, bool isCompleted, const DateOnly& today);
}  // namespace bookorbit
