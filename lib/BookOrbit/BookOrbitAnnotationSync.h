#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitClient.h"
#include "BookOrbitExchangeAck.h"
#include "BookOrbitExchangeResponse.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

struct ExchangeOutcome {
  size_t uploaded = 0;
  size_t applied = 0;
  size_t deleted = 0;
  size_t failed = 0;
  bool hadErrors = false;
  bool skipped = false;    // signature unchanged; nothing was sent
  bool unmatched = false;  // the server does not know this book
};

// Applies server-side changes to whatever holds the local set — the open
// reader, or the on-disk stores when the book is closed. Injected so the
// exchange state machine is host-testable; a std::function would cost more
// than a vtable here and library code avoids it.
class IAnnotationApplier {
 public:
  virtual ~IAnnotationApplier() = default;

  // Returns how many local entries were actually touched. Every entry must
  // produce exactly one ack, failed or not.
  virtual size_t applyAdds(const std::vector<RemoteEntry>& adds, std::vector<AppliedAck>& acks) = 0;
  virtual size_t applyDeletes(const std::vector<RemoteEntry>& deletes, std::vector<DeletedAck>& acks) = 0;
};

// Runs the full three-legged exchange for one book.
//
// Returns the transport-level Error. Status::Ok covers "skipped", "unmatched"
// and "applied everything" — read `outcome` for which. Status::Unauthorized
// aborts the whole sync at the caller's level; every other error leaves the
// book's skip signature unstamped so the next trigger retries. There are no
// retry loops here, deliberately: on a battery device a failed phase waits for
// the next sync trigger.
Error exchangeAnnotations(BookOrbitClient& client, BookSyncState& book, std::string_view hash,
                          const NormalizedAnnotations& local, IAnnotationApplier& applier, uint32_t nowUnix,
                          ExchangeOutcome& outcome);

}  // namespace bookorbit
