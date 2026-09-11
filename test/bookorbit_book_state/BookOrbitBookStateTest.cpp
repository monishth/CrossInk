#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitBookState.h"

using bookorbit::BookStatus;
using bookorbit::kReviewNoteMaxBytes;
using bookorbit::LocalBookState;
using bookorbit::normalizeRating;
using bookorbit::statusFromCompletion;
using bookorbit::statusFromString;
using bookorbit::statusToString;
using bookorbit::truncateReview;

TEST(BookOrbitBookState, StatusRoundTripsThroughWireStrings) {
  EXPECT_STREQ(statusToString(BookStatus::Reading), "reading");
  EXPECT_STREQ(statusToString(BookStatus::Complete), "complete");
  EXPECT_STREQ(statusToString(BookStatus::Abandoned), "abandoned");

  BookStatus parsed = BookStatus::Reading;
  ASSERT_TRUE(statusFromString("complete", parsed));
  EXPECT_EQ(parsed, BookStatus::Complete);
  ASSERT_TRUE(statusFromString("abandoned", parsed));
  EXPECT_EQ(parsed, BookStatus::Abandoned);
  ASSERT_TRUE(statusFromString("reading", parsed));
  EXPECT_EQ(parsed, BookStatus::Reading);
}

// The vocabulary is closed. An unknown status is dropped, never guessed at.
TEST(BookOrbitBookState, UnknownStatusStringIsRejected) {
  BookStatus parsed = BookStatus::Complete;
  EXPECT_FALSE(statusFromString("finished", parsed));
  EXPECT_FALSE(statusFromString("", parsed));
  EXPECT_FALSE(statusFromString("Complete", parsed));
  EXPECT_EQ(parsed, BookStatus::Complete);  // untouched on failure
}

TEST(BookOrbitBookState, CompletedFlagMapsToComplete) {
  EXPECT_EQ(statusFromCompletion(true, BookStatus::Reading), BookStatus::Complete);
  EXPECT_EQ(statusFromCompletion(true, BookStatus::Abandoned), BookStatus::Complete);
}

TEST(BookOrbitBookState, ClearedFlagMapsToReading) {
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Complete), BookStatus::Reading);
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Reading), BookStatus::Reading);
}

// CrossInk has no "abandoned" gesture. A book the server calls abandoned keeps
// that status while isCompleted stays false, so a pull is never undone by the
// next push.
TEST(BookOrbitBookState, AbandonedSurvivesAnUncompletedFlag) {
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Abandoned), BookStatus::Abandoned);
}

TEST(BookOrbitBookState, RatingAcceptsOneToFive) {
  uint8_t out = 0;
  for (int value = 1; value <= 5; value++) {
    ASSERT_TRUE(normalizeRating(value, out)) << value;
    EXPECT_EQ(out, static_cast<uint8_t>(value));
  }
}

TEST(BookOrbitBookState, RatingRejectsOutOfRange) {
  uint8_t out = 3;
  EXPECT_FALSE(normalizeRating(0, out));
  EXPECT_FALSE(normalizeRating(6, out));
  EXPECT_FALSE(normalizeRating(-1, out));
  EXPECT_EQ(out, 3);  // untouched on failure
}

TEST(BookOrbitBookState, ReviewIsTruncatedAtTheWireLimit) {
  const std::string long_note(kReviewNoteMaxBytes + 500, 'a');
  EXPECT_EQ(truncateReview(long_note).size(), kReviewNoteMaxBytes);
  EXPECT_EQ(truncateReview("short").size(), 5u);
}

TEST(BookOrbitBookState, LocalStateStartsEmpty) {
  LocalBookState state;
  EXPECT_FALSE(state.statusKnown);
  EXPECT_FALSE(state.ratingSet);
  EXPECT_FALSE(state.reviewSet);
  EXPECT_FALSE(state.statusModified.valid());
  EXPECT_FALSE(state.reviewModified.valid());
  EXPECT_TRUE(state.reviewNote.empty());
}

TEST(BookOrbitBookState, SetRatingStampsTheDate) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  ASSERT_TRUE(state.setRating(4, today));
  EXPECT_TRUE(state.ratingSet);
  EXPECT_EQ(state.rating, 4);
  EXPECT_EQ(state.statusModified.day, 21);
}

TEST(BookOrbitBookState, ClearRatingStampsTheDateAndUnsetsIt) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));
  ASSERT_TRUE(state.setRating(4, today));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-22", later));
  state.clearRating(later);
  EXPECT_FALSE(state.ratingSet);
  EXPECT_EQ(state.rating, 0);
  EXPECT_EQ(state.statusModified.day, 22);
}

TEST(BookOrbitBookState, SetReviewStampsTheReviewDateOnly) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  state.setReview("Great fun.", today);
  EXPECT_TRUE(state.reviewSet);
  EXPECT_EQ(state.reviewNote, "Great fun.");
  EXPECT_EQ(state.reviewModified.day, 21);
  EXPECT_FALSE(state.statusModified.valid());
}

TEST(BookOrbitBookState, CompletionToggleStampsCompleteAndTheDate) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, today));
  EXPECT_TRUE(state.statusKnown);
  EXPECT_EQ(state.status, BookStatus::Complete);
  EXPECT_EQ(state.statusModified.day, 21);
}

TEST(BookOrbitBookState, UncompletingStampsReadingAndTheDate) {
  LocalBookState state;
  bookorbit::DateOnly first;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", first));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, first));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, false, later));
  EXPECT_EQ(state.status, BookStatus::Reading);
  EXPECT_EQ(state.statusModified.day, 25);
}

// Re-marking an already-complete book must not bump the date, or a harmless
// re-open would win every conflict against a genuine server edit.
TEST(BookOrbitBookState, RepeatedToggleDoesNotBumpTheDate) {
  LocalBookState state;
  bookorbit::DateOnly first;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", first));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, first));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, true, later));
  EXPECT_EQ(state.statusModified.day, 21);
}

// Without the RTC there is no date to resolve a conflict with. Refuse rather
// than stamping a fabricated day, exactly as the event log refuses a startTime.
TEST(BookOrbitBookState, InvalidDateRefusesTheToggle) {
  LocalBookState state;
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, true, bookorbit::DateOnly{}));
  EXPECT_FALSE(state.statusKnown);
}

TEST(BookOrbitBookState, AbandonedIsNotOverwrittenByAnUncompletedToggle) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));
  state.setStatus(BookStatus::Abandoned, today);

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, false, later));
  EXPECT_EQ(state.status, BookStatus::Abandoned);
  EXPECT_EQ(state.statusModified.day, 21);
}
