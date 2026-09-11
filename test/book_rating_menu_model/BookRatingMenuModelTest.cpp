#include <gtest/gtest.h>

#include <string>

#include "src/activities/reader/BookRatingMenuModel.h"

TEST(BookRatingMenuModel, HasSixOptions) {
  EXPECT_EQ(kRatingOptionCount, 6);
}

TEST(BookRatingMenuModel, FirstOptionIsNotRated) {
  EXPECT_EQ(ratingForOptionIndex(0), 0);
}

TEST(BookRatingMenuModel, OptionsOneThroughFiveMapToStars) {
  for (int index = 1; index <= 5; index++) {
    EXPECT_EQ(ratingForOptionIndex(index), index) << index;
  }
}

TEST(BookRatingMenuModel, OutOfRangeIndexFallsBackToNotRated) {
  EXPECT_EQ(ratingForOptionIndex(-1), 0);
  EXPECT_EQ(ratingForOptionIndex(6), 0);
  EXPECT_EQ(ratingForOptionIndex(99), 0);
}

TEST(BookRatingMenuModel, UnsetRatingSelectsTheFirstOption) {
  EXPECT_EQ(optionIndexForRating(false, 0), 0);
  EXPECT_EQ(optionIndexForRating(false, 4), 0);
}

TEST(BookRatingMenuModel, SetRatingSelectsItsOwnOption) {
  EXPECT_EQ(optionIndexForRating(true, 1), 1);
  EXPECT_EQ(optionIndexForRating(true, 5), 5);
}

TEST(BookRatingMenuModel, CorruptRatingSelectsNotRated) {
  EXPECT_EQ(optionIndexForRating(true, 0), 0);
  EXPECT_EQ(optionIndexForRating(true, 9), 0);
}

TEST(BookRatingMenuModel, LabelDrawsFilledAndEmptyStars) {
  char buf[32] = {};
  ratingStarsLabel(3, buf, sizeof(buf));
  EXPECT_STREQ(buf, "***..");
  ratingStarsLabel(5, buf, sizeof(buf));
  EXPECT_STREQ(buf, "*****");
  ratingStarsLabel(1, buf, sizeof(buf));
  EXPECT_STREQ(buf, "*....");
}

TEST(BookRatingMenuModel, LabelForNotRatedIsEmpty) {
  char buf[32] = {'x', '\0'};
  ratingStarsLabel(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "");
}

TEST(BookRatingMenuModel, LabelNeverOverrunsASmallBuffer) {
  char buf[4] = {};
  ratingStarsLabel(5, buf, sizeof(buf));
  EXPECT_EQ(std::string(buf).size(), 3u);
}
