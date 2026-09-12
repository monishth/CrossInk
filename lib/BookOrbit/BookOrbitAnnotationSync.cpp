#include "BookOrbitAnnotationSync.h"

#include <Logging.h>

#include <string>

#include "BookOrbitExchangePolicy.h"
#include "BookOrbitExchangeRequest.h"

namespace bookorbit {
namespace {

const ExchangeBookResult* findResult(const ExchangeResponse& response, const std::string_view hash) {
  for (const auto& result : response.results) {
    if (result.hash == hash) return &result;
  }
  // Single-book requests: some servers echo no hash at all.
  return response.results.empty() ? nullptr : &response.results.front();
}

bool mentionsHash(const std::vector<std::string>& hashes, const std::string_view hash) {
  for (const auto& candidate : hashes) {
    if (candidate == hash) return true;
  }
  return false;
}

bool hasPending(const ExchangeBookResult& result) { return !result.add.empty() || !result.remove.empty(); }

}  // namespace

Error exchangeAnnotations(BookOrbitClient& client, BookSyncState& book, const std::string_view hash,
                          const NormalizedAnnotations& local, IAnnotationApplier& applier, const uint32_t nowUnix,
                          ExchangeOutcome& outcome) {
  outcome = ExchangeOutcome{};

  if (canSkipExchange(book.annSignature, book.annExchangedAt, local.signature, nowUnix)) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  const std::vector<AnnotationKey> keys = collectAnnotationKeys(local.entries);
  const bool keysComplete = keys.size() <= kMaxAnnotationKeysPerBook;

  ExchangeResponse response;
  ExchangeBookResult pending;

  bool firstRequest = true;
  size_t cursor = 0;

  // One pass even with nothing to upload: the exchange is the only channel
  // that delivers server-created highlights.
  do {
    std::vector<Annotation> chunk;
    chunk.reserve(kUploadChunk);
    while (cursor < local.entries.size() && chunk.size() < kUploadChunk) {
      chunk.push_back(local.entries[cursor]);
      cursor++;
    }

    const std::string body = encodeAnnotationExchange(hash, firstRequest ? keys : std::vector<AnnotationKey>{},
                                                      firstRequest && keysComplete, chunk);

    std::string responseBody;
    const Error error = client.postJson(kAnnotationExchangePath, client.withDeviceFields(body, nowUnix), responseBody);
    if (error.status != Status::Ok) {
      LOG_ERR("BORB", "annotation exchange failed (%d)", error.httpStatus);
      outcome.hadErrors = true;
      // Auth aborts the whole sync at the caller's level; every other error
      // just leaves this book unstamped for the next trigger.
      return error;
    }

    if (!decodeExchangeResponse(responseBody, response)) {
      LOG_ERR("BORB", "annotation exchange response was not valid JSON");
      outcome.hadErrors = true;
      return {Status::InvalidJson, 0};
    }

    if (mentionsHash(response.unmatched, hash)) {
      outcome.unmatched = true;
      return {Status::Ok, 0};
    }

    const ExchangeBookResult* result = findResult(response, hash);
    if (result != nullptr) pending = *result;
    outcome.uploaded += chunk.size();
    firstRequest = false;
  } while (cursor < local.entries.size());

  size_t rounds = 0;
  bool pullComplete = true;

  while (hasPending(pending)) {
    if (rounds >= kMaxPullRounds) {
      // A server stuck on more:true must not pin the reader in a loop; the
      // rest arrives on the next sync trigger.
      pullComplete = false;
      break;
    }
    rounds++;

    std::vector<AppliedAck> appliedAcks;
    std::vector<DeletedAck> deletedAcks;
    const size_t addedTouched = applier.applyAdds(pending.add, appliedAcks);
    const size_t deletedTouched = applier.applyDeletes(pending.remove, deletedAcks);
    outcome.applied += addedTouched;
    outcome.deleted += deletedTouched;
    for (const auto& ack : appliedAcks) {
      if (ack.failed) outcome.failed++;
    }

    std::string ackResponse;
    const Error ackError = client.postJson(
        kAnnotationAckPath, client.withDeviceFields(encodeExchangeAck(hash, appliedAcks, deletedAcks), nowUnix),
        ackResponse);
    if (ackError.status != Status::Ok) {
      // The changes are on disk but the server never heard so. Leaving the
      // book unstamped makes it re-exchange; the applier dedupes by identity,
      // so the re-delivery is a no-op rather than a duplicate.
      LOG_ERR("BORB", "annotation exchange ack failed (%d)", ackError.httpStatus);
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }

    if (!pending.more) break;

    std::string followUpBody;
    const Error followUp =
        client.postJson(kAnnotationExchangePath,
                        client.withDeviceFields(encodeAnnotationExchange(hash, {}, false, {}), nowUnix), followUpBody);
    if (followUp.status != Status::Ok) {
      LOG_ERR("BORB", "annotation exchange follow-up failed (%d)", followUp.httpStatus);
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    if (!decodeExchangeResponse(followUpBody, response)) {
      LOG_ERR("BORB", "annotation follow-up response was not valid JSON");
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    const ExchangeBookResult* followUpResult = findResult(response, hash);
    pending = (followUpResult != nullptr) ? *followUpResult : ExchangeBookResult{};
  }

  // Stamped only when the complete key set went out and nothing was left
  // undelivered. Anything the server will re-send keeps the next exchange
  // mandatory.
  if (pullComplete && !outcome.hadErrors && outcome.failed == 0 && keysComplete) {
    rememberExchanged(book.annSignature, sizeof(book.annSignature), book.annExchangedAt, local.signature, nowUnix);
  }
  return {Status::Ok, 0};
}

}  // namespace bookorbit
