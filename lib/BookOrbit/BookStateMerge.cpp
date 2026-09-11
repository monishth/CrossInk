#include "BookStateMerge.h"

namespace bookorbit {

MergeDecision resolveBookState(const LocalBookState& local, const ServerBookState& server) {
  MergeDecision decision;

  if (server.ratingKnown) {
    const bool sameValue =
        (local.ratingSet == server.ratingSet) && (!server.ratingSet || local.rating == server.rating);
    if (!sameValue && resolveByDate(local.statusModified, server.ratingUpdatedAt) == Winner::Server) {
      decision.applyRating = true;
      decision.ratingSet = server.ratingSet;
      decision.rating = server.ratingSet ? server.rating : 0;
      decision.ratingModified = server.ratingUpdatedAt;
    }
  }

  if (server.reviewKnown) {
    const bool sameValue =
        (local.reviewSet == server.reviewNoteSet) && (!server.reviewNoteSet || local.reviewNote == server.reviewNote);
    if (!sameValue && resolveByDate(local.reviewModified, server.reviewUpdatedAt) == Winner::Server) {
      decision.applyReview = true;
      decision.reviewSet = server.reviewNoteSet;
      decision.reviewNote = server.reviewNoteSet ? truncateReview(server.reviewNote) : std::string();
      decision.reviewModified = server.reviewUpdatedAt;
    }
  }

  return decision;
}

void applyMerge(const MergeDecision& decision, LocalBookState& local) {
  if (decision.applyRating) {
    local.ratingSet = decision.ratingSet;
    local.rating = decision.ratingSet ? decision.rating : 0;
    if (decision.ratingModified.valid()) {
      local.statusModified = decision.ratingModified;
    }
  }
  if (decision.applyReview) {
    local.reviewSet = decision.reviewSet;
    local.reviewNote = decision.reviewSet ? decision.reviewNote : std::string();
    if (decision.reviewModified.valid()) {
      local.reviewModified = decision.reviewModified;
    }
  }
}

}  // namespace bookorbit
