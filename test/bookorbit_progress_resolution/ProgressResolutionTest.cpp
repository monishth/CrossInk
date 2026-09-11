#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitProgress.h"
#include "lib/BookOrbit/ProgressResolution.h"

using bookorbit::chooseRemoteProgress;
using bookorbit::isSendable;
using bookorbit::kDegradedJumpThreshold;
using bookorbit::ProgressRecord;
using bookorbit::ProgressSource;

namespace {

ProgressRecord remote(const char* progress, const float percentage) {
  ProgressRecord record;
  record.document = "d18e399f0f79f24d68a8f70b76d59914";
  record.progress = progress;
  record.percentage = percentage;
  record.device = "KOReader";
  record.deviceId = "9F8E7D";
  record.timestamp = 1787407272u;
  return record;
}

}  // namespace

TEST(ProgressResolution, ThresholdIsTwoPercent) { EXPECT_FLOAT_EQ(kDegradedJumpThreshold, 0.02f); }

// Outbound: both fields or nothing.
TEST(ProgressResolution, SendableRequiresBothProgressAndPercentage) {
  EXPECT_TRUE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", 0.5f)));
  EXPECT_FALSE(isSendable(remote("", 0.5f)));
}

TEST(ProgressResolution, SendableRejectsOutOfRangePercentage) {
  EXPECT_FALSE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", 1.5f)));
  EXPECT_FALSE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", -0.1f)));
}

TEST(ProgressResolution, SendableRejectsAnUnparseableXpointer) {
  EXPECT_FALSE(isSendable(remote("not-an-xpointer", 0.5f)));
}

TEST(ProgressResolution, SendableRejectsAMissingDocument) {
  ProgressRecord record = remote("/body[1]/DocFragment[1]/body[1]/p[1]", 0.5f);
  record.document.clear();
  EXPECT_FALSE(isSendable(record));
}

// Inbound: the xpointer wins whenever it resolved.
TEST(ProgressResolution, PrefersTheXpointerWhenItResolves) {
  const auto chosen =
      chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.2052f), true, 0.2049f, 0.1000f);
  EXPECT_EQ(chosen.source, ProgressSource::Xpointer);
  EXPECT_FALSE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
  EXPECT_FLOAT_EQ(chosen.percentage, 0.2049f);
  EXPECT_EQ(chosen.xpointer, "/body[1]/DocFragment[8]/body[1]/p[4]");
}

// A big jump from a *resolved* xpointer is a real position, not a degradation.
TEST(ProgressResolution, LargeJumpFromAResolvedXpointerNeedsNoNotice) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.90f), true, 0.90f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::Xpointer);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, FallsBackToPercentageWhenResolutionFails) {
  const auto chosen =
      chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.2052f), false, 0.0f, 0.2000f);
  EXPECT_EQ(chosen.source, ProgressSource::Percentage);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FLOAT_EQ(chosen.percentage, 0.2052f);
}

// The case the UI must surface: degraded AND the reader moves noticeably.
TEST(ProgressResolution, DegradedJumpBeyondThresholdNeedsNotice) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.60f), false, 0.0f, 0.10f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, DegradedJumpInsideThresholdIsSilent) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.205f), false, 0.0f, 0.200f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, ThresholdIsExclusiveAtExactlyTwoPercent) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.22f), false, 0.0f, 0.20f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, BackwardJumpsCountToo) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.10f), false, 0.0f, 0.60f);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

// No xpointer at all is not a degradation of ours — it is a percentage-only
// peer — but it still flags, because the landing is still approximate.
TEST(ProgressResolution, PercentageOnlyRemoteIsFlaggedDegraded) {
  const auto chosen = chooseRemoteProgress(remote("", 0.60f), false, 0.0f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::Percentage);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, EmptyRemoteResolvesToNothing) {
  const auto chosen = chooseRemoteProgress(remote("", 0.0f), false, 0.0f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::None);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, NormalizesTheChosenXpointer) {
  const auto chosen = chooseRemoteProgress(remote("/body/DocFragment[2]/body/p[3]", 0.3f), true, 0.3f, 0.3f);
  EXPECT_EQ(chosen.xpointer, "/body[1]/DocFragment[2]/body[1]/p[3]");
}
