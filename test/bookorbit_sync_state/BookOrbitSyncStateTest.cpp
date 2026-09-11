#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitSyncState.h"
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

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";

}  // namespace

using bookorbit::SyncStateStore;

TEST(BookOrbitSyncState, FindReturnsNullForUnknownBook) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, FindOrCreateStartsZeroed) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  const auto& book = state.findOrCreate(kMd5);
  EXPECT_EQ(book.statsWatermark, 0u);
  EXPECT_EQ(book.matchVerifiedAt, 0u);
  EXPECT_EQ(book.bookId, 0u);
}

TEST(BookOrbitSyncState, RoundTripsThroughFlushAndLoad) {
  FakeBlobStore store;
  {
    SyncStateStore state(store, "/bookorbit_state.bin");
    auto& book = state.findOrCreate(kMd5);
    book.statsWatermark = 1787407272u;
    book.matchVerifiedAt = 1787561449u;
    book.bookId = 11u;
    book.fileId = 11u;
    book.progressPushedPct = 0.2052f;
    state.setLibraryVersion("5e52cee3c2f603bf");
    ASSERT_TRUE(state.flush());
  }

  SyncStateStore reloaded(store, "/bookorbit_state.bin");
  ASSERT_TRUE(reloaded.load());
  const auto* book = reloaded.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->statsWatermark, 1787407272u);
  EXPECT_EQ(book->matchVerifiedAt, 1787561449u);
  EXPECT_EQ(book->bookId, 11u);
  EXPECT_FLOAT_EQ(book->progressPushedPct, 0.2052f);
  EXPECT_EQ(reloaded.libraryVersion(), "5e52cee3c2f603bf");
}

TEST(BookOrbitSyncState, LoadOnMissingFileSucceedsEmpty) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_TRUE(state.load());
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, CorruptBlobLoadsEmptyRatherThanCrashing) {
  FakeBlobStore store;
  store.files["/bookorbit_state.bin"] = {0xFF, 0xFF, 0x01};
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_TRUE(state.load());
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, MultipleBooksPersistIndependently) {
  FakeBlobStore store;
  constexpr char kOther[] = "6fba8d1c39745a4fe79813f76dbb314a";
  {
    SyncStateStore state(store, "/bookorbit_state.bin");
    state.findOrCreate(kMd5).statsWatermark = 100u;
    state.findOrCreate(kOther).statsWatermark = 200u;
    ASSERT_TRUE(state.flush());
  }
  SyncStateStore reloaded(store, "/bookorbit_state.bin");
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->statsWatermark, 100u);
  EXPECT_EQ(reloaded.find(kOther)->statsWatermark, 200u);
}

TEST(BookOrbitSyncState, MatchIsFreshInsideTwentyFourHours) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  EXPECT_TRUE(state.isMatchFresh(book, 1000u + 86399u));
}

TEST(BookOrbitSyncState, MatchExpiresAtTwentyFourHours) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  EXPECT_FALSE(state.isMatchFresh(book, 1000u + 86400u));
}

// A changed library version invalidates every cached match immediately.
TEST(BookOrbitSyncState, LibraryVersionChangeInvalidatesMatch) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  state.setLibraryVersion("v2");
  EXPECT_FALSE(state.isMatchFresh(book, 1001u));
}

TEST(BookOrbitSyncState, NeverMatchedIsNotFresh) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  const auto& book = state.findOrCreate(kMd5);
  EXPECT_FALSE(state.isMatchFresh(book, 1000u));
}
