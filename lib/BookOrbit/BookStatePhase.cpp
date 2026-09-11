#include "BookStatePhase.h"

#include <cstring>
#include <vector>

#include "BookStateCodec.h"
#include "BookStateMerge.h"

namespace bookorbit {
namespace {

void copyDateInto(char (&dest)[11], const DateOnly& date) { formatDateOnly(date, dest, sizeof(dest)); }

}  // namespace

BookStatePhase::BookStatePhase(BookOrbitClient& httpClient, SyncStateStore& sync, BookStateStore& states)
    : client(httpClient), syncState(sync), stateStore(states) {}

StatePhaseOutcome BookStatePhase::run(const std::string_view md5, const bool forcePull, const uint32_t nowUnix) {
  requestBody.clear();

  // Establish the sync-state record before doing any work. A phase that fails
  // must leave a record whose watermark is visibly unadvanced, not no record at
  // all — the outbox and the tests both read the watermark back after a
  // failure. Nothing is written to disk until flush() on the success path, so
  // this costs no I/O.
  syncState.findOrCreate(md5);

  BookStateRecord& record = stateStore.findOrCreate(md5);
  const bool pullDue = forcePull || stateStore.needsStatePull(record, nowUnix);

  BookStatePayload payload;
  if (!buildStatePayload(md5, record.local, record.synced, pullDue, payload)) {
    return StatePhaseOutcome::Skipped;
  }

  std::vector<BookStatePayload> batch;
  batch.reserve(1);
  batch.push_back(payload);
  // Every /koreader/plugin/* POST carries the device fields; the phase routes
  // the encoded body through the client so identity stays in one place.
  requestBody = client.withDeviceFields(encodeBookStates(batch), nowUnix);

  std::string responseBody;
  const Error error = client.postJson(kBookStatesPath, requestBody, responseBody);
  if (isAuthError(error)) {
    return StatePhaseOutcome::AuthFailed;
  }
  if (error.status != Status::Ok) {
    return StatePhaseOutcome::Failed;
  }

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  if (!decodeBookStates(responseBody, unmatched, results)) {
    return StatePhaseOutcome::Failed;
  }
  for (const auto& hash : unmatched) {
    if (hash == record.md5) {
      return StatePhaseOutcome::Unmatched;
    }
  }

  const ServerBookState* result = nullptr;
  for (const auto& candidate : results) {
    if (candidate.hash == record.md5) {
      result = &candidate;
      break;
    }
  }

  if (result != nullptr) {
    applyMerge(resolveBookState(record.local, *result), record.local);
    if (result->ratingKnown) {
      record.synced.ratingKnown = true;
      record.synced.ratingSet = record.local.ratingSet;
      record.synced.rating = record.local.rating;
    }
    if (result->reviewKnown) {
      record.synced.reviewKnown = true;
      record.synced.reviewSet = record.local.reviewSet;
      record.synced.reviewNote = record.local.reviewNote;
    }
  }

  // Whatever was asserted in the payload is now the server's view, even when
  // the server kept its own value on a tie: the device value was considered.
  if (payload.hasRating || payload.ratingCleared) {
    record.synced.ratingKnown = true;
    if (result == nullptr || !result->ratingKnown) {
      record.synced.ratingSet = payload.hasRating;
      record.synced.rating = payload.hasRating ? payload.rating : 0;
    }
  }
  if (payload.hasReview || payload.reviewCleared) {
    record.synced.reviewKnown = true;
    if (result == nullptr || !result->reviewKnown) {
      record.synced.reviewSet = payload.hasReview;
      record.synced.reviewNote = payload.hasReview ? payload.reviewNote : std::string();
    }
  }
  if (payload.statusModified.valid()) {
    record.synced.statusSyncedModified = payload.statusModified;
  }
  if (pullDue) {
    BookStateStore::markStatePulled(record, nowUnix);
  }

  // The watermark lives in P0's per-book record so every phase's durability
  // story stays in one file.
  BookSyncState& book = syncState.findOrCreate(md5);
  if (payload.statusModified.valid()) {
    copyDateInto(book.statusSyncedModified, payload.statusModified);
  }

  // Persist before returning: the annotations phase must never start on top of
  // an unacknowledged state result.
  if (!stateStore.flush() || !syncState.flush()) {
    return StatePhaseOutcome::Failed;
  }
  return StatePhaseOutcome::Synced;
}

}  // namespace bookorbit
