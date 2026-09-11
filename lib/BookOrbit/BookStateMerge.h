#pragma once

#include <cstdint>
#include <string>

#include "BookOrbitBookState.h"
#include "BookOrbitDate.h"
#include "BookStateCodec.h"

namespace bookorbit {

// What the device should adopt from one server result. Each field is decided
// independently, because the server dates them independently.
struct MergeDecision {
  bool applyRating = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  DateOnly ratingModified;
  bool applyReview = false;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly reviewModified;

  bool changedAnything() const { return applyRating || applyReview; }
};

// Newer date wins; a tie prefers the server. A field the server did not answer
// for is never touched, and a server value identical to the local one is not
// reported as a change so callers can skip a pointless write.
MergeDecision resolveBookState(const LocalBookState& local, const ServerBookState& server);

// Writes an accepted decision into the local record, stamping the server's date
// so the next buildStatePayload does not treat the pulled value as a local edit.
void applyMerge(const MergeDecision& decision, LocalBookState& local);

}  // namespace bookorbit
