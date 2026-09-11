#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBulkProgress.h"

using bookorbit::BulkProgressItem;
using bookorbit::decodeBulkProgressResponse;
using bookorbit::encodeBulkProgress;
using bookorbit::kBulkProgressBatchSize;

namespace {

BulkProgressItem item(const char* hash, const float percentage, const char* progress, const uint32_t timestamp) {
  BulkProgressItem value;
  value.hash = hash;
  value.percentage = percentage;
  value.progress = progress;
  value.timestamp = timestamp;
  return value;
}

std::vector<BulkProgressItem> many(const size_t count) {
  std::vector<BulkProgressItem> items;
  items.reserve(count);
  for (size_t i = 0; i < count; i++) {
    items.push_back(item("d18e399f0f79f24d68a8f70b76d59914", 0.5f, "/body[1]/DocFragment[1]/body[1]/p[1]", 1u));
  }
  return items;
}

}  // namespace

TEST(BookOrbitBulkProgress, BatchSizeIsOneHundred) { EXPECT_EQ(kBulkProgressBatchSize, 100u); }

TEST(BookOrbitBulkProgress, EncodesOneItem) {
  const std::vector<BulkProgressItem> items = {item("d18e399f0f79f24d68a8f70b76d59914", 0.001859f,
                                                    "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0", 1787561453u)};
  size_t consumed = 0;
  EXPECT_EQ(encodeBulkProgress(items, 0, consumed),
            "{\"items\":[{\"hash\":\"d18e399f0f79f24d68a8f70b76d59914\","
            "\"percentage\":0.001859,"
            "\"progress\":\"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0\","
            "\"timestamp\":1787561453}]}");
  EXPECT_EQ(consumed, 1u);
}

TEST(BookOrbitBulkProgress, SeparatesItemsWithCommas) {
  const std::vector<BulkProgressItem> items = {item("aa", 0.0f, "/body[1]/DocFragment[1]/body[1]", 1u),
                                               item("bb", 1.0f, "/body[1]/DocFragment[2]/body[1]", 2u)};
  size_t consumed = 0;
  const std::string body = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, 2u);
  EXPECT_NE(body.find("},{"), std::string::npos);
}

TEST(BookOrbitBulkProgress, StopsAtTheBatchLimit) {
  const auto items = many(250);
  size_t consumed = 0;
  const std::string first = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, kBulkProgressBatchSize);
  EXPECT_FALSE(first.empty());

  size_t second = 0;
  encodeBulkProgress(items, kBulkProgressBatchSize, second);
  EXPECT_EQ(second, kBulkProgressBatchSize);

  size_t third = 0;
  encodeBulkProgress(items, 2 * kBulkProgressBatchSize, third);
  EXPECT_EQ(third, 50u);
}

TEST(BookOrbitBulkProgress, OffsetPastTheEndEncodesNothing) {
  const auto items = many(3);
  size_t consumed = 7;
  EXPECT_EQ(encodeBulkProgress(items, 3, consumed), "");
  EXPECT_EQ(consumed, 0u);
}

// Never degrade silently: items missing an xpointer are dropped rather than
// uploaded percentage-only, and the caller learns via consumed vs emitted.
TEST(BookOrbitBulkProgress, SkipsItemsWithoutAnXpointer) {
  std::vector<BulkProgressItem> items = many(2);
  items[0].progress.clear();
  size_t consumed = 0;
  const std::string body = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, 2u);
  EXPECT_EQ(body.find("},{"), std::string::npos);
}

TEST(BookOrbitBulkProgress, AllItemsSkippedEncodesNothing) {
  std::vector<BulkProgressItem> items = many(2);
  items[0].progress.clear();
  items[1].progress.clear();
  size_t consumed = 0;
  EXPECT_EQ(encodeBulkProgress(items, 0, consumed), "");
  EXPECT_EQ(consumed, 2u);
}

TEST(BookOrbitBulkProgress, NormalizesUnindexedXpointersOnEncode) {
  const std::vector<BulkProgressItem> items = {item("aa", 0.5f, "/body/DocFragment[2]/body/p[3]", 1u)};
  size_t consumed = 0;
  EXPECT_NE(encodeBulkProgress(items, 0, consumed).find("\"/body[1]/DocFragment[2]/body[1]/p[3]\""), std::string::npos);
}

TEST(BookOrbitBulkProgress, DecodesUnmatchedHashes) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"unmatched\":[\"aa\",\"bb\"]}", unmatched));
  ASSERT_EQ(unmatched.size(), 2u);
  EXPECT_EQ(unmatched[0], "aa");
  EXPECT_EQ(unmatched[1], "bb");
}

TEST(BookOrbitBulkProgress, DecodesAnEmptyUnmatchedArray) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"unmatched\":[]}", unmatched));
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookOrbitBulkProgress, IgnoresUnrelatedArrays) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"accepted\":[\"cc\"],\"unmatched\":[\"aa\"]}", unmatched));
  ASSERT_EQ(unmatched.size(), 1u);
  EXPECT_EQ(unmatched[0], "aa");
}

TEST(BookOrbitBulkProgress, MalformedJsonIsRejected) {
  std::vector<std::string> unmatched;
  EXPECT_FALSE(decodeBulkProgressResponse("{\"unmatched\":[", unmatched));
  EXPECT_TRUE(unmatched.empty());
}
