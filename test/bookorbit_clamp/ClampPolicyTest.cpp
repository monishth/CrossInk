#include <gtest/gtest.h>

#include "lib/BookOrbit/ClampPolicy.h"

using bookorbit::clampDwell;
using bookorbit::ClampSettings;

TEST(ClampPolicy, DefaultsMatchKOReader) {
  const ClampSettings settings;
  EXPECT_EQ(settings.minSec, 5);
  EXPECT_EQ(settings.maxSec, 120);
}

TEST(ClampPolicy, DiscardsBelowMinimum) {
  const ClampSettings settings;
  uint16_t out = 0xFFFF;
  EXPECT_FALSE(clampDwell(4, settings, out));
}

TEST(ClampPolicy, AcceptsExactlyMinimum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(5, settings, out));
  EXPECT_EQ(out, 5);
}

TEST(ClampPolicy, CreditsInFullWithinRange) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(37, settings, out));
  EXPECT_EQ(out, 37);
}

TEST(ClampPolicy, AcceptsExactlyMaximum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(120, settings, out));
  EXPECT_EQ(out, 120);
}

// The critical divergence from CrossInk's current behaviour: a long dwell is
// CLAMPED to max_sec, not discarded. KOReader credits 120s for an idle page.
TEST(ClampPolicy, ClampsAboveMaximumRatherThanDiscarding) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(121, settings, out));
  EXPECT_EQ(out, 120);
}

TEST(ClampPolicy, ClampsVeryLongIdleToMaximum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(7200, settings, out));
  EXPECT_EQ(out, 120);
}

TEST(ClampPolicy, HonoursCustomSettings) {
  ClampSettings settings;
  settings.minSec = 10;
  settings.maxSec = 60;
  uint16_t out = 0;
  EXPECT_FALSE(clampDwell(9, settings, out));
  ASSERT_TRUE(clampDwell(300, settings, out));
  EXPECT_EQ(out, 60);
}

TEST(ClampPolicy, ZeroDwellIsDiscarded) {
  const ClampSettings settings;
  uint16_t out = 0xFFFF;
  EXPECT_FALSE(clampDwell(0, settings, out));
}
