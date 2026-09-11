#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lib/BookOrbit/ReadingStatsQuery.h"

using bookorbit::ClampSettings;
using bookorbit::computeBookStats;
using bookorbit::ReadingEvent;

namespace {

struct Expected {
  uint32_t pagesRead = 0;
  uint32_t uncapped = 0;
  uint32_t capped = 0;
  uint32_t activeDays = 0;
};

std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) fields.push_back(field);
  return fields;
}

std::string fixturePath(const char* name) { return std::string(BOOKORBIT_GROUND_TRUTH_DIR) + "/" + name; }

std::map<int, std::vector<ReadingEvent>> loadEvents() {
  std::map<int, std::vector<ReadingEvent>> byBook;
  std::ifstream file(fixturePath("events.csv"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    const auto fields = splitCsv(line);
    if (fields.size() < 5) continue;
    ReadingEvent event;
    event.page = static_cast<uint32_t>(std::stoul(fields[1]));
    event.startTime = static_cast<uint32_t>(std::stoul(fields[2]));
    event.durationSeconds = static_cast<uint16_t>(std::stoul(fields[3]));
    event.totalPages = static_cast<uint16_t>(std::stoul(fields[4]));
    byBook[std::stoi(fields[0])].push_back(event);
  }
  return byBook;
}

std::map<int, Expected> loadExpected() {
  std::map<int, Expected> expected;
  std::ifstream file(fixturePath("expected.csv"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    const auto fields = splitCsv(line);
    if (fields.size() < 5 || fields[1].empty()) continue;
    Expected row;
    row.pagesRead = static_cast<uint32_t>(std::stoul(fields[1]));
    row.uncapped = fields[2].empty() ? 0 : static_cast<uint32_t>(std::stoul(fields[2]));
    row.capped = fields[3].empty() ? 0 : static_cast<uint32_t>(std::stoul(fields[3]));
    row.activeDays = static_cast<uint32_t>(std::stoul(fields[4]));
    expected[std::stoi(fields[0])] = row;
  }
  return expected;
}

}  // namespace

// Guards against a vacuous pass: if the fixtures are missing or empty, every
// other assertion below would trivially hold.
TEST(StatsGroundTruth, FixturesAreLoaded) {
  const auto events = loadEvents();
  const auto expected = loadExpected();
  ASSERT_FALSE(events.empty()) << "events.csv missing — run export_ground_truth.sh";
  ASSERT_FALSE(expected.empty()) << "expected.csv missing — run export_ground_truth.sh";

  size_t total = 0;
  for (const auto& [id, list] : events) {
    (void)id;
    total += list.size();
  }
  EXPECT_GT(total, 1000u) << "expected roughly 2027 real events";
}

// The real check: our C++ must agree with numbers SQLite actually produced.
TEST(StatsGroundTruth, MatchesKOReaderSqlForEveryBook) {
  const auto events = loadEvents();
  const auto expected = loadExpected();

  ClampSettings settings;  // maxSec = 120, matching MAX_SEC in the export script
  size_t compared = 0;

  for (const auto& [bookId, expectedRow] : expected) {
    const auto it = events.find(bookId);
    if (it == events.end()) continue;

    const auto stats = computeBookStats(it->second, settings, /*utcOffsetSeconds=*/0);
    EXPECT_EQ(stats.pagesRead, expectedRow.pagesRead) << "pagesRead mismatch for book " << bookId;
    EXPECT_EQ(stats.totalTimeUncapped, expectedRow.uncapped) << "uncapped mismatch for book " << bookId;
    EXPECT_EQ(stats.totalTimeCapped, expectedRow.capped) << "capped mismatch for book " << bookId;
    EXPECT_EQ(stats.activeDays, expectedRow.activeDays) << "activeDays mismatch for book " << bookId;
    compared++;
  }

  EXPECT_GT(compared, 0u) << "no books compared — fixture book ids do not line up";
}
