#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBookmarkSync.h"
#include "lib/BookOrbit/BookOrbitCapabilities.h"
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

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (cursor < queued.size()) return queued[cursor++];
    return {200, false, R"({"unmatched":[],"results":[]})"};
  }

 private:
  size_t cursor = 0;
};

class RecordingApplier : public bookorbit::IAnnotationApplier {
 public:
  std::vector<std::string> storedKeys;

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds, std::vector<bookorbit::AppliedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : adds) {
      const std::string key = bookorbit::buildBookmarkKey(entry.datetime, entry.pos0);
      if (std::find(storedKeys.begin(), storedKeys.end(), key) == storedKeys.end()) {
        storedKeys.push_back(key);
        touched++;
      }
      bookorbit::AppliedAck ack;
      ack.serverId = entry.serverId;
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

std::vector<bookorbit::Bookmark> dogears(const int count) {
  std::vector<bookorbit::Bookmark> entries;
  entries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    bookorbit::Bookmark entry;
    char datetime[24];
    snprintf(datetime, sizeof(datetime), "2026-09-01 08:%02d:00", i % 60);
    entry.datetime = datetime;
    char pos[96];
    snprintf(pos, sizeof(pos), "/body[1]/DocFragment[5]/body[1]/p[%d]/text()[1].0", i + 1);
    entry.pos = pos;
    entries.push_back(entry);
  }
  return entries;
}

std::string emptyResponse() {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[],"delete":[]},"more":false}]})";
}

}  // namespace

using bookorbit::Capability;
using bookorbit::CapabilityCache;
using bookorbit::exchangeBookmarks;
using bookorbit::ExchangeOutcome;
using bookorbit::kBookmarkCapability;
using bookorbit::normalizeBookmarks;
using bookorbit::Status;

TEST(BookOrbitBookmarkSync, ConfirmedUnsupportedServerIsNeverCalled) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  capabilities.markUnsupported(kBookmarkCapability);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(2)), applier, kNow, outcome)
                .status,
            Status::Ok);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

// Unknown is not a negative. A server we have not asked yet still gets tried.
TEST(BookOrbitBookmarkSync, UnknownCapabilityStillAttemptsTheExchange) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  ASSERT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome);
  EXPECT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/bookmarks/exchange");
}

TEST(BookOrbitBookmarkSync, ConfirmedNotFoundDowngradesTheCapability) {
  ScriptedTransport transport;
  transport.queued = {{404, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
                .status,
            Status::NotFound);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unsupported);
}

// THE TRI-STATE INVARIANT. One 500 must never permanently disable bookmark
// sync.
TEST(BookOrbitBookmarkSync, ServerErrorLeavesTheCapabilityUnknown) {
  ScriptedTransport transport;
  transport.queued = {{500, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
                .status,
            Status::ServerError);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, TransportFailureLeavesTheCapabilityUnknown) {
  ScriptedTransport transport;
  transport.queued = {{0, true, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
                .status,
            Status::Transport);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, UnchangedBookSkipsTheExchange) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeBookmarks(dogears(2));
  snprintf(book.bmSignature, sizeof(book.bmSignature), "%s", local.signature.c_str());
  book.bmExchangedAt = kNow - 60u;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, local, applier, kNow, outcome);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitBookmarkSync, UploadsInChunksOfFifty) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(60)), applier, kNow, outcome);
  EXPECT_EQ(transport.sent.size(), 2u);
  EXPECT_EQ(outcome.uploaded, 60u);
}

// The bookmark key cap is 500, not the annotation route's 5000.
TEST(BookOrbitBookmarkSync, OverFiveHundredKeysSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(501)), applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(book.bmSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, FiveHundredKeysStillSendsTheCompleteSet) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(500)), applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":true)"), std::string::npos);
}

TEST(BookOrbitBookmarkSync, AppliesAddsAndAcknowledgesOnTheBookmarkRoute) {
  ScriptedTransport transport;
  transport.queued = {{200, false,
                       std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
                           R"(","toApply":{"add":[{"serverId":5,"datetime":"2026-09-02 10:00:00",)"
                           R"("pos":"/body[1]/DocFragment[7]/body[1]/p[1]/text()[1].0","title":"Chapter 7"}],)"
                           R"("delete":[]},"more":false}]})"},
                      {200, false, "{}"}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks({}), applier, kNow, outcome).status,
            Status::Ok);
  EXPECT_EQ(outcome.applied, 1u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  EXPECT_EQ(transport.sent[1].url, "https://books.example.com/api/v1/koreader/plugin/bookmarks/exchange-ack");
}

TEST(BookOrbitBookmarkSync, CleanExchangeStampsTheBookmarkSignature) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeBookmarks(dogears(2));

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, local, applier, kNow, outcome);
  EXPECT_STREQ(book.bmSignature, local.signature.c_str());
  EXPECT_EQ(book.bmExchangedAt, kNow);
  // The annotation stamp is a separate field and must be untouched.
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, LostAckLeavesTheBookmarkStampUnwritten) {
  ScriptedTransport transport;
  transport.queued = {{200, false,
                       std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
                           R"(","toApply":{"add":[{"serverId":5,"datetime":"2026-09-02 10:00:00",)"
                           R"("pos":"/body[1]/DocFragment[7]/body[1]/p[1]/text()[1].0"}],"delete":[]},)"
                           R"("more":false}]})"},
                      {0, true, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks({}), applier, kNow, outcome);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.bmSignature[0], '\0');
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  // A lost ack is a transport failure, not a statement about support.
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, UnmatchedBookAborts) {
  ScriptedTransport transport;
  transport.queued = {{200, false, std::string(R"({"unmatched":[")") + kHash + R"("],"results":[]})"}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome);
  EXPECT_TRUE(outcome.unmatched);
  EXPECT_EQ(book.bmSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, AuthErrorAbortsAndDoesNotTouchTheCapability) {
  ScriptedTransport transport;
  transport.queued = {{403, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
                .status,
            Status::Unauthorized);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}
