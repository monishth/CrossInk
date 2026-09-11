#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitBookState.h"
#include "BookStateCodec.h"
#include "IBlobStore.h"

namespace bookorbit {

// A forced pull is the only way a rating written on the web reaches the device,
// so it is bounded by age rather than run on every sync.
inline constexpr uint32_t kStatePullMaxAgeSeconds = 86400;

struct BookStateRecord {
  char md5[33] = {};
  LocalBookState local;
  SyncedBookState synced;
  uint32_t statePulledAt = 0;
};

// Holds every book's status/rating/review plus the last-known-server shadow in
// one atomically-written blob, beside BookOrbitSyncState's watermark blob.
class BookStateStore {
 public:
  BookStateStore(IBlobStore& blobs, std::string path);

  // Returns true on success, including when no file exists yet or the blob is
  // unreadable — a corrupt state file degrades to "re-sync", never to a crash.
  bool load();
  bool flush();

  BookStateRecord* find(std::string_view md5);
  BookStateRecord& findOrCreate(std::string_view md5);

  bool needsStatePull(const BookStateRecord& record, uint32_t nowUnix) const;
  static void markStatePulled(BookStateRecord& record, uint32_t nowUnix);

 private:
  static constexpr uint8_t kFormatVersion = 1;

  IBlobStore& blobs;
  std::string path;
  std::vector<BookStateRecord> records;
};

}  // namespace bookorbit
