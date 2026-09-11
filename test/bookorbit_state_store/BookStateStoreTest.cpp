#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateStore.h"
#include "lib/BookOrbit/IBlobStore.h"

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

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr char kPath[] = "/bookorbit_states.bin";

}  // namespace

using bookorbit::BookStateStore;
using bookorbit::BookStatus;

TEST(BookStateStore, FindReturnsNullForUnknownBook) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, FindOrCreateStartsEmpty) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  const auto& record = store.findOrCreate(kMd5);
  EXPECT_FALSE(record.local.statusKnown);
  EXPECT_FALSE(record.local.ratingSet);
  EXPECT_FALSE(record.synced.ratingKnown);
  EXPECT_EQ(record.statePulledAt, 0u);
}

TEST(BookStateStore, RoundTripsThroughFlushAndLoad) {
  FakeBlobStore blobs;
  {
    BookStateStore store(blobs, kPath);
    auto& record = store.findOrCreate(kMd5);
    record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
    record.local.setRating(4, day("2026-08-21"));
    record.local.setReview("Great fun.", day("2026-08-22"));
    record.synced.ratingKnown = true;
    record.synced.ratingSet = true;
    record.synced.rating = 4;
    record.synced.reviewKnown = true;
    record.synced.reviewSet = true;
    record.synced.reviewNote = "Great fun.";
    record.synced.statusSyncedModified = day("2026-08-21");
    record.statePulledAt = 1787561449u;
    ASSERT_TRUE(store.flush());
  }

  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  const auto* record = reloaded.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->local.status, BookStatus::Complete);
  EXPECT_EQ(record->local.rating, 4);
  EXPECT_EQ(record->local.reviewNote, "Great fun.");
  EXPECT_EQ(record->local.reviewModified.day, 22);
  EXPECT_TRUE(record->synced.reviewSet);
  EXPECT_EQ(record->synced.statusSyncedModified.day, 21);
  EXPECT_EQ(record->statePulledAt, 1787561449u);
}

TEST(BookStateStore, LoadOnMissingFileSucceedsEmpty) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.load());
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, CorruptBlobLoadsEmptyRatherThanCrashing) {
  FakeBlobStore blobs;
  blobs.files[kPath] = {0xFF, 0xFF, 0x01};
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.load());
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, MultipleBooksPersistIndependently) {
  FakeBlobStore blobs;
  constexpr char kOther[] = "6fba8d1c39745a4fe79813f76dbb314a";
  {
    BookStateStore store(blobs, kPath);
    store.findOrCreate(kMd5).local.setRating(1, day("2026-08-21"));
    store.findOrCreate(kOther).local.setRating(5, day("2026-08-22"));
    ASSERT_TRUE(store.flush());
  }
  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->local.rating, 1);
  EXPECT_EQ(reloaded.find(kOther)->local.rating, 5);
}

TEST(BookStateStore, LongReviewIsTruncatedOnPersist) {
  FakeBlobStore blobs;
  const std::string huge(bookorbit::kReviewNoteMaxBytes + 100, 'x');
  {
    BookStateStore store(blobs, kPath);
    store.findOrCreate(kMd5).local.setReview(huge, day("2026-08-21"));
    ASSERT_TRUE(store.flush());
  }
  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->local.reviewNote.size(), bookorbit::kReviewNoteMaxBytes);
}

TEST(BookStateStore, NeverPulledNeedsAPull) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.needsStatePull(store.findOrCreate(kMd5), 1000u));
}

TEST(BookStateStore, RecentPullDoesNotNeedAnother) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 1000u);
  EXPECT_FALSE(store.needsStatePull(record, 1000u + 86400u));
}

TEST(BookStateStore, PullExpiresAfterTwentyFourHours) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 1000u);
  EXPECT_TRUE(store.needsStatePull(record, 1000u + 86401u));
}

// A clock that jumped backwards must force a pull, not silence one forever.
TEST(BookStateStore, ClockGoingBackwardsForcesAPull) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 5000u);
  EXPECT_TRUE(store.needsStatePull(record, 1000u));
}
