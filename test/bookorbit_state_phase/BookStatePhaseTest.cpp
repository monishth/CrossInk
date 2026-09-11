#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/BookStatePhase.h"
#include "lib/BookOrbit/IBlobStore.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class FakeBlobStore : public bookorbit::IBlobStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;

  bool read(const std::string_view path, std::vector<uint8_t>& out) override {
    const auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool write(const std::string_view path, const uint8_t* data, const size_t len) override {
    files[std::string(path)] = std::vector<uint8_t>(data, data + len);
    return true;
  }

  bool append(const std::string_view path, const uint8_t* data, const size_t len) override {
    auto& file = files[std::string(path)];
    file.insert(file.end(), data, data + len);
    return true;
  }
  bool rename(const std::string_view from, const std::string_view to) override {
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  bookorbit::HttpResponse next{200, false, R"({"unmatched":[],"results":[]})"};

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    return next;
  }
};

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr uint32_t kNow = 1787561449u;

struct Fixture {
  FakeBlobStore blobs;
  ScriptedTransport transport;
  bookorbit::SyncStateStore syncState{blobs, "/bookorbit_state.bin"};
  bookorbit::BookStateStore stateStore{blobs, "/bookorbit_states.bin"};
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "monish",
                                    "5f4dcc3b5aa765d61d8327deb882cf99",
                                    bookorbit::DeviceIdentity{"crossink-abc123", "Xteink X4 Pro", "0.1.0"}};
  bookorbit::BookStatePhase phase{client, syncState, stateStore};
};

}  // namespace

using bookorbit::BookStatus;
using bookorbit::StatePhaseOutcome;

TEST(BookStatePhase, UnchangedBookSendsNothing) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow);
  auto& book = fx.syncState.findOrCreate(kMd5);
  std::snprintf(book.statusSyncedModified, sizeof(book.statusSyncedModified), "2026-08-21");

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Skipped);
  EXPECT_TRUE(fx.transport.sent.empty());
}

TEST(BookStatePhase, PostsToTheBookStatesPath) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  ASSERT_EQ(fx.transport.sent.size(), 1u);
  EXPECT_EQ(fx.transport.sent[0].method, "POST");
  EXPECT_EQ(fx.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/book-states");
  EXPECT_NE(fx.transport.sent[0].body.find(R"("rating":4)"), std::string::npos);
}

// withDevice injection is the client's job, but the phase must go through it.
TEST(BookStatePhase, RequestCarriesDeviceFields) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const std::string& body = fx.transport.sent[0].body;
  EXPECT_NE(body.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  EXPECT_NE(body.find(R"("deviceModel":"Xteink X4 Pro")"), std::string::npos);
  EXPECT_NE(body.find(R"("pluginVersion":"0.1.0")"), std::string::npos);
  EXPECT_NE(body.find(R"("deviceTime":)"), std::string::npos);
}

TEST(BookStatePhase, ForcedPullSendsABareHash) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow);

  ASSERT_EQ(fx.phase.run(kMd5, true, kNow), StatePhaseOutcome::Synced);
  ASSERT_EQ(fx.transport.sent.size(), 1u);
  EXPECT_NE(fx.transport.sent[0].body.find(std::string(R"("hash":")") + kMd5 + '"'), std::string::npos);
  EXPECT_EQ(fx.transport.sent[0].body.find(R"("status":)"), std::string::npos);
}

TEST(BookStatePhase, StalePullAgeForcesARequestWithoutAnyChange) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow - 86401u);

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  EXPECT_EQ(fx.transport.sent.size(), 1u);
}

TEST(BookStatePhase, ServerRatingIsAppliedLocally) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(2, day("2026-08-20"));
  fx.transport.next = {200, false,
                       R"({"unmatched":[],"results":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f",)"
                       R"("ratingSet":true,"rating":5,"ratingUpdatedAt":"2026-08-21"}]})"};

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* record = fx.stateStore.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->local.rating, 5);
  EXPECT_TRUE(record->synced.ratingKnown);
  EXPECT_EQ(record->synced.rating, 5);
}

TEST(BookStatePhase, SyncedShadowRecordsWhatWasUploaded) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* record = fx.stateStore.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->synced.ratingKnown);
  EXPECT_TRUE(record->synced.ratingSet);
  EXPECT_EQ(record->synced.rating, 4);
}

// A server-kept tie still counts as synced: the device value was considered.
TEST(BookStatePhase, StatusWatermarkAdvancesOnSuccess) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* book = fx.syncState.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->statusSyncedModified, "2026-08-21");
  EXPECT_EQ(fx.stateStore.find(kMd5)->synced.statusSyncedModified.day, 21);
}

TEST(BookStatePhase, UnmatchedBookDoesNotAdvanceTheWatermark) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {200, false, R"({"unmatched":["0f0a792b00a37cf80baa5e50c078b31f"],"results":[]})"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Unmatched);
  const auto* book = fx.syncState.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->statusSyncedModified, "");
}

TEST(BookStatePhase, ServerErrorLeavesTheWatermarkUnadvanced) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {503, false, "upstream unavailable"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

TEST(BookStatePhase, AuthErrorIsReportedDistinctly) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {401, false, "unauthorized"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::AuthFailed);
}

TEST(BookStatePhase, TransportFailureIsAFailureNotACrash) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {0, true, ""};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

TEST(BookStatePhase, MalformedResponseFails) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {200, false, "{not json"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

// The phase must be durable before annotations run: a crash after the POST but
// before the flush would otherwise re-upload, and re-uploads are idempotent,
// but a lost pull would be silently dropped.
TEST(BookStatePhase, SuccessIsPersistedBeforeReturning) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));
  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);

  bookorbit::BookStateStore reloaded(fx.blobs, "/bookorbit_states.bin");
  ASSERT_TRUE(reloaded.load());
  ASSERT_NE(reloaded.find(kMd5), nullptr);
  EXPECT_EQ(reloaded.find(kMd5)->synced.rating, 4);

  bookorbit::SyncStateStore reloadedSync(fx.blobs, "/bookorbit_state.bin");
  ASSERT_TRUE(reloadedSync.load());
  ASSERT_NE(reloadedSync.find(kMd5), nullptr);
  EXPECT_STREQ(reloadedSync.find(kMd5)->statusSyncedModified, "2026-08-21");
}
