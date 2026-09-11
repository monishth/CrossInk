#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IBlobStore.h"

namespace bookorbit {

// Matches cache for 24 hours, as the Lua plugin's BookOrbitState.MATCH_MAX_AGE does.
inline constexpr uint32_t kMatchMaxAgeSeconds = 86400;

// Per-book sync bookkeeping, keyed by the file's partial MD5.
// Field names mirror bookorbit_sync_state.lua so the two stay comparable.
struct BookSyncState {
  char md5[33] = {};                   // 32 hex chars + NUL
  uint32_t statsWatermark = 0;         // max uploaded event startTime
  uint32_t matchVerifiedAt = 0;        // unix; 24h TTL
  char matchVerifiedVersion[24] = {};  // server libraryVersion at match time
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  float progressPushedPct = 0.0f;
  char annSignature[48] = {};  // "count:maxDt:h1:h2"
  char bmSignature[48] = {};
  uint32_t annExchangedAt = 0;
  uint32_t bmExchangedAt = 0;
  char statusSyncedModified[11] = {};  // YYYY-MM-DD

  void setMd5(std::string_view value);
  void setMatchVerifiedVersion(std::string_view value);
};

// Holds every book's sync state in one atomically-written blob.
class SyncStateStore {
 public:
  SyncStateStore(IBlobStore& store, std::string path);

  // Returns true on success, including when no file exists yet or the existing
  // blob is unreadable — a corrupt state file must degrade to "sync everything
  // again", never to a crash.
  bool load();
  bool flush();

  BookSyncState* find(std::string_view md5);
  BookSyncState& findOrCreate(std::string_view md5);

  void setLibraryVersion(std::string_view value);
  std::string libraryVersion() const { return library; }

  // Fresh means: matched within 24h AND matched against the current
  // libraryVersion. Either condition failing forces a re-match.
  bool isMatchFresh(const BookSyncState& book, uint32_t nowUnix) const;

 private:
  static constexpr uint8_t kFormatVersion = 1;

  IBlobStore& blobs;
  std::string path;
  std::string library;
  std::vector<BookSyncState> books;
};

}  // namespace bookorbit
