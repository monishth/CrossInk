#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

// In-memory IFileSink with fault injection, so publish-safety can be tested
// without a filesystem.
class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  bool failOpen = false;
  bool failWrite = false;
  bool failPublish = false;
  int closes = 0;

  bool open(const std::string_view path) override {
    if (failOpen) return false;
    openPath.assign(path);
    files[openPath] = "";  // truncates any leftover of the same name
    isOpen = true;
    return true;
  }

  bool write(const uint8_t* data, const size_t len) override {
    if (!isOpen || failWrite) return false;
    files[openPath].append(reinterpret_cast<const char*>(data), len);
    return true;
  }

  bool close() override {
    closes++;
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

bool feed(bookorbit::PartFileWriter& writer, const std::string& payload) {
  return writer.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

}  // namespace

using bookorbit::kMaxTransferBytes;
using bookorbit::maxBytesForExpected;
using bookorbit::PartFileWriter;
using bookorbit::partPathFor;

TEST(PartPath, AppendsPartSuffix) {
  EXPECT_EQ(partPathFor("/books/we-solve-murders.epub"), "/books/we-solve-murders.epub.part");
}

TEST(MaxBytesForExpected, UnknownSizeUsesTheHardCeiling) { EXPECT_EQ(maxBytesForExpected(0), kMaxTransferBytes); }

TEST(MaxBytesForExpected, AddsMarginAndSlackToAKnownSize) {
  // 1,000,000 * 1.25 = 1,250,000, plus 1 MiB of slack for container overhead.
  EXPECT_EQ(maxBytesForExpected(1000000), 1250000u + 1024u * 1024u);
}

TEST(MaxBytesForExpected, NeverExceedsTheHardCeiling) {
  EXPECT_EQ(maxBytesForExpected(kMaxTransferBytes), kMaxTransferBytes);
}

TEST(PartFileWriter, WritesToThePartPathNotTheFinalPath) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  // 11 bytes: "PK" + 0x03 + 0x04 + "payload". Derived from the literal rather
  // than hard-coded, so the count cannot drift from the payload again.
  const std::string payload("PK\x03\x04payload");
  ASSERT_TRUE(feed(writer, payload));

  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_EQ(writer.bytes(), payload.size());
}

TEST(PartFileWriter, CommitPublishesAtomicallyAndRemovesThePart) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "hello"));
  ASSERT_TRUE(writer.commit());

  EXPECT_TRUE(sink.exists("/books/a.epub"));
  EXPECT_FALSE(sink.exists("/books/a.epub.part"));
  EXPECT_EQ(sink.files["/books/a.epub"], "hello");
  EXPECT_EQ(sink.closes, 1);
}

// The interrupted-download guarantee: whatever happened, a half-transferred
// book is only ever visible as a ".part" file. A .part must never be mistaken
// for a complete book.
TEST(PartFileWriter, AbandonLeavesOnlyThePartFile) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "half a book"));
  writer.abandon();

  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_EQ(sink.closes, 1);
}

TEST(PartFileWriter, AbandonedTransferCannotBeCommittedAfterwards) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "half a book"));
  writer.abandon();

  EXPECT_FALSE(writer.commit());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

// A leftover .part from a previous power loss is truncated, never appended to.
TEST(PartFileWriter, StalePartFromAPreviousRunIsDiscarded) {
  FakeFileSink sink;
  sink.files["/books/a.epub.part"] = "garbage from a battery pull";

  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "fresh"));
  ASSERT_TRUE(writer.commit());
  EXPECT_EQ(sink.files["/books/a.epub"], "fresh");
}

TEST(PartFileWriter, ExceedingTheByteCapAbortsAndPublishesNothing) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 8);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "12345678"));
  EXPECT_FALSE(feed(writer, "9"));

  EXPECT_TRUE(writer.capExceeded());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_FALSE(writer.commit());
}

// The cap is checked before the write, so not one byte past it reaches the card.
TEST(PartFileWriter, NoBytesPastTheCapAreWritten) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 4);
  ASSERT_TRUE(writer.begin());
  EXPECT_FALSE(feed(writer, "123456"));
  EXPECT_EQ(sink.files["/books/a.epub.part"], "");
  EXPECT_EQ(writer.bytes(), 0u);
}

TEST(PartFileWriter, WriteFailureAbortsWithoutPublishing) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  sink.failWrite = true;
  EXPECT_FALSE(feed(writer, "hello"));
  EXPECT_FALSE(writer.commit());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

TEST(PartFileWriter, OpenFailureIsReportedByBegin) {
  FakeFileSink sink;
  sink.failOpen = true;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  EXPECT_FALSE(writer.begin());
}

TEST(PartFileWriter, PublishFailureLeavesThePartInPlace) {
  FakeFileSink sink;
  sink.failPublish = true;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "hello"));

  EXPECT_FALSE(writer.commit());
  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

TEST(PartFileWriter, ZeroByteResponseStillPublishes) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(writer.commit());
  EXPECT_TRUE(sink.exists("/books/a.epub"));
  EXPECT_EQ(writer.bytes(), 0u);
}

TEST(PartFileWriter, DataBeforeBeginIsRejected) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  EXPECT_FALSE(feed(writer, "hello"));
}
