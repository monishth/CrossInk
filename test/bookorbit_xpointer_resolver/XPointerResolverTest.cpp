#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"
#include "lib/BookOrbit/XPointerResolver.h"

using bookorbit::emitXPointer;
using bookorbit::parseXPointer;
using bookorbit::resolveOffsetToXPointer;
using bookorbit::resolveXPointerToOffset;
using bookorbit::stripSyntheticSteps;
using bookorbit::visibleTextLength;
using bookorbit::XPointer;

namespace {

// Hand-countable document. Visible codepoints, in order:
//   h1        "ABCDE"  -> 0..4
//   div/p[1]  "12345"  -> 5..9
//   div/p[2]  "67890"  -> 10..14
//   p[1]      "XYZ"    -> 15..17
// <title> is inside <head> and contributes nothing.
constexpr char kDoc[] =
    "<html><head><title>IGNORED</title></head><body>"
    "<h1>ABCDE</h1>"
    "<div><p>12345</p><p>67890</p></div>"
    "<p>XYZ</p>"
    "</body></html>";

uint32_t offsetOf(const char* xpointer) {
  XPointer target;
  EXPECT_TRUE(parseXPointer(xpointer, target));
  uint32_t offset = 0xFFFFFFFFu;
  EXPECT_TRUE(resolveXPointerToOffset(kDoc, target, offset));
  return offset;
}

}  // namespace

TEST(XPointerResolver, CountsOnlyVisibleText) {
  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(kDoc, length));
  EXPECT_EQ(length, 18u);
}

TEST(XPointerResolver, ResolvesTopLevelElement) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/h1[1]"), 0u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]"), 5u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/p[1]"), 15u);
}

// Sibling counting is per parent, not per document: the div's two <p> children
// are p[1] and p[2] even though a third <p> follows at body level.
TEST(XPointerResolver, SiblingIndicesAreScopedToTheirParent) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[1]"), 5u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2]"), 10u);
}

TEST(XPointerResolver, UnindexedFormResolvesIdentically) {
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/div/p[2]"), 10u);
}

TEST(XPointerResolver, CharacterOffsetIsAddedToTheTextNodeStart) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2]/text()[1].2"), 12u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2].3"), 13u);
}

TEST(XPointerResolver, MissingElementFails) {
  XPointer target;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/div[9]/p[1]", target));
  uint32_t offset = 0;
  EXPECT_FALSE(resolveXPointerToOffset(kDoc, target, offset));
}

TEST(XPointerResolver, OffsetResolvesToCanonicalXPointer) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 12u, 0, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[1]/body[1]/div[1]/p[2]/text()[1].2");
}

TEST(XPointerResolver, OffsetZeroResolvesToTheFirstVisibleElement) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 0u, 0, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0");
}

TEST(XPointerResolver, SpineIndexBecomesTheDocFragmentIndex) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 16u, 7, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[8]/body[1]/p[1]/text()[1].1");
}

TEST(XPointerResolver, OffsetPastTheEndFails) {
  XPointer out;
  EXPECT_FALSE(resolveOffsetToXPointer(kDoc, 999u, 0, out));
}

TEST(XPointerResolver, RoundTripsEveryVisibleOffsetExactly) {
  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(kDoc, length));
  for (uint32_t offset = 0; offset < length; offset++) {
    XPointer out;
    ASSERT_TRUE(resolveOffsetToXPointer(kDoc, offset, 0, out)) << offset;
    uint32_t back = 0xFFFFFFFFu;
    ASSERT_TRUE(resolveXPointerToOffset(kDoc, out, back)) << emitXPointer(out);
    EXPECT_EQ(back, offset) << emitXPointer(out);
  }
}

// ---- The corpus test: real KOReader xpointers, real spine XHTML ------------

