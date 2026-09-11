#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateCodec.h"

using bookorbit::decodeBookStates;
using bookorbit::ServerBookState;

TEST(BookStateDecode, DecodesRatingAndReview) {
  const std::string body = R"({
    "unmatched": [],
    "results": [
      {"hash": "abc", "ratingSet": true, "rating": 4, "ratingUpdatedAt": "2026-08-21",
       "reviewNoteSet": true, "reviewNote": "Great fun.", "reviewUpdatedAt": "2026-08-22"}
    ]
  })";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].hash, "abc");
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_TRUE(results[0].ratingSet);
  EXPECT_EQ(results[0].rating, 4);
  EXPECT_EQ(results[0].ratingUpdatedAt.day, 21);
  EXPECT_TRUE(results[0].reviewKnown);
  EXPECT_TRUE(results[0].reviewNoteSet);
  EXPECT_EQ(results[0].reviewNote, "Great fun.");
  EXPECT_EQ(results[0].reviewUpdatedAt.day, 22);
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookStateDecode, DecodesUnmatchedHashes) {
  const std::string body = R"({"unmatched":["abc","def"],"results":[]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(unmatched.size(), 2u);
  EXPECT_EQ(unmatched[0], "abc");
  EXPECT_EQ(unmatched[1], "def");
  EXPECT_TRUE(results.empty());
}

// ratingSet:false means "the server holds no rating" — a definitive answer,
// distinct from the field being absent, which means "no opinion".
TEST(BookStateDecode, RatingSetFalseIsKnownAndUnset) {
  const std::string body = R"({"results":[{"hash":"abc","ratingSet":false}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].ratingSet);
  EXPECT_EQ(results[0].rating, 0);
}

TEST(BookStateDecode, AbsentFlagsLeaveTheFieldUnknown) {
  const std::string body = R"({"results":[{"hash":"abc"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].reviewKnown);
}

TEST(BookStateDecode, ReviewNoteSetFalseIsKnownAndEmpty) {
  const std::string body = R"({"results":[{"hash":"abc","reviewNoteSet":false,"reviewUpdatedAt":"2026-08-22"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].reviewKnown);
  EXPECT_FALSE(results[0].reviewNoteSet);
  EXPECT_TRUE(results[0].reviewNote.empty());
  EXPECT_EQ(results[0].reviewUpdatedAt.day, 22);
}

TEST(BookStateDecode, AcceptsTimestampsInUpdatedAtFields) {
  const std::string body =
      R"({"results":[{"hash":"abc","ratingSet":true,"rating":3,"ratingUpdatedAt":"2026-08-21T09:14:00Z"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].ratingUpdatedAt.year, 2026);
  EXPECT_EQ(results[0].ratingUpdatedAt.month, 8);
  EXPECT_EQ(results[0].ratingUpdatedAt.day, 21);
}

TEST(BookStateDecode, DecodesSeveralResults) {
  const std::string body = R"({"results":[
    {"hash":"abc","ratingSet":true,"rating":1},
    {"hash":"def","ratingSet":true,"rating":5}
  ]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].rating, 1);
  EXPECT_EQ(results[1].hash, "def");
  EXPECT_EQ(results[1].rating, 5);
}

TEST(BookStateDecode, RejectsMalformedJson) {
  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  EXPECT_FALSE(decodeBookStates("{not json", unmatched, results));
  EXPECT_TRUE(results.empty());
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookStateDecode, EmptyBodyObjectIsSuccessWithNothing) {
  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates("{}", unmatched, results));
  EXPECT_TRUE(results.empty());
  EXPECT_TRUE(unmatched.empty());
}

// Out-of-range ratings are dropped rather than stored, matching the encoder.
TEST(BookStateDecode, DropsOutOfRangeRating) {
  const std::string body = R"({"results":[{"hash":"abc","ratingSet":true,"rating":9}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].ratingSet);
}

TEST(BookStateDecode, ResultsWithoutHashAreDropped) {
  const std::string body = R"({"results":[{"ratingSet":true,"rating":3}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  EXPECT_TRUE(results.empty());
}
