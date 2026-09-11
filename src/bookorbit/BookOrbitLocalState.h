#pragma once

#include <cstdint>
#include <string>

/**
 * Device-side helpers for recording a book's BookOrbit state locally, so the
 * reader can update status and rating without knowing about blob stores.
 *
 * Each call loads the store, mutates one book, and flushes. That is deliberate:
 * these fire on deliberate user gestures (finishing a book, setting a rating),
 * not on page turns, so `AGENTS.md`'s "debounce persistent writes" rule is not
 * in tension here. The store is never held open across activities.
 *
 * Every function is a no-op when BookOrbit is disabled or the clock has no
 * valid date — conflict resolution is entirely date-driven, so an edit that
 * cannot be stamped is refused rather than stamped with a fabricated date.
 */
namespace bookorbit_local {

// Records that a book was marked finished (or unfinished). Returns true when
// something changed and was persisted.
bool recordCompletion(const std::string& epubPath, bool isCompleted);

// Records a 1-5 rating, or clears it when rating <= 0.
bool recordRating(const std::string& epubPath, int rating);

// The rating currently stored for a book, or 0 when unrated/unknown.
int storedRating(const std::string& epubPath);

}  // namespace bookorbit_local
