#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/CatalogSyncKey.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  bool failPublish = false;

  bool open(const std::string_view path) override {
    openPath.assign(path);
    files[openPath] = "";
    isOpen = true;
    return true;
  }
  bool write(const uint8_t* data, const size_t len) override {
    if (!isOpen) return false;
    files[openPath].append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  bool close() override {
    isOpen = false;
    return true;
  }
  bool publish(const std::string_view from, const std::string_view to) override {
    if (failPublish) return false;
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

// Records which path it was asked to hash, so the test can prove the finished
// file is hashed and not the .part.
class RecordingHasher : public bookorbit::IDocumentHasher {
 public:
  std::vector<std::string> hashed;
  std::string result = "0f0a792b00a37cf80baa5e50c078b31f";

  std::string partialMd5(const std::string_view path) override {
    hashed.emplace_back(path);
    return result;
  }
};

bookorbit::ManifestItem item() {
  bookorbit::ManifestItem entry;
  entry.bookId = 41;
  entry.fileId = 902;
  entry.fileBytes = 5;
  entry.filename = "we-solve-murders.epub";
  return entry;
}

bookorbit::PartFileWriter completedWriter(FakeFileSink& sink) {
  bookorbit::PartFileWriter writer(sink, "/books/we-solve-murders.epub", 1024);
  writer.begin();
  const std::string payload = "hello";
  writer.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  return writer;
}

}  // namespace

using bookorbit::DownloadedBook;
using bookorbit::finalizeDownload;
using bookorbit::isPartialMd5;

TEST(PartialMd5Shape, AcceptsThirtyTwoLowercaseHexDigits) {
  EXPECT_TRUE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31f"));
}

TEST(PartialMd5Shape, RejectsWrongLengthUppercaseAndNonHex) {
  EXPECT_FALSE(isPartialMd5(""));
  EXPECT_FALSE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31"));
  EXPECT_FALSE(isPartialMd5("0F0A792B00A37CF80BAA5E50C078B31F"));
  EXPECT_FALSE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31z"));
}

// The core guarantee: the hash is taken from the published file, not the .part.
TEST(FinalizeDownload, HashesThePublishedFileNotThePart) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, item(), book));
  ASSERT_EQ(hasher.hashed.size(), 1u);
  EXPECT_EQ(hasher.hashed[0], "/books/we-solve-murders.epub");
}

TEST(FinalizeDownload, ProducesTheSyncKeyAndIdentity) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_EQ(book.hash, "0f0a792b00a37cf80baa5e50c078b31f");
  EXPECT_EQ(book.path, "/books/we-solve-murders.epub");
  EXPECT_EQ(book.bookId, 41u);
  EXPECT_EQ(book.fileId, 902u);
  EXPECT_EQ(book.bytes, 5u);
  EXPECT_TRUE(sink.exists("/books/we-solve-murders.epub"));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub.part"));
}

// An unhashable file cannot be synced by any later phase, so it is removed
// rather than left as a book the device can never identify.
TEST(FinalizeDownload, UnhashableFileIsRemovedAndReportedAsFailure) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "";

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
  EXPECT_TRUE(book.hash.empty());
}

TEST(FinalizeDownload, MalformedHashIsTreatedAsUnhashable) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "not-a-hash";

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
}

// An interrupted transfer must never be hashed: it is not a book.
TEST(FinalizeDownload, AbandonedTransferIsNeverHashed) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  writer.abandon();
  RecordingHasher hasher;

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_TRUE(hasher.hashed.empty());
  EXPECT_TRUE(sink.exists("/books/we-solve-murders.epub.part"));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
}

TEST(FinalizeDownload, PublishFailureIsNeverHashed) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  sink.failPublish = true;
  RecordingHasher hasher;

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_TRUE(hasher.hashed.empty());
}

// The device-computed hash is authoritative. A server hash that disagrees is
// recorded for diagnosis but never substituted: every other phase keys on what
// KOReaderDocumentId produced from the bytes actually on this card.
TEST(FinalizeDownload, DeviceHashWinsOverAServerSuppliedHash) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "1111111111111111111111111111aaaa";

  auto entry = item();
  entry.hash = "2222222222222222222222222222bbbb";

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, entry, book));
  EXPECT_EQ(book.hash, "1111111111111111111111111111aaaa");
  EXPECT_TRUE(book.serverHashMismatch);
}

TEST(FinalizeDownload, MatchingServerHashIsNotFlagged) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  auto entry = item();
  entry.hash = "0f0a792b00a37cf80baa5e50c078b31f";

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, entry, book));
  EXPECT_FALSE(book.serverHashMismatch);
}
