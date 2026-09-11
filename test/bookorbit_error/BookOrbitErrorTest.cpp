#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitError.h"

using bookorbit::classify;
using bookorbit::isAuthError;
using bookorbit::isTransient;
using bookorbit::Status;

TEST(BookOrbitError, SuccessRangeIsOk) {
  EXPECT_EQ(classify(200, false).status, Status::Ok);
  EXPECT_EQ(classify(201, false).status, Status::Ok);
  EXPECT_EQ(classify(204, false).status, Status::Ok);
}

TEST(BookOrbitError, ThreeHundredIsNotOk) { EXPECT_NE(classify(300, false).status, Status::Ok); }

TEST(BookOrbitError, AuthErrorsAreClassifiedAndFlagged) {
  EXPECT_EQ(classify(401, false).status, Status::Unauthorized);
  EXPECT_EQ(classify(403, false).status, Status::Unauthorized);
  EXPECT_TRUE(isAuthError(classify(401, false)));
  EXPECT_TRUE(isAuthError(classify(403, false)));
  EXPECT_FALSE(isAuthError(classify(404, false)));
}

TEST(BookOrbitError, NotFoundIsItsOwnStatus) { EXPECT_EQ(classify(404, false).status, Status::NotFound); }

TEST(BookOrbitError, ServerErrorsAreTransient) {
  EXPECT_EQ(classify(500, false).status, Status::ServerError);
  EXPECT_TRUE(isTransient(classify(500, false)));
  EXPECT_TRUE(isTransient(classify(503, false)));
}

TEST(BookOrbitError, TransportFailureIsTransientRegardlessOfStatus) {
  const auto err = classify(0, true);
  EXPECT_EQ(err.status, Status::Transport);
  EXPECT_TRUE(isTransient(err));
}

// A definitive 4xx means the server understood and said no. It must NOT be
// treated as transient, or a capability would never cache a negative.
TEST(BookOrbitError, ClientErrorsAreNotTransient) {
  EXPECT_FALSE(isTransient(classify(400, false)));
  EXPECT_FALSE(isTransient(classify(404, false)));
  EXPECT_FALSE(isTransient(classify(422, false)));
}

TEST(BookOrbitError, HttpStatusIsPreserved) { EXPECT_EQ(classify(418, false).httpStatus, 418); }
