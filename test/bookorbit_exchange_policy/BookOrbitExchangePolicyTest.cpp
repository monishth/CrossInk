#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "lib/BookOrbit/BookOrbitExchangePolicy.h"

using bookorbit::canSkipExchange;
using bookorbit::kExchangeMaxAgeSeconds;
using bookorbit::rememberExchanged;

namespace {
constexpr char kSignature[] = "2:2026-09-11 15:00:00:3677518418:4274252292";
}

TEST(BookOrbitExchangePolicy, NeverExchangedCannotSkip) {
  char stored[48] = {};
  EXPECT_FALSE(canSkipExchange(stored, 0, kSignature, 2000000000u));
}

TEST(BookOrbitExchangePolicy, MatchingSignatureInsideTheWindowSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  ASSERT_TRUE(rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u));
  EXPECT_TRUE(canSkipExchange(stored, exchangedAt, kSignature, 2000000000u + kExchangeMaxAgeSeconds - 1));
}

// The exchange is the only channel that delivers web-created highlights, so a
// book may not skip forever even when nothing changed locally.
TEST(BookOrbitExchangePolicy, StaleStampForcesAnExchange) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, kSignature, 2000000000u + kExchangeMaxAgeSeconds));
}

TEST(BookOrbitExchangePolicy, ChangedSignatureForcesAnExchange) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, "3:2026-09-12 08:00:00:1:2", 2000000000u + 10u));
}

TEST(BookOrbitExchangePolicy, EmptySignatureNeverSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, "", 2000000000u + 10u));
}

// A clock that jumped backwards must not look like a fresh exchange.
TEST(BookOrbitExchangePolicy, StampInTheFutureNeverSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, kSignature, 1999999000u));
}

// The field is char[48]. A signature that does not fit must not be half-written
// and must not stamp a skip, or the book silently stops exchanging.
TEST(BookOrbitExchangePolicy, OversizedSignatureIsRefusedAndLeavesNoStamp) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  const std::string tooLong(80, 'x');
  EXPECT_FALSE(rememberExchanged(stored, sizeof(stored), exchangedAt, tooLong, 2000000000u));
  EXPECT_EQ(stored[0], '\0');
  EXPECT_EQ(exchangedAt, 0u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, tooLong, 2000000000u));
}

TEST(BookOrbitExchangePolicy, RememberIsNulTerminatedAndExact) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_STREQ(stored, kSignature);
  EXPECT_EQ(exchangedAt, 2000000000u);
}
