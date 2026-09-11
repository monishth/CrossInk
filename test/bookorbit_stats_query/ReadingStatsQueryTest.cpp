#include <gtest/gtest.h>

#include <vector>

#include "lib/BookOrbit/ReadingStatsQuery.h"

using bookorbit::ClampSettings;
using bookorbit::computeBookStats;
using bookorbit::currentStreakDays;
using bookorbit::estimatedSecondsLeft;
using bookorbit::longestStreakDays;
using bookorbit::ReadingEvent;

namespace {

ReadingEvent ev(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

constexpr uint32_t kDay = 86400;

}  // namespace

TEST(ReadingStatsQuery, SumsUncappedTime) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 30), ev(2, 1030, 40)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 70u);
}

TEST(ReadingStatsQuery, CountsDistinctPages) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 30), ev(1, 2000, 40), ev(2, 3000, 20)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.pagesRead, 2u);
}

// KOReader caps the SUM per page, not each event. Two 90s visits to one page
// total 180s uncapped but only 120s capped.
TEST(ReadingStatsQuery, CapsPerPageNotPerEvent) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 90), ev(1, 2000, 90)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 180u);
  EXPECT_EQ(stats.totalTimeCapped, 120u);
}

TEST(ReadingStatsQuery, CapsEachPageIndependently) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 90), ev(1, 2000, 90), ev(2, 3000, 30)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeCapped, 150u);  // 120 + 30
}

TEST(ReadingStatsQuery, AverageTimePerPageUsesCappedTotals) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60), ev(2, 2000, 40)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_FLOAT_EQ(stats.avgSecondsPerPage, 50.0f);
}

TEST(ReadingStatsQuery, CountsDistinctActiveDays) {
  const std::vector<ReadingEvent> events{ev(1, 0, 30), ev(2, 100, 30), ev(3, kDay, 30)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.activeDays, 2u);
}

TEST(ReadingStatsQuery, ActiveDaysHonourUtcOffset) {
  // 23:30 UTC is the next local day at +01:00.
  const std::vector<ReadingEvent> events{ev(1, 84600, 30), ev(2, 84700, 30)};
  const auto utc = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(utc.activeDays, 1u);
  const auto shifted = computeBookStats(events, ClampSettings{}, 3600);
  EXPECT_EQ(shifted.activeDays, 1u);
}

TEST(ReadingStatsQuery, EmptyLogYieldsZeroes) {
  const auto stats = computeBookStats({}, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 0u);
  EXPECT_EQ(stats.pagesRead, 0u);
  EXPECT_FLOAT_EQ(stats.avgSecondsPerPage, 0.0f);
}

TEST(ReadingStatsQuery, EstimatesTimeLeftFromAveragePace) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60), ev(2, 2000, 60)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(estimatedSecondsLeft(stats, 10, 20), 600u);  // 10 pages * 60s
}

TEST(ReadingStatsQuery, TimeLeftIsZeroAtEndOfBook) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(estimatedSecondsLeft(stats, 20, 20), 0u);
}

TEST(ReadingStatsQuery, CurrentStreakCountsConsecutiveDaysEndingToday) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 2 * kDay, 30), ev(2, today - kDay, 30), ev(3, today, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 3u);
}

TEST(ReadingStatsQuery, CurrentStreakBreaksOnAGap) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 5 * kDay, 30), ev(2, today, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 1u);
}

TEST(ReadingStatsQuery, CurrentStreakIsZeroWhenNotReadRecently) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 5 * kDay, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 0u);
}

TEST(ReadingStatsQuery, LongestStreakFindsBestRun) {
  const std::vector<ReadingEvent> events{
      ev(1, 1 * kDay, 30),
      ev(2, 2 * kDay, 30),
      ev(3, 3 * kDay, 30),  // run of 3
      ev(4, 9 * kDay, 30),  // gap, run of 1
  };
  EXPECT_EQ(longestStreakDays(events, 0), 3u);
}
