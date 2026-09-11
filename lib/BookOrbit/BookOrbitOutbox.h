#pragma once

#include "BookOrbitError.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

enum class SyncPhase {
  Match,
  Stats,
  Progress,
  State,
  Annotations,
  Bookmarks,
  Done,
};

// Sequences one book's sync phases, persisting each acknowledgement before
// advancing so a crash can never skip a watermark.
//
// There is deliberately no retry loop. A failed phase leaves its watermark
// unadvanced and retries on the next sync trigger; on a battery device a timed
// retry loop is the wrong default.
class SyncOutbox {
 public:
  SyncPhase currentPhase() const { return phase; }
  bool isAborted() const { return aborted; }
  bool hadErrors() const { return errors; }

  // Applies one phase's outcome. Returns true when the sync should continue.
  // Auth and transport failures abort everything; other errors skip just this
  // phase.
  bool advance(const Error& phaseResult);

  // Persists state before the phase transition is considered durable.
  bool acknowledge(SyncPhase completed, SyncStateStore& state);

  SyncPhase nextPhase(SyncPhase from) const;

 private:
  SyncPhase phase = SyncPhase::Match;
  bool aborted = false;
  bool errors = false;
};

}  // namespace bookorbit
