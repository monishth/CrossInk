#include <gtest/gtest.h>

#include <map>
#include <string>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/CatalogThumbnail.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  int opens = 0;

  bool open(const std::string_view path) override {
    opens++;
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
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

bool feed(bookorbit::ThumbnailGate& gate, const std::string& payload) {
  return gate.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

}  // namespace

using bookorbit::isImageContentType;
using bookorbit::PartFileWriter;
using bookorbit::ThumbnailGate;
using bookorbit::thumbnailPath;

TEST(ImageContentType, AcceptsAnyImageSubtype) {
  EXPECT_TRUE(isImageContentType("image/jpeg"));
  EXPECT_TRUE(isImageContentType("image/png"));
  EXPECT_TRUE(isImageContentType("image/webp"));
}

TEST(ImageContentType, IsCaseInsensitive) {
  EXPECT_TRUE(isImageContentType("IMAGE/JPEG"));
  EXPECT_TRUE(isImageContentType("Image/Png"));
}

TEST(ImageContentType, IgnoresParametersAndLeadingSpace) {
  EXPECT_TRUE(isImageContentType("image/jpeg; charset=binary"));
  EXPECT_TRUE(isImageContentType("  image/jpeg"));
}

TEST(ImageContentType, RejectsEverythingElse) {
  EXPECT_FALSE(isImageContentType("text/html"));
  EXPECT_FALSE(isImageContentType("application/json"));
  EXPECT_FALSE(isImageContentType("application/octet-stream"));
  EXPECT_FALSE(isImageContentType(""));
  // A subtype-only prefix match must not pass: "imagex/..." is not an image.
  EXPECT_FALSE(isImageContentType("imagex/jpeg"));
}

TEST(ThumbnailPath, IsDerivedFromTheBookId) { EXPECT_EQ(thumbnailPath(41), "/.crosspoint/bookorbit/thumbs/41.jpg"); }

TEST(ThumbnailGate, ImageResponseWritesAndPublishes) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  ASSERT_TRUE(gate.onHeaders("image/jpeg"));
  ASSERT_TRUE(feed(gate, "\xff\xd8\xff\xe0jpegbytes"));
  ASSERT_TRUE(writer.commit());
  EXPECT_TRUE(sink.exists("/.crosspoint/bookorbit/thumbs/41.jpg"));
  EXPECT_FALSE(gate.rejected());
}

// The guard: a non-image response is refused before anything is opened, so no
// .part is ever created for it.
TEST(ThumbnailGate, NonImageResponseIsRefusedBeforeAnyFileIsOpened) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(gate.onHeaders("text/html; charset=utf-8"));
  EXPECT_TRUE(gate.rejected());
  EXPECT_EQ(sink.opens, 0);
  EXPECT_TRUE(sink.files.empty());
}

TEST(ThumbnailGate, DataAfterRejectionIsDiscarded) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  ASSERT_FALSE(gate.onHeaders("application/json"));
  EXPECT_FALSE(feed(gate, R"({"error":"unauthorized"})"));
  EXPECT_TRUE(sink.files.empty());
  EXPECT_FALSE(writer.commit());
}

TEST(ThumbnailGate, MissingContentTypeIsRefused) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(gate.onHeaders(""));
  EXPECT_TRUE(gate.rejected());
  EXPECT_EQ(sink.opens, 0);
}

TEST(ThumbnailGate, DataBeforeHeadersIsRefused) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(feed(gate, "\xff\xd8"));
  EXPECT_EQ(sink.opens, 0);
}

// An oversized cover is still capped, and still leaves nothing published.
TEST(ThumbnailGate, OversizedThumbnailIsCapped) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), 16);
  ThumbnailGate gate(writer);

  ASSERT_TRUE(gate.onHeaders("image/png"));
  EXPECT_FALSE(feed(gate, std::string(17, 'x')));
  EXPECT_TRUE(writer.capExceeded());
  EXPECT_FALSE(sink.exists("/.crosspoint/bookorbit/thumbs/41.jpg"));
}
