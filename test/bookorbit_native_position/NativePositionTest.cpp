#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/NativePosition.h"

using bookorbit::appendNativePosition;
using bookorbit::decodeNativePosition;
using bookorbit::NativePosition;

namespace {

NativePosition sample() {
  NativePosition position;
  position.pctQ = 205200u;
  position.spine = 7u;
  position.page = 3u;
  position.pages = 12u;
  position.para = 42u;
  position.xpath = "/body[1]/DocFragment[8]/body[1]/p[42]";
  position.present = true;
  return position;
}

}  // namespace

TEST(NativePosition, EncodesEveryField) {
  std::string out;
  appendNativePosition(out, sample());
  EXPECT_EQ(out,
            "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
            "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}");
}

TEST(NativePosition, AbsentPositionEncodesNothing) {
  NativePosition absent;
  std::string out;
  appendNativePosition(out, absent);
  EXPECT_EQ(out, "");
}

TEST(NativePosition, PagesNeverEncodesZero) {
  NativePosition zeroPages = sample();
  zeroPages.pages = 0u;
  std::string out;
  appendNativePosition(out, zeroPages);
  EXPECT_NE(out.find("\"pages\":1"), std::string::npos);
}

TEST(NativePosition, NormalizesTheXpathOnEncode) {
  NativePosition unindexed = sample();
  unindexed.xpath = "/body/DocFragment[8]/body/p[42]";
  std::string out;
  appendNativePosition(out, unindexed);
  EXPECT_NE(out.find("\"/body[1]/DocFragment[8]/body[1]/p[42]\""), std::string::npos);
}

TEST(NativePosition, DecodesEveryField) {
  const char* json =
      "{\"percentage\":0.2052,\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[42]\","
      "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
      "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}}";

  NativePosition out;
  ASSERT_TRUE(decodeNativePosition(json, out));
  EXPECT_TRUE(out.present);
  EXPECT_EQ(out.pctQ, 205200u);
  EXPECT_EQ(out.spine, 7u);
  EXPECT_EQ(out.page, 3u);
  EXPECT_EQ(out.pages, 12u);
  EXPECT_EQ(out.para, 42u);
  EXPECT_EQ(out.xpath, "/body[1]/DocFragment[8]/body[1]/p[42]");
}

// A server without the change simply omits the field. That is not an error.
TEST(NativePosition, MissingPositionDecodesAsAbsent) {
  NativePosition out;
  ASSERT_TRUE(decodeNativePosition("{\"percentage\":0.5,\"progress\":\"/body[1]/DocFragment[1]/body[1]\"}", out));
  EXPECT_FALSE(out.present);
}

TEST(NativePosition, TopLevelKeysNamedLikePositionKeysAreIgnored) {
  NativePosition out;
  ASSERT_TRUE(decodeNativePosition("{\"page\":99,\"spine\":99}", out));
  EXPECT_FALSE(out.present);
  EXPECT_EQ(out.page, 0u);
}

TEST(NativePosition, MalformedJsonIsRejected) {
  NativePosition out;
  EXPECT_FALSE(decodeNativePosition("{\"position\":{", out));
}

TEST(NativePosition, EncodeDecodeRoundTrip) {
  std::string body = "{\"percentage\":0.2052,";
  appendNativePosition(body, sample());
  body += '}';

  NativePosition out;
  ASSERT_TRUE(decodeNativePosition(body, out));
  EXPECT_EQ(out.pctQ, sample().pctQ);
  EXPECT_EQ(out.spine, sample().spine);
  EXPECT_EQ(out.para, sample().para);
  EXPECT_EQ(out.xpath, sample().xpath);
}
