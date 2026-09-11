#include "ReadingStatsQuery.h"

#include <algorithm>

namespace bookorbit {
namespace {

uint32_t localDay(const uint32_t startTime, const int32_t utcOffsetSeconds) {
  const int64_t shifted = static_cast<int64_t>(startTime) + utcOffsetSeconds;
  if (shifted < 0) return 0;
  return static_cast<uint32_t>(shifted / 86400);
}

std::vector<uint32_t> sortedUniqueDays(const std::vector<ReadingEvent>& events, const int32_t utcOffsetSeconds) {
  std::vector<uint32_t> days;
  days.reserve(events.size());
  for (const auto& event : events) {
    days.push_back(localDay(event.startTime, utcOffsetSeconds));
  }
  std::sort(days.begin(), days.end());
  days.erase(std::unique(days.begin(), days.end()), days.end());
  return days;
}

}  // namespace

BookStats computeBookStats(const std::vector<ReadingEvent>& events, const ClampSettings& settings,
                           const int32_t utcOffsetSeconds) {
  BookStats stats;
  if (events.empty()) return stats;

  std::vector<std::pair<uint32_t, uint32_t>> perPage;  // (page, summed duration)
  perPage.reserve(events.size());

  for (const auto& event : events) {
    stats.totalTimeUncapped += event.durationSeconds;
    const auto it =
        std::find_if(perPage.begin(), perPage.end(), [&](const auto& entry) { return entry.first == event.page; });
    if (it == perPage.end()) {
      perPage.emplace_back(event.page, event.durationSeconds);
    } else {
      it->second += event.durationSeconds;
    }
  }

  stats.pagesRead = static_cast<uint32_t>(perPage.size());
  for (const auto& [page, summed] : perPage) {
    (void)page;
    stats.totalTimeCapped += std::min<uint32_t>(summed, settings.maxSec);
  }

  stats.activeDays = static_cast<uint32_t>(sortedUniqueDays(events, utcOffsetSeconds).size());

  if (stats.pagesRead > 0) {
    stats.avgSecondsPerPage = static_cast<float>(stats.totalTimeCapped) / static_cast<float>(stats.pagesRead);
  }
  if (stats.activeDays > 0) {
    stats.avgSecondsPerDay = static_cast<float>(stats.totalTimeCapped) / static_cast<float>(stats.activeDays);
  }
  return stats;
}

uint32_t estimatedSecondsLeft(const BookStats& stats, const uint32_t currentPage, const uint32_t totalPages) {
  if (totalPages <= currentPage) return 0;
  const uint32_t remaining = totalPages - currentPage;
  return static_cast<uint32_t>(static_cast<float>(remaining) * stats.avgSecondsPerPage);
}

uint32_t currentStreakDays(const std::vector<ReadingEvent>& events, const uint32_t nowUnix,
                           const int32_t utcOffsetSeconds) {
  const auto days = sortedUniqueDays(events, utcOffsetSeconds);
  if (days.empty()) return 0;

  const uint32_t today = localDay(nowUnix, utcOffsetSeconds);
  if (days.back() != today) return 0;

  uint32_t streak = 1;
  for (size_t i = days.size() - 1; i > 0; i--) {
    if (days[i] - days[i - 1] != 1) break;
    streak++;
  }
  return streak;
}

uint32_t longestStreakDays(const std::vector<ReadingEvent>& events, const int32_t utcOffsetSeconds) {
  const auto days = sortedUniqueDays(events, utcOffsetSeconds);
  if (days.empty()) return 0;

  uint32_t best = 1;
  uint32_t run = 1;
  for (size_t i = 1; i < days.size(); i++) {
    run = (days[i] - days[i - 1] == 1) ? run + 1 : 1;
    best = std::max(best, run);
  }
  return best;
}

}  // namespace bookorbit
