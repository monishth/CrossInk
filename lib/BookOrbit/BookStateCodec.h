#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitBookState.h"
#include "BookOrbitDate.h"

namespace bookorbit {

// POST /koreader/plugin/book-states accepts at most this many books.
inline constexpr size_t kBookStateBatchSize = 200;

// What the server was last known to hold for one book. Persisted alongside the
// local state so an unchanged book costs no request at all.
struct SyncedBookState {
  bool ratingKnown = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  bool reviewKnown = false;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly statusSyncedModified;  // mirrors BookSyncState::statusSyncedModified
};

// One entry of the request's "books" array. Every optional field is explicit:
// hasX means "send X", xCleared means "unset X server-side", and neither means
// "leave it alone".
struct BookStatePayload {
  std::string hash;
  bool hasStatus = false;
  BookStatus status = BookStatus::Reading;
  DateOnly statusModified;
  bool hasRating = false;
  uint8_t rating = 0;
  bool ratingCleared = false;
  bool hasReview = false;
  std::string reviewNote;
  bool reviewCleared = false;
  DateOnly reviewModified;
};

// Returns false when nothing changed and no pull was forced — the caller then
// skips the request entirely. Mirrors buildStatePayload in bookorbit_sidecar.lua.
bool buildStatePayload(std::string_view hash, const LocalBookState& local, const SyncedBookState& synced,
                       bool forcePull, BookStatePayload& out);

// Emits {"books":[...]} only. Device fields are added by BookOrbitClient::withDevice.
std::string encodeBookStates(const std::vector<BookStatePayload>& books);

// One entry of the response's "results" array. The *Known flags distinguish
// "the server answered for this field" from "the field was absent".
struct ServerBookState {
  std::string hash;
  bool ratingKnown = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  DateOnly ratingUpdatedAt;
  bool reviewKnown = false;
  bool reviewNoteSet = false;
  std::string reviewNote;
  DateOnly reviewUpdatedAt;
};

// Returns false only on malformed JSON. A well-formed body with neither array
// is a success with both outputs empty.
bool decodeBookStates(std::string_view json, std::vector<std::string>& unmatched,
                      std::vector<ServerBookState>& results);
}  // namespace bookorbit
