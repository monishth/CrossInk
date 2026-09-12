#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitAnnotationSync.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/BookOrbitSyncState.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr uint32_t kNow = 2000000000u;

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  std::vector<bookorbit::HttpResponse> queued;
  bool repeatLast = false;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (cursor < queued.size()) return queued[cursor++];
    if (repeatLast && !queued.empty()) return queued.back();
    return {200, false, R"({"unmatched":[],"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false}]})"};
  }

  size_t countTo(const std::string& pathSuffix) const {
    size_t count = 0;
    for (const auto& request : sent) {
      if (request.url.size() >= pathSuffix.size() &&
          request.url.compare(request.url.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0) {
        count++;
      }
    }
    return count;
  }

 private:
  size_t cursor = 0;
};

// Applies by local identity, exactly as the device store does: re-delivering
// an entry that is already present is a no-op, not a duplicate.
class RecordingApplier : public bookorbit::IAnnotationApplier {
 public:
  std::vector<std::string> storedKeys;
  size_t redelivered = 0;
  bool failEverything = false;

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds, std::vector<bookorbit::AppliedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : adds) {
      bookorbit::AppliedAck ack;
      ack.serverId = entry.serverId;
      if (failEverything) {
        ack.failed = true;
        acks.push_back(ack);
        continue;
      }
      const std::string key = bookorbit::buildAnnotationKey(entry.datetime, entry.pos0);
      if (std::find(storedKeys.begin(), storedKeys.end(), key) == storedKeys.end()) {
        storedKeys.push_back(key);
        touched++;
      } else {
        redelivered++;
      }
      ack.key = key;
      ack.datetime = entry.datetime;
      ack.pos0 = entry.pos0;
      acks.push_back(ack);
    }
    return touched;
  }

  size_t applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                      std::vector<bookorbit::DeletedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : deletes) {
      const auto it = std::find(storedKeys.begin(), storedKeys.end(), entry.key);
      if (it != storedKeys.end()) {
        storedKeys.erase(it);
        touched++;
      }
      acks.push_back({entry.serverId, false});
    }
    return touched;
  }
};

bookorbit::BookOrbitClient makeClient(ScriptedTransport& transport) {
  return bookorbit::BookOrbitClient(transport, "https://books.example.com/api/v1", "u", "k",
                                    {"crossink-abc123", "Xteink X4 Pro", "0.1.0"});
}

bookorbit::Annotation highlightAt(const int minute) {
  bookorbit::Annotation entry;
  char datetime[24];
  snprintf(datetime, sizeof(datetime), "2026-09-11 14:%02d:00", minute % 60);
  entry.datetime = datetime;
  entry.drawer = "lighten";
  entry.text = "a highlight";
  char pos[96];
  snprintf(pos, sizeof(pos), "/body[1]/DocFragment[3]/body[1]/p[%d]/text()[1].0", minute + 1);
  entry.pos0 = pos;
  entry.pos1 = pos;
  return entry;
}

std::vector<bookorbit::Annotation> highlights(const int count) {
  std::vector<bookorbit::Annotation> entries;
  entries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) entries.push_back(highlightAt(i));
  return entries;
}

std::string oneAddResponse(const bool more) {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[{"serverId":4711,"datetime":"2026-09-11 14:03:00","drawer":"lighten",)"
         R"("text":"from the web","posFormat":"xpointer",)"
         R"("pos0":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0",)"
         R"("pos1":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].9"}],"delete":[]},"more":)" +
         (more ? "true" : "false") + "}]}";
}

std::string emptyResponse() {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[],"delete":[]},"more":false}]})";
}

}  // namespace

using bookorbit::exchangeAnnotations;
using bookorbit::ExchangeOutcome;
using bookorbit::normalizeAnnotations;
using bookorbit::Status;

TEST(BookOrbitAnnotationSync, UnchangedBookSkipsTheExchangeEntirely) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(3));
  snprintf(book.annSignature, sizeof(book.annSignature), "%s", local.signature.c_str());
  book.annExchangedAt = kNow - 60u;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitAnnotationSync, ChangedBookUploadsInChunksOfFifty) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(120));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 3u);
  EXPECT_EQ(outcome.uploaded, 120u);
}

