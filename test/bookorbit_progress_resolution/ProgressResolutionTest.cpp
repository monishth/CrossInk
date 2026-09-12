#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitProgress.h"
#include "lib/BookOrbit/ProgressResolution.h"

using bookorbit::chooseRemoteProgress;
using bookorbit::decideProgressAction;
using bookorbit::isSendable;
using bookorbit::kDegradedJumpThreshold;
using bookorbit::ProgressAction;
using bookorbit::ProgressRecord;
using bookorbit::ProgressSource;
using bookorbit::ResolvedProgress;

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

namespace {

constexpr char kThisDevice[] = "crossink-900ed53f1fb8";

// A landing this device could actually open: an exact xpointer that resolved to
// a spine item and an offset.
ResolvedProgress landable(const uint32_t timestamp, const char* deviceId) {
  ResolvedProgress resolved;
  resolved.source = ProgressSource::Xpointer;
  resolved.xpointer = "/body[1]/DocFragment[30]/body[1]/p[62]/text()[1].196";
  resolved.percentage = 0.6994f;
  resolved.spineIndex = 29;
  resolved.visibleTextOffset = 8123;
  resolved.remoteTimestamp = timestamp;
  resolved.remoteDeviceId = deviceId;
  return resolved;
}

}  // namespace

// The whole point of the feature: read on the Kindle, then sync on the reader
// and land where the Kindle left off.
TEST(ProgressResolution, NewerRemoteWithAnExactPositionMovesTheReader) {
  const auto remoteSide = landable(2000u, "kindle-7E0E4005");
  EXPECT_EQ(decideProgressAction(remoteSide, 1000u, kThisDevice), ProgressAction::ApplyRemote);
}

// Reading done here after the last sync must not be undone by an older record.
TEST(ProgressResolution, OlderRemoteLosesToLocalReading) {
  const auto remoteSide = landable(1000u, "kindle-7E0E4005");
  EXPECT_EQ(decideProgressAction(remoteSide, 2000u, kThisDevice), ProgressAction::PushLocal);
}

// Equal timestamps are not newer. Without this the reader would re-apply the
// same position every sync.
TEST(ProgressResolution, EqualTimestampsPushRatherThanApply) {
  const auto remoteSide = landable(2000u, "kindle-7E0E4005");
  EXPECT_EQ(decideProgressAction(remoteSide, 2000u, kThisDevice), ProgressAction::PushLocal);
}

// Our own push echoed back is not another device's reading, however new it is.
TEST(ProgressResolution, ThisDevicesOwnRecordIsNeverApplied) {
  const auto remoteSide = landable(9999u, kThisDevice);
  EXPECT_EQ(decideProgressAction(remoteSide, 1000u, kThisDevice), ProgressAction::PushLocal);
}

// A fresh device has no reading to protect, so a usable remote wins.
TEST(ProgressResolution, NoLocalReadingLetsTheRemoteWin) {
  const auto remoteSide = landable(1u, "kindle-7E0E4005");
  EXPECT_EQ(decideProgressAction(remoteSide, 0u, kThisDevice), ProgressAction::ApplyRemote);
}

// Newer elsewhere but percentage-only: there is nothing to land on, and
// pushing would replace a real position with an older one.
TEST(ProgressResolution, NewerButUnlandableHoldsBothSides) {
  ResolvedProgress remoteSide;
  remoteSide.source = ProgressSource::Percentage;
  remoteSide.degraded = true;
  remoteSide.percentage = 0.55f;
  remoteSide.remoteTimestamp = 2000u;
  remoteSide.remoteDeviceId = "kindle-7E0E4005";
  EXPECT_EQ(decideProgressAction(remoteSide, 1000u, kThisDevice), ProgressAction::HoldBoth);
}

// An xpointer that parsed but resolved to no spine item is not landable either.
TEST(ProgressResolution, ResolvedSourceWithoutASpineHoldsBothSides) {
  auto remoteSide = landable(2000u, "kindle-7E0E4005");
  remoteSide.spineIndex = -1;
  EXPECT_EQ(decideProgressAction(remoteSide, 1000u, kThisDevice), ProgressAction::HoldBoth);
}

TEST(ProgressResolution, NothingUsableFallsBackToPushing) {
  const ResolvedProgress nothing;
  EXPECT_EQ(decideProgressAction(nothing, 0u, kThisDevice), ProgressAction::PushLocal);
}

// chooseRemoteProgress must carry identity through, or the caller cannot tell
// whose record it is holding.
TEST(ProgressResolution, CarriesRemoteIdentityThrough) {
  const auto chosen = chooseRemoteProgress(remote("/body/DocFragment[2]/body/p[3]", 0.3f), true, 0.3f, 0.3f);
  EXPECT_EQ(chosen.remoteDeviceId, "9F8E7D");
  EXPECT_EQ(chosen.remoteTimestamp, 1787407272u);
}
