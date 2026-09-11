#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitDate.h"

using bookorbit::compareDateOnly;
using bookorbit::DateOnly;
using bookorbit::formatDateOnly;
using bookorbit::parseDateOnly;
using bookorbit::resolveByDate;
using bookorbit::Winner;

namespace {

DateOnly date(const uint16_t year, const uint8_t month, const uint8_t day) {
  DateOnly out;
  out.year = year;
  out.month = month;
  out.day = day;
  return out;
}

}  // namespace

TEST(BookOrbitDate, ParsesIsoDate) {
  DateOnly out;
  ASSERT_TRUE(parseDateOnly("2026-08-21", out));
  EXPECT_EQ(out.year, 2026);
  EXPECT_EQ(out.month, 8);
  EXPECT_EQ(out.day, 21);
  EXPECT_TRUE(out.valid());
}

// The server also sends full timestamps in *UpdatedAt. The Lua plugin keys on
// the first ten characters; do the same rather than rejecting the value.
TEST(BookOrbitDate, ParsesDatePrefixOfATimestamp) {
  DateOnly out;
  ASSERT_TRUE(parseDateOnly("2026-08-21T14:05:09Z", out));
  EXPECT_EQ(out.day, 21);
}

TEST(BookOrbitDate, RejectsMalformedInput) {
  DateOnly out;
  EXPECT_FALSE(parseDateOnly("", out));
  EXPECT_FALSE(parseDateOnly("2026-8-21", out));
  EXPECT_FALSE(parseDateOnly("21-08-2026", out));
  EXPECT_FALSE(parseDateOnly("2026/08/21", out));
  EXPECT_FALSE(parseDateOnly("abcd-ef-gh", out));
}

TEST(BookOrbitDate, RejectsOutOfRangeComponents) {
  DateOnly out;
  EXPECT_FALSE(parseDateOnly("2026-13-01", out));
  EXPECT_FALSE(parseDateOnly("2026-00-01", out));
  EXPECT_FALSE(parseDateOnly("2026-02-30", out));
  EXPECT_FALSE(parseDateOnly("2026-01-00", out));
}

TEST(BookOrbitDate, AcceptsLeapDay) {
  DateOnly out;
  EXPECT_TRUE(parseDateOnly("2024-02-29", out));
  EXPECT_FALSE(parseDateOnly("2026-02-29", out));
}

TEST(BookOrbitDate, FormatsZeroPadded) {
  char buf[11] = {};
  formatDateOnly(date(2026, 8, 1), buf, sizeof(buf));
  EXPECT_STREQ(buf, "2026-08-01");
}

TEST(BookOrbitDate, FormatsInvalidDateAsEmpty) {
  char buf[11] = {'x', '\0'};
  formatDateOnly(DateOnly{}, buf, sizeof(buf));
  EXPECT_STREQ(buf, "");
}

TEST(BookOrbitDate, ComparesChronologically) {
  EXPECT_LT(compareDateOnly(date(2026, 8, 20), date(2026, 8, 21)), 0);
  EXPECT_GT(compareDateOnly(date(2026, 9, 1), date(2026, 8, 31)), 0);
  EXPECT_EQ(compareDateOnly(date(2026, 8, 21), date(2026, 8, 21)), 0);
  EXPECT_LT(compareDateOnly(date(2025, 12, 31), date(2026, 1, 1)), 0);
}

TEST(BookOrbitDate, NewerLocalDateWins) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 22), date(2026, 8, 21)), Winner::Local);
}

TEST(BookOrbitDate, NewerServerDateWins) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 20), date(2026, 8, 21)), Winner::Server);
}

// Date-only granularity means ties are common. The server is authoritative on
// a tie, so two devices editing on the same day converge instead of flapping.
TEST(BookOrbitDate, TiePrefersServer) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 21), date(2026, 8, 21)), Winner::Server);
}

TEST(BookOrbitDate, MissingLocalDateLosesToServer) {
  EXPECT_EQ(resolveByDate(DateOnly{}, date(2026, 8, 21)), Winner::Server);
}

TEST(BookOrbitDate, MissingServerDateLosesToLocal) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 21), DateOnly{}), Winner::Local);
}

// Neither side is dated: the server value is the shared baseline, so take it.
TEST(BookOrbitDate, BothMissingPrefersServer) { EXPECT_EQ(resolveByDate(DateOnly{}, DateOnly{}), Winner::Server); }
