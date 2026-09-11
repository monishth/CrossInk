#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateCodec.h"

using bookorbit::BookStatePayload;
using bookorbit::BookStatus;
using bookorbit::buildStatePayload;
using bookorbit::encodeBookStates;
using bookorbit::LocalBookState;
using bookorbit::SyncedBookState;

namespace {

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";

}  // namespace

TEST(BookStateEncode, BatchSizeIsTwoHundred) { EXPECT_EQ(bookorbit::kBookStateBatchSize, 200u); }

TEST(BookStateEncode, EncodesStatusAndDate) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasStatus = true;
  payload.status = BookStatus::Complete;
  payload.statusModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("books":[)"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("hash":")") + kHash + '"'), std::string::npos);
  EXPECT_NE(json.find(R"("status":"complete")"), std::string::npos);
  EXPECT_NE(json.find(R"("statusModified":"2026-08-21")"), std::string::npos);
}

TEST(BookStateEncode, EncodesRatingAsAnInteger) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasRating = true;
  payload.rating = 4;
  payload.statusModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("rating":4)"), std::string::npos);
  EXPECT_EQ(json.find("ratingCleared"), std::string::npos);
}

// Clearing is explicit. An absent field means "unchanged", so a cleared rating
// must ride on ratingCleared:true or the server keeps the old value forever.
TEST(BookStateEncode, ClearingRatingIsExplicit) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.ratingCleared = true;
  payload.statusModified = day("2026-08-22");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("ratingCleared":true)"), std::string::npos);
  EXPECT_EQ(json.find(R"("rating":)"), std::string::npos);
}

TEST(BookStateEncode, ClearingReviewIsExplicit) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.reviewCleared = true;
  payload.reviewModified = day("2026-08-22");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("reviewCleared":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("reviewModified":"2026-08-22")"), std::string::npos);
  EXPECT_EQ(json.find(R"("reviewNote":)"), std::string::npos);
}

TEST(BookStateEncode, EscapesReviewText) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasReview = true;
  payload.reviewNote = "Line \"one\"\nLine two";
  payload.reviewModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"(Line \"one\"\nLine two)"), std::string::npos);
}

TEST(BookStateEncode, EncodesSeveralBooksInOneArray) {
  BookStatePayload first;
  first.hash = "aaa";
  first.hasStatus = true;
  first.status = BookStatus::Reading;
  first.statusModified = day("2026-08-21");
  BookStatePayload second;
  second.hash = "bbb";
  second.hasStatus = true;
  second.status = BookStatus::Abandoned;
  second.statusModified = day("2026-08-20");

  const std::string json = encodeBookStates({first, second});
  EXPECT_NE(json.find(R"({"hash":"aaa")"), std::string::npos);
  EXPECT_NE(json.find(R"({"hash":"bbb")"), std::string::npos);
  EXPECT_NE(json.find(R"("status":"abandoned")"), std::string::npos);
}

TEST(BookStateEncode, EmptyBatchStillEncodesAnArray) { EXPECT_EQ(encodeBookStates({}), R"({"books":[]})"); }

TEST(BookStateEncode, NothingChangedProducesNoPayload) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  EXPECT_FALSE(buildStatePayload(kHash, local, synced, false, payload));
}

// A forced pull is the only way a rating written on the web reaches the device.
TEST(BookStateEncode, ForcedPullSendsHashOnly) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, true, payload));
  EXPECT_EQ(payload.hash, kHash);
  EXPECT_FALSE(payload.hasStatus);
  EXPECT_FALSE(payload.hasRating);
  EXPECT_FALSE(payload.ratingCleared);
  EXPECT_FALSE(payload.hasReview);
}

TEST(BookStateEncode, ChangedStatusDateProducesAStatusPayload) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-22"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasStatus);
  EXPECT_EQ(payload.status, BookStatus::Complete);
  EXPECT_EQ(payload.statusModified.day, 22);
}

TEST(BookStateEncode, FirstRatingIsAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-21"));
  SyncedBookState synced;  // nothing known server-side yet

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasRating);
  EXPECT_EQ(payload.rating, 5);
  EXPECT_EQ(payload.statusModified.day, 21);
}

TEST(BookStateEncode, SameRatingIsNotAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");
  synced.ratingKnown = true;
  synced.ratingSet = true;
  synced.rating = 5;

  BookStatePayload payload;
  EXPECT_FALSE(buildStatePayload(kHash, local, synced, false, payload));
}

TEST(BookStateEncode, RemovingAKnownRatingProducesRatingCleared) {
  LocalBookState local;  // no rating locally
  local.statusModified = day("2026-08-22");
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-22");
  synced.ratingKnown = true;
  synced.ratingSet = true;
  synced.rating = 5;

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.ratingCleared);
  EXPECT_FALSE(payload.hasRating);
}

TEST(BookStateEncode, ChangedReviewProducesAReviewPayload) {
  LocalBookState local;
  local.setReview("Better than expected.", day("2026-08-22"));
  SyncedBookState synced;
  synced.reviewKnown = true;
  synced.reviewSet = true;
  synced.reviewNote = "Old note.";

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasReview);
  EXPECT_EQ(payload.reviewNote, "Better than expected.");
  EXPECT_EQ(payload.reviewModified.day, 22);
}

TEST(BookStateEncode, RemovingAKnownReviewProducesReviewCleared) {
  LocalBookState local;
  local.reviewModified = day("2026-08-22");
  SyncedBookState synced;
  synced.reviewKnown = true;
  synced.reviewSet = true;
  synced.reviewNote = "Old note.";

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.reviewCleared);
  EXPECT_FALSE(payload.hasReview);
}
