#pragma once

#include <cstdint>
#include <vector>

#include "ClampPolicy.h"
#include "ReadingEvent.h"

namespace bookorbit {

struct BookStats {
  uint32_t totalTimeUncapped = 0;
  uint32_t totalTimeCapped = 0;
  uint32_t pagesRead = 0;   // distinct pages
  uint32_t activeDays = 0;  // distinct local calendar days
  float avgSecondsPerPage = 0.0f;
  float avgSecondsPerDay = 0.0f;
};

// KOReader's book statistics, computed over the raw event log.
//
// The capped total groups by PAGE, not by event: min(sum(duration), maxSec)
// per distinct page, matching STATISTICS_SQL_BOOK_CAPPED_TOTALS_QUERY in
// statistics.koplugin/main.lua:40-49. Averages derive from the capped totals.
BookStats computeBookStats(const std::vector<ReadingEvent>& events, const ClampSettings& settings,
                           int32_t utcOffsetSeconds);

// (totalPages - currentPage) * avgSecondsPerPage.
uint32_t estimatedSecondsLeft(const BookStats& stats, uint32_t currentPage, uint32_t totalPages);

// Consecutive local days with reading, ending today. Zero when today has none.
uint32_t currentStreakDays(const std::vector<ReadingEvent>& events, uint32_t nowUnix, int32_t utcOffsetSeconds);

uint32_t longestStreakDays(const std::vector<ReadingEvent>& events, int32_t utcOffsetSeconds);

}  // namespace bookorbit
