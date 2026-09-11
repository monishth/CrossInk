#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitUrl.h"

using bookorbit::joinPath;
using bookorbit::normalizeServerUrl;

TEST(BookOrbitUrl, AppendsApiV1WhenAbsent) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, StripsTrailingSlashesBeforeAppending) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com///"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, KeepsExistingApiV1) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com/api/v1"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, RewritesLegacyKoreaderSuffix) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com/api/v1/koreader"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, TrimsSurroundingWhitespace) {
  EXPECT_EQ(normalizeServerUrl("  https://books.example.com  "), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, RejectsEmptyInput) {
  EXPECT_EQ(normalizeServerUrl(""), "");
  EXPECT_EQ(normalizeServerUrl("   "), "");
}

TEST(BookOrbitUrl, JoinsPathOntoBase) {
  EXPECT_EQ(joinPath("https://books.example.com/api/v1", "/koreader/users/auth"),
            "https://books.example.com/api/v1/koreader/users/auth");
}
