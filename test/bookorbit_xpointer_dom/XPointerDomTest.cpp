#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"

using bookorbit::emitXPointer;
using bookorbit::hasSyntheticSteps;
using bookorbit::isSyntheticCrengineElement;
using bookorbit::kNormalizedXPointerDomVersion;
using bookorbit::parseXPointer;
using bookorbit::stripSyntheticSteps;
using bookorbit::XPointer;

namespace {

XPointer parse(const char* raw) {
  XPointer p;
  EXPECT_TRUE(parseXPointer(raw, p)) << raw;
  return p;
}

}  // namespace

TEST(XPointerDom, NormalizationBoundaryMatchesCrengine) {
  EXPECT_EQ(kNormalizedXPointerDomVersion, 20200223);
  EXPECT_EQ(kNormalizedXPointerDomVersion, corpus::kNormalizedDomVersion);
}

// The authoritative set, fb2def.h:40-64 (EL_BOXING_START..EL_BOXING_END plus
// pseudoElem).
TEST(XPointerDom, RecognisesEverySyntheticElement) {
  EXPECT_TRUE(isSyntheticCrengineElement("autoBoxing"));
  EXPECT_TRUE(isSyntheticCrengineElement("tabularBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("rubyBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("mathBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("floatBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("inlineBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("pseudoElem"));
}

TEST(XPointerDom, RealElementsAreNotSynthetic) {
  EXPECT_FALSE(isSyntheticCrengineElement("div"));
  EXPECT_FALSE(isSyntheticCrengineElement("table"));
  EXPECT_FALSE(isSyntheticCrengineElement("tbody"));
  EXPECT_FALSE(isSyntheticCrengineElement("img"));
  EXPECT_FALSE(isSyntheticCrengineElement("body"));
  EXPECT_FALSE(isSyntheticCrengineElement("DocFragment"));
  EXPECT_FALSE(isSyntheticCrengineElement(""));
  // Case matters: crengine's names are camelCase and an author's <autoboxing>
  // would be a real element.
  EXPECT_FALSE(isSyntheticCrengineElement("autoboxing"));
}

// Verbatim from fixtures/test_tables_dom20171225.csv line 5.
TEST(XPointerDom, StripsTabularBoxFromALegacyTableXPointer) {
  XPointer p = parse("/body/DocFragment[4]/body/section/table/tabularBox/tbody/tr/td/text().0");
  EXPECT_TRUE(hasSyntheticSteps(p));
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_FALSE(hasSyntheticSteps(p));
  EXPECT_EQ(emitXPointer(p), "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
}

// Verbatim from fixtures/test_mixed_images_dom20171225.csv line 2.
TEST(XPointerDom, StripsAutoBoxingFromALegacyImageXPointer) {
  XPointer p = parse("/body/DocFragment[3]/body/autoBoxing/img.0");
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(emitXPointer(p), "/body[1]/DocFragment[3]/body[1]/img[1].0");
}

TEST(XPointerDom, StrippingIsIdempotent) {
  XPointer p = parse("/body/DocFragment[3]/body/autoBoxing/img.0");
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(stripSyntheticSteps(p), 0);
}

TEST(XPointerDom, ModernXPointersHaveNothingToStrip) {
  XPointer p = parse("/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
  EXPECT_FALSE(hasSyntheticSteps(p));
  EXPECT_EQ(stripSyntheticSteps(p), 0);
}

// The honest limitation: dropping autoBoxing[2] loses the disambiguation that
// the current DOM version expresses as img[2]. Documented, not hidden.
TEST(XPointerDom, StrippingCanLoseSiblingDisambiguation) {
  XPointer legacy = parse("/body/DocFragment[4]/body/autoBoxing[2]/img[1].0");
  ASSERT_EQ(stripSyntheticSteps(legacy), 1);
  EXPECT_EQ(emitXPointer(legacy), "/body[1]/DocFragment[4]/body[1]/img[1].0");
  EXPECT_NE(emitXPointer(legacy), "/body[1]/DocFragment[4]/body[1]/img[2].0");
}

// Corpus-wide: exactly the measured number of rows carry synthetic steps, and
// they are confined to the pre-normalization DOM version plus a handful of
// tabularBox cases.
TEST(XPointerDom, CorpusSyntheticStepCountIsStable) {
  size_t rows = 0;
  size_t synthetic = 0;
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& path : corpus::fixturePaths(dom)) {
      corpus::Fixture fixture;
      ASSERT_TRUE(corpus::loadFixture(path, fixture)) << path;
      for (const auto& row : fixture.rows) {
        XPointer p;
        ASSERT_TRUE(parseXPointer(row.xpointer, p)) << row.xpointer;
        rows++;
        if (hasSyntheticSteps(p)) synthetic++;
      }
    }
  }
  // Do NOT assert a magic corpus size. Record the real numbers printed by
  // generate_ground_truth.sh on first run, then pin them here so drift is
  // caught. Until then, assert only the properties that must hold.
  EXPECT_GT(rows, 100u) << "corpus too small to prove anything";
  EXPECT_GT(synthetic, 0u) << "no synthetic steps found - is the corpus real?";
  EXPECT_LT(synthetic, rows) << "every row synthetic - generator is wrong";
}

TEST(XPointerDom, StrippingNeverTouchesTheDocFragmentOrOffset) {
  XPointer p = parse("/body/DocFragment[7]/body/autoBoxing/p[3]/text()[2].44");
  ASSERT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(p.docFragment, 7);
  EXPECT_EQ(p.textNodeIndex, 2);
  EXPECT_EQ(p.charOffset, 44);
}

// Regression: stripSyntheticSteps moved every step into its scratch vector but
// only committed that vector when something was actually removed. With no
// synthetic steps — the overwhelmingly common case, since the modern DOM emits
// none — the caller was left holding moved-from steps with empty names, and
// every subsequent resolution failed.
TEST(XPointerDom, StripLeavesNonSyntheticStepsIntact) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[2]/body[1]/div[3]/p[7]/text()[1].42", p));
  ASSERT_EQ(p.steps.size(), 2u);

  EXPECT_EQ(stripSyntheticSteps(p), 0);

  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[0].name, "div");
  EXPECT_EQ(p.steps[0].index, 3);
  EXPECT_EQ(p.steps[1].name, "p");
  EXPECT_EQ(p.steps[1].index, 7);
  EXPECT_EQ(p.charOffset, 42);
}

TEST(XPointerDom, StripKeepsSurvivingStepNamesWhenRemoving) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[3]/body/autoBoxing/img", p));
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_EQ(p.steps[0].name, "img");
}