// The key set is the deletion-detection ground truth; it belongs on the first
// request only, and later chunks must not claim to be complete.
TEST(BookOrbitAnnotationSync, OnlyTheFirstRequestCarriesTheKeySet) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(60));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_EQ(transport.sent.size(), 2u);
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":true)"), std::string::npos);
  EXPECT_NE(transport.sent[0].body.find(R"("k":")"), std::string::npos);
  EXPECT_NE(transport.sent[1].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(transport.sent[1].body.find(R"("k":")"), std::string::npos);
}

// Over the 5000 cap, deletion detection degrades gracefully instead of letting
// the server delete entries it cannot see.
TEST(BookOrbitAnnotationSync, OverTheKeyCapSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(5001));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(transport.sent[0].body.find(R"("k":")"), std::string::npos);
  // An incomplete key set means the exchange was not authoritative, so the
  // book must not be stamped as skippable.
  EXPECT_EQ(book.annSignature[0], '\0');
}

// A highlight CrossInk cannot map to an xpointer is dropped by
// normalizeAnnotations, but it is still in the book. Claiming the surviving
// keys are the whole truth makes the server soft-delete the dropped ones --
// the device reporting a deletion the reader never made.
TEST(BookOrbitAnnotationSync, ADroppedHighlightSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  auto raw = highlights(4);
  raw[2].pos0 = "not an xpointer";  // ProgressMapper failing on one clipping
  const auto local = normalizeAnnotations(raw);
  ASSERT_EQ(local.entries.size(), 3u);
  ASSERT_FALSE(local.complete);

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  // Not authoritative, so the book stays unstamped and re-exchanges next time.
  EXPECT_EQ(book.annSignature[0], '\0');
}

// The whole-and-unmapped case: zero readable highlights must never go out as
// "the user deleted all of them".
TEST(BookOrbitAnnotationSync, AllHighlightsUnmappableSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  auto raw = highlights(3);
  for (auto& entry : raw) entry.pos0.clear();
  const auto local = normalizeAnnotations(raw);
  ASSERT_TRUE(local.entries.empty());
  ASSERT_FALSE(local.complete);

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
}

// Observed on hardware: 24 highlights on the device collapsed to 2 distinct
// keys, because every one carried the same boot-relative timestamp. The server
// deduped 24 down to 2, found the 8 identities it already knew missing from the
// set, and soft-deleted all 8. A colliding key set names fewer annotations than
// the device holds, so it can never be the deletion census.
TEST(BookOrbitAnnotationSync, CollidingKeysSendKeysCompleteFalse) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  auto raw = highlights(4);
  raw[3].datetime = raw[1].datetime;  // same stamp...
  raw[3].pos0 = raw[1].pos0;          // ...and the same coarse position
  const auto local = normalizeAnnotations(raw);
  ASSERT_EQ(local.entries.size(), 4u);
  ASSERT_TRUE(local.complete);  // nothing was dropped; the keys simply collide

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(book.annSignature[0], '\0');
}

// The ordinary case must stay authoritative: distinct keys still enable
// deletion detection, which is the whole point of sending them.
TEST(BookOrbitAnnotationSync, DistinctKeysStayAuthoritative) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(4));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":true)"), std::string::npos);
}

TEST(BookOrbitAnnotationSync, AppliesServerAddsAndAcknowledgesThem) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(outcome.applied, 1u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  ASSERT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 1u);
  EXPECT_NE(transport.sent[1].body.find(R"("serverId":4711)"), std::string::npos);
  EXPECT_NE(transport.sent[1].body.find(R"("status":"applied")"), std::string::npos);
}

TEST(BookOrbitAnnotationSync, CleanExchangeStampsTheSkipSignature) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(2));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_STREQ(book.annSignature, local.signature.c_str());
  EXPECT_EQ(book.annExchangedAt, kNow);
}

// THE CRASH-SAFETY CASE. The entries were applied locally but the ack never
// landed, so the server still owes them. Nothing may be stamped, and the next
// exchange must re-apply without duplicating.
TEST(BookOrbitAnnotationSync, AckFailureLeavesTheBookUnstamped) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {0, true, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.annSignature[0], '\0');
  EXPECT_EQ(book.annExchangedAt, 0u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
}

TEST(BookOrbitAnnotationSync, ReExchangeAfterALostAckDoesNotDuplicate) {
  ScriptedTransport first;
  first.queued = {{200, false, oneAddResponse(false)}, {0, true, ""}};
  auto firstClient = makeClient(first);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome lost;
  exchangeAnnotations(firstClient, book, kHash, local, applier, kNow, lost);
  ASSERT_EQ(applier.storedKeys.size(), 1u);

  // The server re-sends the same entry because it was never acknowledged.
  ScriptedTransport second;
  second.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto secondClient = makeClient(second);

  ExchangeOutcome retry;
  ASSERT_EQ(exchangeAnnotations(secondClient, book, kHash, local, applier, kNow + 5u, retry).status, Status::Ok);
  EXPECT_EQ(applier.storedKeys.size(), 1u) << "re-delivery created a duplicate annotation";
  EXPECT_EQ(applier.redelivered, 1u);
  EXPECT_STREQ(book.annSignature, local.signature.c_str());
}

TEST(BookOrbitAnnotationSync, KeepsPullingWhileTheServerReportsMore) {
  ScriptedTransport transport;
  transport.queued = {
      {200, false, oneAddResponse(true)}, {200, false, "{}"}, {200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 2u);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 2u);
}

// A server stuck on more:true must not pin the reader in a loop.
TEST(BookOrbitAnnotationSync, StopsAfterTenPullRounds) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, oneAddResponse(true)}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 10u);
  // The pull did not complete, so the book stays mandatory next time.
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, UnmatchedBookAbortsWithoutStamping) {
  ScriptedTransport transport;
  transport.queued = {{200, false, std::string(R"({"unmatched":[")") + kHash + R"("],"results":[]})"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_TRUE(outcome.unmatched);
  EXPECT_EQ(book.annSignature[0], '\0');
  EXPECT_EQ(transport.sent.size(), 1u);
}

TEST(BookOrbitAnnotationSync, AuthErrorAbortsImmediately) {
  ScriptedTransport transport;
  transport.queued = {{401, false, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Unauthorized);
  EXPECT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, ServerErrorLeavesTheWatermarkUnadvanced) {
  ScriptedTransport transport;
  transport.queued = {{503, false, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::ServerError);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.annSignature[0], '\0');
}

// A failed apply means the server still owes us the entry; stamping would hide
// it until the 6-hour bound expired.
TEST(BookOrbitAnnotationSync, FailedApplyPreventsStamping) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  applier.failEverything = true;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  EXPECT_EQ(outcome.failed, 1u);
  EXPECT_NE(transport.sent[1].body.find(R"("status":"failed")"), std::string::npos);
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, MalformedResponseIsAnError) {
  ScriptedTransport transport;
  transport.queued = {{200, false, "{not json"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::InvalidJson);
  EXPECT_EQ(book.annSignature[0], '\0');
}

// An empty local set still exchanges once: it is the only way server-created
// highlights reach the device.
TEST(BookOrbitAnnotationSync, EmptyLocalSetStillExchangesOnce) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, normalizeAnnotations({}), applier, kNow, outcome).status,
            Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 1u);
}
