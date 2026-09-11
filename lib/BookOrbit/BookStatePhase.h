#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "BookOrbitClient.h"
#include "BookOrbitSyncState.h"
#include "BookStateStore.h"

namespace bookorbit {

// The state phase's place in the per-book chain is
// match -> stats -> progress -> state -> annotations -> bookmarks.
inline constexpr char kBookStatesPath[] = "/koreader/plugin/book-states";

enum class StatePhaseOutcome : uint8_t {
  Skipped,     // nothing changed and no pull was due — no request was made
  Synced,      // request succeeded; watermark advanced and persisted
  Unmatched,   // the server does not hold this book; watermark unchanged
  AuthFailed,  // 401/403 — the caller aborts the whole sync
  Failed,      // any other error; watermark unchanged, retried next trigger
};

// Runs one book's state phase. There is no retry loop by design: a failure
// leaves the watermark unadvanced and is retried on the next sync trigger.
class BookStatePhase {
 public:
  BookStatePhase(BookOrbitClient& client, SyncStateStore& syncState, BookStateStore& stateStore);

  StatePhaseOutcome run(std::string_view md5, bool forcePull, uint32_t nowUnix);

  const std::string& lastRequestBody() const { return requestBody; }

 private:
  BookOrbitClient& client;
  SyncStateStore& syncState;
  BookStateStore& stateStore;
  std::string requestBody;
};

}  // namespace bookorbit
