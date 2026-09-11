#include "BookOrbitBookmarkSync.h"

#include <Logging.h>

#include <string>
#include <vector>

#include "BookOrbitExchangeAck.h"
#include "BookOrbitExchangePolicy.h"
#include "BookOrbitExchangeRequest.h"
#include "BookOrbitExchangeResponse.h"

namespace bookorbit {
namespace {

const ExchangeBookResult* findResult(const ExchangeResponse& response, const std::string_view hash) {
  for (const auto& result : response.results) {
    if (result.hash == hash) return &result;
  }
  return response.results.empty() ? nullptr : &response.results.front();
}

bool mentionsHash(const std::vector<std::string>& hashes, const std::string_view hash) {
  for (const auto& candidate : hashes) {
    if (candidate == hash) return true;
  }
  return false;
}

bool hasPending(const ExchangeBookResult& result) { return !result.add.empty() || !result.remove.empty(); }

// One place decides what an error means for the capability, so the tri-state
// rule cannot be half-applied.
void noteRouteError(CapabilityCache& capabilities, const Error& error) {
  if (error.status == Status::NotFound) {
    // Definitive: this server has no bookmark route.
    capabilities.markUnsupported(kBookmarkCapability);
    return;
  }
  // Everything else — 5xx, transport, auth — says nothing about support.
  // Caching a negative here is exactly the bug the tri-state design exists to
  // prevent.
}

}  // namespace

Error exchangeBookmarks(BookOrbitClient& client, CapabilityCache& capabilities, BookSyncState& book,
                        const std::string_view hash, const NormalizedBookmarks& local, IAnnotationApplier& applier,
                        const uint32_t nowUnix, ExchangeOutcome& outcome) {
  outcome = ExchangeOutcome{};

  if (capabilities.get(kBookmarkCapability) == Capability::Unsupported) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  if (canSkipExchange(book.bmSignature, book.bmExchangedAt, local.signature, nowUnix)) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  const std::vector<BookmarkKey> keys = collectBookmarkKeys(local.entries);
  const bool keysComplete = keys.size() <= kMaxBookmarkKeysPerBook;

  ExchangeResponse response;
  ExchangeBookResult pending;
  bool firstRequest = true;
  size_t cursor = 0;

  do {
    std::vector<Bookmark> chunk;
    chunk.reserve(kUploadChunk);
    while (cursor < local.entries.size() && chunk.size() < kUploadChunk) {
      chunk.push_back(local.entries[cursor]);
      cursor++;
    }

    const std::string body = encodeBookmarkExchange(hash, firstRequest ? keys : std::vector<BookmarkKey>{},
                                                    firstRequest && keysComplete, chunk);

    std::string responseBody;
    const Error error = client.postJson(kBookmarkExchangePath, body, responseBody);
    if (error.status != Status::Ok) {
      LOG_ERR("BORB", "bookmark exchange failed (%d)", error.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, error);
      return error;
    }

    if (!decodeExchangeResponse(responseBody, response)) {
      LOG_ERR("BORB", "bookmark exchange response was not valid JSON");
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
      pullComplete = false;
      break;
    }
    rounds++;

    std::vector<AppliedAck> appliedAcks;
    std::vector<DeletedAck> deletedAcks;
    outcome.applied += applier.applyAdds(pending.add, appliedAcks);
    outcome.deleted += applier.applyDeletes(pending.remove, deletedAcks);
    for (const auto& ack : appliedAcks) {
      if (ack.failed) outcome.failed++;
    }

    std::string ackResponse;
    const Error ackError =
        client.postJson(kBookmarkAckPath, encodeExchangeAck(hash, appliedAcks, deletedAcks), ackResponse);
    if (ackError.status != Status::Ok) {
      LOG_ERR("BORB", "bookmark exchange ack failed (%d)", ackError.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, ackError);
      pullComplete = false;
      break;
    }

    if (!pending.more) break;

    std::string followUpBody;
    const Error followUp =
        client.postJson(kBookmarkExchangePath, encodeBookmarkExchange(hash, {}, false, {}), followUpBody);
    if (followUp.status != Status::Ok) {
      LOG_ERR("BORB", "bookmark exchange follow-up failed (%d)", followUp.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, followUp);
      pullComplete = false;
      break;
    }
    if (!decodeExchangeResponse(followUpBody, response)) {
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    const ExchangeBookResult* result = findResult(response, hash);
    pending = (result != nullptr) ? *result : ExchangeBookResult{};
  }

  if (pullComplete && !outcome.hadErrors && outcome.failed == 0 && keysComplete) {
    rememberExchanged(book.bmSignature, sizeof(book.bmSignature), book.bmExchangedAt, local.signature, nowUnix);
  }
  return {Status::Ok, 0};
}

}  // namespace bookorbit
