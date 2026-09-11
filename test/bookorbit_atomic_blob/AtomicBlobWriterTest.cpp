#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/AtomicBlobWriter.h"
#include "lib/BookOrbit/IBlobStore.h"

namespace {

// In-memory IBlobStore with a fault-injection hook, so the crash-safety
// guarantee can be tested without a filesystem.
class FakeBlobStore : public bookorbit::IBlobStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  int failWriteAfter = -1;  // fail the Nth write (0-based); -1 disables
  int failRenameAfter = -1;
  int writes = 0;
  int renames = 0;

  bool read(const std::string_view path, std::vector<uint8_t>& out) override {
    const auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }

  bool write(const std::string_view path, const uint8_t* data, const size_t len) override {
    if (failWriteAfter >= 0 && writes == failWriteAfter) {
      writes++;
      return false;
    }
    writes++;
    files[std::string(path)] = std::vector<uint8_t>(data, data + len);
    return true;
  }

  bool append(const std::string_view path, const uint8_t* data, const size_t len) override {
    auto& file = files[std::string(path)];
    file.insert(file.end(), data, data + len);
    return true;
  }

  bool rename(const std::string_view from, const std::string_view to) override {
    if (failRenameAfter >= 0 && renames == failRenameAfter) {
      renames++;
      return false;
    }
    renames++;
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }

  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }

  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST(AtomicBlobWriter, WritesPayloadToFinalPath) {
  FakeBlobStore store;
  const auto payload = bytes("hello");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", payload.data(), payload.size()));
  EXPECT_EQ(store.files["/state.bin"], payload);
}

TEST(AtomicBlobWriter, LeavesNoTempFileBehind) {
  FakeBlobStore store;
  const auto payload = bytes("hello");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", payload.data(), payload.size()));
  EXPECT_FALSE(store.exists("/state.bin.tmp"));
}

TEST(AtomicBlobWriter, RotatesPreviousVersionToBackup) {
  FakeBlobStore store;
  const auto first = bytes("first");
  const auto second = bytes("second");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", first.data(), first.size()));
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", second.data(), second.size()));
  EXPECT_EQ(store.files["/state.bin"], second);
  EXPECT_EQ(store.files["/state.bin.bak"], first);
}

// The whole point: a failed write must never damage the existing good file.
TEST(AtomicBlobWriter, FailedTempWriteLeavesOriginalIntact) {
  FakeBlobStore store;
  const auto good = bytes("good");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", good.data(), good.size()));

  store.failWriteAfter = store.writes;  // fail the next write
  const auto bad = bytes("bad");
  EXPECT_FALSE(bookorbit::atomicWriteBlob(store, "/state.bin", bad.data(), bad.size()));
  EXPECT_EQ(store.files["/state.bin"], good);
}

TEST(AtomicBlobWriter, ReadsBackupWhenPrimaryMissing) {
  FakeBlobStore store;
  store.files["/state.bin.bak"] = bytes("recovered");
  std::vector<uint8_t> out;
  ASSERT_TRUE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
  EXPECT_EQ(out, bytes("recovered"));
}

TEST(AtomicBlobWriter, PrefersPrimaryOverBackup) {
  FakeBlobStore store;
  store.files["/state.bin"] = bytes("primary");
  store.files["/state.bin.bak"] = bytes("backup");
  std::vector<uint8_t> out;
  ASSERT_TRUE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
  EXPECT_EQ(out, bytes("primary"));
}

TEST(AtomicBlobWriter, ReportsFailureWhenNeitherExists) {
  FakeBlobStore store;
  std::vector<uint8_t> out;
  EXPECT_FALSE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
}