// Every xpointer KOReader emitted for these books, resolved against the very
// spine item it names. Expect 100% resolution across both DOM versions.
TEST(XPointerResolverCorpus, ResolvesEveryGeneratedXPointer) {
  size_t rows = 0;
  size_t resolved = 0;

  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& name : corpus::epubNames()) {
      corpus::Fixture fixture;
      ASSERT_TRUE(corpus::loadFixture(corpus::fixturePath(name, dom), fixture)) << name;

      for (const auto& row : fixture.rows) {
        XPointer target;
        ASSERT_TRUE(parseXPointer(row.xpointer, target)) << row.xpointer;
        // Synthetic boxing elements exist only in crengine's DOM, never in the
        // source XHTML we stream (Task 3).
        stripSyntheticSteps(target);

        const std::string xhtml = corpus::readFile(corpus::spinePath(name, target.docFragment));
        ASSERT_FALSE(xhtml.empty()) << corpus::spinePath(name, target.docFragment);

        rows++;
        uint32_t offset = 0;
        if (resolveXPointerToOffset(xhtml, target, offset)) {
          resolved++;
        } else {
          ADD_FAILURE() << name << " dom " << dom << ": " << row.xpointer;
        }
      }
    }
  }

  EXPECT_GT(rows, 100u) << "corpus too small to prove anything";
  EXPECT_EQ(resolved, rows);
}

// Resolution must respect document order: a later page is never at a smaller
// offset than an earlier page in the same spine item.
TEST(XPointerResolverCorpus, OffsetsAdvanceWithinASpineItem) {
  for (const auto& name : corpus::epubNames()) {
    corpus::Fixture fixture;
    ASSERT_TRUE(corpus::loadFixture(corpus::fixturePath(name, corpus::kLatestDomVersion), fixture)) << name;

    int previousFragment = 0;
    uint32_t previousOffset = 0;
    for (const auto& row : fixture.rows) {
      XPointer target;
      ASSERT_TRUE(parseXPointer(row.xpointer, target)) << row.xpointer;
      stripSyntheticSteps(target);

      const std::string xhtml = corpus::readFile(corpus::spinePath(name, target.docFragment));
      ASSERT_FALSE(xhtml.empty());

      uint32_t offset = 0;
      ASSERT_TRUE(resolveXPointerToOffset(xhtml, target, offset)) << row.xpointer;

      if (target.docFragment == previousFragment) {
        EXPECT_GE(offset, previousOffset) << name << ": " << row.xpointer;
      }
      previousFragment = target.docFragment;
      previousOffset = offset;
    }
  }
}

// The fidelity measurement the spec asks for: offset -> xpointer -> offset over
// every visible position of a real spine item, exactly.
TEST(XPointerResolverCorpus, RoundTripsEveryVisibleOffsetInARealSpineItem) {
  const std::string xhtml = corpus::readFile(corpus::spinePath("test_reader_rendering_matrix", 2));
  ASSERT_FALSE(xhtml.empty());

  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(xhtml, length));
  ASSERT_GT(length, 100u);

  for (uint32_t offset = 0; offset < length; offset++) {
    XPointer out;
    ASSERT_TRUE(resolveOffsetToXPointer(xhtml, offset, 1, out)) << offset;
    uint32_t back = 0xFFFFFFFFu;
    ASSERT_TRUE(resolveXPointerToOffset(xhtml, out, back)) << emitXPointer(out);
    EXPECT_EQ(back, offset) << emitXPointer(out);
  }
}

// Regression: a text node sitting directly under <body> has no element steps,
// and the forward resolver treated every zero-step xpointer as "chapter start,
// offset 0", ignoring text()[N] entirely. text()[2] therefore resolved to the
// same offset as text()[1] — a silent wrong landing, which is precisely the
// degradation this phase exists to remove.
TEST(XPointerResolver, DistinguishesTextNodesDirectlyUnderBody) {
  const std::string xhtml = "<html><body>ab<span>cd</span>ef</body></html>";

  XPointer first;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/text()[1].0", first));
  XPointer second;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/text()[2].0", second));

  uint32_t firstOffset = 0;
  uint32_t secondOffset = 0;
  ASSERT_TRUE(resolveXPointerToOffset(xhtml, first, firstOffset));
  ASSERT_TRUE(resolveXPointerToOffset(xhtml, second, secondOffset));

  EXPECT_EQ(firstOffset, 0u);   // "ab"
  EXPECT_EQ(secondOffset, 4u);  // after "ab" and "cd"
  EXPECT_NE(firstOffset, secondOffset);
}
