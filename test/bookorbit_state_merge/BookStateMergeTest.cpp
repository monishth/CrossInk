#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookStateMerge.h"

using bookorbit::applyMerge;
using bookorbit::LocalBookState;
using bookorbit::MergeDecision;
using bookorbit::resolveBookState;
using bookorbit::ServerBookState;

namespace {

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

}  // namespace

TEST(BookStateMerge, UnknownServerFieldsChangeNothing) {
  LocalBookState local;
  local.setRating(3, day("2026-08-21"));
  ServerBookState server;
  server.hash = "abc";

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
  EXPECT_FALSE(decision.applyReview);
  EXPECT_FALSE(decision.changedAnything());
}

TEST(BookStateMerge, NewerServerRatingWins) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_TRUE(decision.ratingSet);
  EXPECT_EQ(decision.rating, 5);
  EXPECT_EQ(decision.ratingModified.day, 21);
}

TEST(BookStateMerge, NewerLocalRatingIsKept) {
  LocalBookState local;
  local.setRating(3, day("2026-08-22"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
}

// Date-only granularity makes same-day edits a tie, and the server wins those.
TEST(BookStateMerge, SameDayTiePrefersServer) {
  LocalBookState local;
  local.setRating(3, day("2026-08-21"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_EQ(decision.rating, 5);
}

TEST(BookStateMerge, ServerClearedRatingIsApplied) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = false;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_FALSE(decision.ratingSet);
  EXPECT_EQ(decision.rating, 0);
}

TEST(BookStateMerge, ServerRatingArrivesWhenDeviceHasNone) {
  LocalBookState local;
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 2;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_EQ(decision.rating, 2);
}

TEST(BookStateMerge, ReviewUsesItsOwnDate) {
  LocalBookState local;
  local.setRating(3, day("2026-08-25"));  // newer status date
  local.setReview("Old note.", day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "Server note.";
  server.reviewUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);  // server said nothing about the rating
  ASSERT_TRUE(decision.applyReview);
  EXPECT_EQ(decision.reviewNote, "Server note.");
  EXPECT_EQ(decision.reviewModified.day, 21);
}

TEST(BookStateMerge, NewerLocalReviewIsKept) {
  LocalBookState local;
  local.setReview("Local note.", day("2026-08-22"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "Server note.";
  server.reviewUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyReview);
}

TEST(BookStateMerge, IdenticalValuesAreNotAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
  EXPECT_FALSE(decision.changedAnything());
}

TEST(BookStateMerge, ApplyMergeWritesRatingAndItsDate) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.ratingSet);
  EXPECT_EQ(local.rating, 5);
  EXPECT_EQ(local.statusModified.day, 21);
}

TEST(BookStateMerge, ApplyMergeWritesReviewAndItsDate) {
  LocalBookState local;
  local.setReview("Old.", day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "New.";
  server.reviewUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.reviewSet);
  EXPECT_EQ(local.reviewNote, "New.");
  EXPECT_EQ(local.reviewModified.day, 21);
}

TEST(BookStateMerge, ApplyMergeLeavesTheStatusFlagAlone) {
  LocalBookState local;
  local.setStatus(bookorbit::BookStatus::Complete, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.statusKnown);
  EXPECT_EQ(local.status, bookorbit::BookStatus::Complete);
}
