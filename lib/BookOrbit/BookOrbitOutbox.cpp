#include "BookOrbitOutbox.h"

namespace bookorbit {

SyncPhase SyncOutbox::nextPhase(const SyncPhase from) const {
  switch (from) {
    case SyncPhase::Match:
      return SyncPhase::Stats;
    case SyncPhase::Stats:
      return SyncPhase::Progress;
    case SyncPhase::Progress:
      return SyncPhase::State;
    case SyncPhase::State:
      return SyncPhase::Annotations;
    case SyncPhase::Annotations:
      return SyncPhase::Bookmarks;
    case SyncPhase::Bookmarks:
      return SyncPhase::Done;
    case SyncPhase::Done:
      return SyncPhase::Done;
  }
  return SyncPhase::Done;
}

bool SyncOutbox::advance(const Error& phaseResult) {
  if (aborted || phase == SyncPhase::Done) return false;

  if (isAuthError(phaseResult) || phaseResult.status == Status::Transport) {
    aborted = true;
    errors = true;
    return false;
  }

  if (phaseResult.status != Status::Ok) {
    errors = true;  // watermark stays unadvanced; retried next trigger
  }

  phase = nextPhase(phase);
  return true;
}

bool SyncOutbox::acknowledge(const SyncPhase completed, SyncStateStore& state) {
  (void)completed;
  // The durability guarantee: state hits disk before the phase is considered
  // complete, so a crash re-runs the phase rather than skipping it.
  return state.flush();
}

}  // namespace bookorbit
