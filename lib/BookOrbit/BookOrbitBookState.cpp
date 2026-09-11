#include "BookOrbitBookState.h"

namespace bookorbit {

const char* statusToString(const BookStatus status) {
  switch (status) {
    case BookStatus::Complete:
      return "complete";
    case BookStatus::Abandoned:
      return "abandoned";
    case BookStatus::Reading:
    default:
      return "reading";
  }
}

bool statusFromString(const std::string_view text, BookStatus& out) {
  if (text == "reading") {
    out = BookStatus::Reading;
    return true;
  }
  if (text == "complete") {
    out = BookStatus::Complete;
    return true;
  }
  if (text == "abandoned") {
    out = BookStatus::Abandoned;
    return true;
  }
  return false;
}

BookStatus statusFromCompletion(const bool isCompleted, const BookStatus previous) {
  if (isCompleted) return BookStatus::Complete;
  if (previous == BookStatus::Abandoned) return BookStatus::Abandoned;
  return BookStatus::Reading;
}

bool normalizeRating(const int raw, uint8_t& out) {
  if (raw < 1 || raw > 5) return false;
  out = static_cast<uint8_t>(raw);
  return true;
}

std::string truncateReview(const std::string_view note) {
  if (note.size() <= kReviewNoteMaxBytes) return std::string(note);
  return std::string(note.substr(0, kReviewNoteMaxBytes));
}

void LocalBookState::setStatus(const BookStatus value, const DateOnly& today) {
  statusKnown = true;
  status = value;
  statusModified = today;
}

bool LocalBookState::setRating(const int value, const DateOnly& today) {
  uint8_t normalized = 0;
  if (!normalizeRating(value, normalized)) return false;
  ratingSet = true;
  rating = normalized;
  statusModified = today;
  return true;
}

void LocalBookState::clearRating(const DateOnly& today) {
  ratingSet = false;
  rating = 0;
  statusModified = today;
}

void LocalBookState::setReview(const std::string_view note, const DateOnly& today) {
  reviewSet = true;
  reviewNote = truncateReview(note);
  reviewModified = today;
}

void LocalBookState::clearReview(const DateOnly& today) {
  reviewSet = false;
  reviewNote.clear();
  reviewModified = today;
}

bool applyCompletionToggle(LocalBookState& local, const bool isCompleted, const DateOnly& today) {
  if (!today.valid()) return false;
  const BookStatus next = statusFromCompletion(isCompleted, local.statusKnown ? local.status : BookStatus::Reading);
  if (local.statusKnown && local.status == next) return false;
  local.setStatus(next, today);
  return true;
}

}  // namespace bookorbit
