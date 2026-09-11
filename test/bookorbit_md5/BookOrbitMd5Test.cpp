#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::md5Hex;

TEST(BookOrbitMd5, RfcTestVectors) {
  EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
  EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
  EXPECT_EQ(md5Hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
}

// Crosses the 56-byte padding boundary, so the two-block tail path runs.
TEST(BookOrbitMd5, SpansTwoPaddingBlocks) {
  EXPECT_EQ(md5Hex("The quick brown fox jumps over the lazy dog"), "9e107d9d372bb6826bd81d3542a419d6");
}

TEST(BookOrbitMd5, HandlesManyBlocks) { EXPECT_EQ(md5Hex(std::string(1000, 'x')), "398533d48111e9f664b1f64cb10c4b63"); }

TEST(BookOrbitMd5, OutputIsAlwaysThirtyTwoLowercaseHexChars) {
  const std::string digest = md5Hex("anything");
  ASSERT_EQ(digest.size(), 32u);
  for (const char ch : digest) {
    EXPECT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) << "non-hex char: " << ch;
  }
}

TEST(BookOrbitMd5, HandlesEmbeddedNulBytes) {
  const std::string withNul("a\0b", 3);
  EXPECT_NE(md5Hex(withNul), md5Hex("ab"));
}

// These are the real BookOrbit annotation keys for canonical xpointers:
// md5(datetime .. "|" .. pos0), matching BookOrbitAnnotations.buildKey().
TEST(BookOrbitMd5, MatchesBookOrbitAnnotationKeys) {
  EXPECT_EQ(md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17"),
            "08759494897afa79aec0d37d83498d30");
  EXPECT_EQ(md5Hex("2026-08-21 09:15:42|/body[1]/DocFragment[2]/body[1]/div[1]/p[7]/text()[1].0"),
            "256f02a78307c97781079db2c35e4397");
}

// A one-character difference in the char offset is a different annotation.
TEST(BookOrbitMd5, CharOffsetChangesTheKey) {
  EXPECT_NE(md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17"),
            md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].99"));
}
