#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/PageStatsCodec.h"

using bookorbit::decodePageStats;
using bookorbit::encodePageStats;
using bookorbit::kStatsBatchSize;
using bookorbit::nextWatermark;
using bookorbit::PageStatsAck;
using bookorbit::ReadingEvent;

namespace {

ReadingEvent makeEvent(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

std::vector<ReadingEvent> makeBatch(const size_t count, const uint32_t sharedStartTime) {
  std::vector<ReadingEvent> events;
  events.reserve(count);
  for (size_t i = 0; i < count; i++) {
    events.push_back(makeEvent(static_cast<uint32_t>(i + 1), sharedStartTime, 10));
  }
  return events;
}

}  // namespace

TEST(PageStatsCodec, BatchSizeMatchesLuaClient) { EXPECT_EQ(kStatsBatchSize, 500u); }

TEST(PageStatsCodec, EncodesBooksWithHashAndEvents) {
  const std::vector<ReadingEvent> events{makeEvent(42, 1787561453u, 37)};
  const std::string json = encodePageStats("0f0a792b00a37cf80baa5e50c078b31f", events);

  EXPECT_NE(json.find(R"("hash":"0f0a792b00a37cf80baa5e50c078b31f")"), std::string::npos);
  EXPECT_NE(json.find(R"("page":42)"), std::string::npos);
  EXPECT_NE(json.find(R"("startTime":1787561453)"), std::string::npos);
  EXPECT_NE(json.find(R"("durationSeconds":37)"), std::string::npos);
  EXPECT_NE(json.find(R"("totalPages":310)"), std::string::npos);
}

TEST(PageStatsCodec, EncodesEmptyEventListAsEmptyArray) {
  const std::string json = encodePageStats("abc", {});
  EXPECT_NE(json.find(R"("events":[])"), std::string::npos);
}

TEST(PageStatsCodec, DecodesWatermarksAndUnmatched) {
  const std::string body = R"({
    "unmatched": ["deadbeef"],
    "results": [{"hash": "abc", "watermark": 1787561453}]
  })";

  PageStatsAck ack;
  ASSERT_TRUE(decodePageStats(body, ack));
  ASSERT_EQ(ack.unmatched.size(), 1u);
  EXPECT_EQ(ack.unmatched[0], "deadbeef");
  ASSERT_EQ(ack.watermarks.size(), 1u);
  EXPECT_EQ(ack.watermarks[0].first, "abc");
  EXPECT_EQ(ack.watermarks[0].second, 1787561453u);
}

TEST(PageStatsCodec, RejectsMalformedJson) {
  PageStatsAck ack;
  EXPECT_FALSE(decodePageStats("{not json", ack));
}

// A short batch means the server saw everything: trust its watermark and stop.
TEST(PageStatsCodec, ShortBatchTrustsServerWatermarkAndStops) {
  const auto events = makeBatch(10, 5000);
  uint32_t watermark = 0;
  EXPECT_FALSE(nextWatermark(events, kStatsBatchSize, 1000u, 5000u, watermark));
  EXPECT_EQ(watermark, 5000u);
}

// THE CRITICAL RULE. A full batch may have been cut inside a group of events
// sharing one startTime. Backing off one second re-sends that boundary group
// on the next round; without this, those events are lost forever. Re-sends are
// idempotent server-side.
TEST(PageStatsCodec, FullBatchBacksOffOneSecondAndContinues) {
  const auto events = makeBatch(kStatsBatchSize, 5000);
  uint32_t watermark = 0;
  EXPECT_TRUE(nextWatermark(events, kStatsBatchSize, 1000u, 5000u, watermark));
  EXPECT_EQ(watermark, 4999u);
}

// The back-off must never move the watermark backwards past where we already
// were, or the sync would loop forever re-sending the same events.
TEST(PageStatsCodec, BackOffNeverRegressesBelowOldWatermark) {
  const auto events = makeBatch(kStatsBatchSize, 1000);
  uint32_t watermark = 0;
  EXPECT_TRUE(nextWatermark(events, kStatsBatchSize, 1000u, 7000u, watermark));
  EXPECT_EQ(watermark, 7000u);  // falls back to the server's value
}

TEST(PageStatsCodec, EmptyBatchStopsWithoutChangingWatermark) {
  uint32_t watermark = 0;
  EXPECT_FALSE(nextWatermark({}, kStatsBatchSize, 1234u, 1234u, watermark));
  EXPECT_EQ(watermark, 1234u);
}
