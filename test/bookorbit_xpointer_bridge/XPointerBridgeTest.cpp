#include <gtest/gtest.h>

#include <cstring>

#include "lib/BookOrbit/XPointer.h"

using bookorbit::toXPathSteps;

namespace {

constexpr int kMaxDepth = 16;

// Thin wrapper so the tests below read as the call ProgressMapper will make.
bool toXPointerBridgeCall(char (&tags)[kMaxDepth][12], int (&indices)[kMaxDepth], int& count, const char* raw) {
  return toXPathSteps(raw, tags, indices, kMaxDepth, count);
}

}  // namespace

TEST(XPointerBridge, FillsFixedSizeStepArrayFromIndexedForm) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(
      toXPointerBridgeCall(tags, indices, count, "/body[1]/DocFragment[1]/body[1]/div[1]/ul[1]/li[4]/text()[1].51"));
  ASSERT_EQ(count, 3);
  EXPECT_STREQ(tags[0], "div");
  EXPECT_EQ(indices[0], 1);
  EXPECT_STREQ(tags[1], "ul");
  EXPECT_EQ(indices[1], 1);
  EXPECT_STREQ(tags[2], "li");
  EXPECT_EQ(indices[2], 4);
}

TEST(XPointerBridge, FillsFixedSizeStepArrayFromUnindexedForm) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(toXPointerBridgeCall(tags, indices, count, "/body/DocFragment[1]/body/div/ul/li[4]"));
  ASSERT_EQ(count, 3);
  EXPECT_STREQ(tags[0], "div");
  EXPECT_EQ(indices[0], 1);
  EXPECT_STREQ(tags[2], "li");
  EXPECT_EQ(indices[2], 4);
}

TEST(XPointerBridge, TruncatesOverlongTagNamesSafely) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(toXPointerBridgeCall(tags, indices, count, "/body[1]/DocFragment[1]/body[1]/averyverylongtagname[2]"));
  ASSERT_EQ(count, 1);
  EXPECT_EQ(std::strlen(tags[0]), 11u);
  EXPECT_STREQ(tags[0], "averyverylo");
  EXPECT_EQ(indices[0], 2);
}

TEST(XPointerBridge, StopsAtCapacity) {
  char tags[2][12] = {};
  int indices[2] = {};
  int count = 0;

  EXPECT_TRUE(toXPathSteps("/body[1]/DocFragment[1]/body[1]/a[1]/b[1]/c[1]", tags, indices, 2, count));
  EXPECT_EQ(count, 2);
}

TEST(XPointerBridge, RejectsNonXPointers) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 7;

  EXPECT_FALSE(toXPathSteps("nonsense", tags, indices, kMaxDepth, count));
  EXPECT_EQ(count, 0);
}

TEST(XPointerBridge, ChapterStartYieldsZeroSteps) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 7;

  ASSERT_TRUE(toXPathSteps("/body[1]/DocFragment[9]/body[1]", tags, indices, kMaxDepth, count));
  EXPECT_EQ(count, 0);
}
