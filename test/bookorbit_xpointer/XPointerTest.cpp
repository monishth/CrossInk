#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"

using bookorbit::buildCanonicalXPointer;
using bookorbit::emitXPointer;
using bookorbit::isChapterStartXPointer;
using bookorbit::normalizeXPointer;
using bookorbit::parseXPointer;
using bookorbit::rewriteDocFragmentIndex;
using bookorbit::sameLocation;
using bookorbit::XPointer;
using bookorbit::xpointerDocFragmentIndex;
using bookorbit::XPointerStep;

namespace {

// Verbatim reproduction of the matching rule at ProgressMapper.cpp:62 as it
// stands before this task. Kept in the test forever as the defect record.
bool legacyLiteralMatch(const std::string& xpath) { return xpath.find("/body/DocFragment[") != std::string::npos; }

std::vector<corpus::Fixture> loadAll(const int domVersion) {
  std::vector<corpus::Fixture> fixtures;
  for (const auto& path : corpus::fixturePaths(domVersion)) {
    corpus::Fixture fixture;
    EXPECT_TRUE(corpus::loadFixture(path, fixture)) << path;
    fixtures.push_back(fixture);
  }
  return fixtures;
}

}  // namespace

// ---- The defect, proven against generated ground truth ---------------------

// Every xpointer a current KOReader writes misses the literal search. This is
// the whole bug, measured rather than argued.
TEST(XPointerDefect, LegacyLiteralMatchMissesEveryModernXPointer) {
  size_t rows = 0;
  size_t missed = 0;
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      rows++;
      if (!legacyLiteralMatch(row.xpointer)) missed++;
    }
  }
  ASSERT_GT(rows, 150u);
  EXPECT_EQ(missed, rows);
}

TEST(XPointerDefect, LegacyLiteralMatchMissesBothRealSidecarXPointers) {
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_FALSE(legacyLiteralMatch(corpus::kSidecarEntries[i].raw)) << corpus::kSidecarEntries[i].provenance;
  }
}

TEST(XPointerDefect, TheGrammarMatchesWhereTheLiteralDidNot) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_GT(xpointerDocFragmentIndex(row.xpointer), 0) << row.xpointer;
    }
  }
}

// ---- Corpus round trip, both DOM versions ----------------------------------

TEST(XPointerCorpus, EveryGeneratedXPointerParses) {
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& fixture : loadAll(dom)) {
      for (const auto& row : fixture.rows) {
        XPointer parsed;
        EXPECT_TRUE(parseXPointer(row.xpointer, parsed)) << row.xpointer;
        EXPECT_TRUE(parsed.valid) << row.xpointer;
      }
    }
  }
}

// The canonical form is what the device emits, so it must be stable under
// re-normalization or a pushed position would drift on every sync.
TEST(XPointerCorpus, CanonicalFormIsAFixedPoint) {
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& fixture : loadAll(dom)) {
      for (const auto& row : fixture.rows) {
        const std::string once = normalizeXPointer(row.xpointer);
        ASSERT_FALSE(once.empty()) << row.xpointer;
        EXPECT_EQ(normalizeXPointer(once), once) << row.xpointer;
      }
    }
  }
}

// crengine already emits the canonical form at the latest DOM version, so
// normalization must be the identity there.
TEST(XPointerCorpus, LatestDomVersionNormalizesToItself) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_EQ(normalizeXPointer(row.xpointer), row.xpointer) << row.xpointer;
    }
  }
}

TEST(XPointerCorpus, EmitIsTheInverseOfParse) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      XPointer parsed;
      ASSERT_TRUE(parseXPointer(row.xpointer, parsed)) << row.xpointer;
      EXPECT_EQ(emitXPointer(parsed), row.xpointer) << row.xpointer;
    }
  }
}

TEST(XPointerCorpus, NormalizationNeverCollapsesDistinctPositions) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    std::set<std::string> normalized;
    for (const auto& row : fixture.rows) normalized.insert(normalizeXPointer(row.xpointer));
    EXPECT_EQ(normalized.size(), fixture.rows.size()) << fixture.epub;
  }
}

TEST(XPointerCorpus, SidecarEntriesNormalizeToTheirCanonicalForm) {
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_EQ(normalizeXPointer(corpus::kSidecarEntries[i].raw), corpus::kSidecarEntries[i].canonical)
        << corpus::kSidecarEntries[i].provenance;
  }
}

// ---- Grammar details -------------------------------------------------------

TEST(XPointer, ParsesIndexedStepsAndOffset) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0", p));
  EXPECT_EQ(p.docFragment, 1);
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[0].name, "div");
  EXPECT_EQ(p.steps[0].index, 1);
  EXPECT_EQ(p.steps[1].name, "svg");
  EXPECT_EQ(p.steps[1].index, 1);
  EXPECT_EQ(p.textNodeIndex, 0);
  EXPECT_EQ(p.charOffset, 0);
}

// The oldest DOM version's shape, taken verbatim from
// fixtures/test_reader_rendering_matrix_dom20171225.csv line 1.
TEST(XPointer, UnindexedStepsDefaultToOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[1]/body/h1/text().0", p));
  EXPECT_EQ(p.docFragment, 1);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_EQ(p.steps[0].name, "h1");
  EXPECT_EQ(p.steps[0].index, 1);
  EXPECT_EQ(p.textNodeIndex, 1);
  EXPECT_EQ(p.charOffset, 0);
}

TEST(XPointer, TextNodeWithoutIndexIsNodeOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[2]/body/p[8]/text().85", p));
  EXPECT_EQ(p.textNodeIndex, 1);
  EXPECT_EQ(p.charOffset, 85);
}

TEST(XPointer, ExplicitTextNodeIndexSurvives) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[3]/body[1]/ul[1]/li[4]/text()[2].51", p));
  EXPECT_EQ(p.textNodeIndex, 2);
  EXPECT_EQ(p.charOffset, 51);
}

TEST(XPointer, MissingOffsetIsNegativeOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[1]/body/p[5]", p));
  EXPECT_EQ(p.charOffset, -1);
}

TEST(XPointer, RejectsGarbage) {
  XPointer p;
  EXPECT_FALSE(parseXPointer("", p));
  EXPECT_FALSE(parseXPointer("not-an-xpointer", p));
  EXPECT_FALSE(parseXPointer("/html/body/p[1]", p));
  EXPECT_EQ(normalizeXPointer("/html/body/p[1]"), "");
}

TEST(XPointer, IndexedAndUnindexedFormsAreTheSameLocation) {
  EXPECT_TRUE(sameLocation("/body/DocFragment[1]/body/p[5]", "/body[1]/DocFragment[1]/body[1]/p[5]"));
  EXPECT_FALSE(sameLocation("/body/DocFragment[1]/body/p[5]", "/body[1]/DocFragment[1]/body[1]/p[6]"));
  EXPECT_FALSE(sameLocation("garbage", "garbage"));
}

TEST(XPointer, ChapterStartRecognisedInBothForms) {
  EXPECT_TRUE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1]"));
  EXPECT_TRUE(isChapterStartXPointer("/body/DocFragment[12]"));
  EXPECT_TRUE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1].0"));
  EXPECT_FALSE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1]/p[2]"));
  EXPECT_FALSE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1].17"));
}

TEST(XPointer, RewritesDocFragmentIndexInBothForms) {
  std::string indexed = "/body[1]/DocFragment[1]/body[1]/p[5]";
  ASSERT_TRUE(rewriteDocFragmentIndex(indexed, 7));
  EXPECT_EQ(indexed, "/body[1]/DocFragment[7]/body[1]/p[5]");

  std::string unindexed = "/body/DocFragment[1]/body/p[5]";
  ASSERT_TRUE(rewriteDocFragmentIndex(unindexed, 7));
  EXPECT_EQ(unindexed, "/body[1]/DocFragment[7]/body[1]/p[5]");

  std::string bad = "nope";
  EXPECT_FALSE(rewriteDocFragmentIndex(bad, 7));
  EXPECT_EQ(bad, "nope");
}

TEST(XPointer, BuildCanonicalMatchesTheEmitter) {
  const std::vector<XPointerStep> steps = {{"section", 1}, {"table", 1}, {"tbody", 1}, {"tr", 1}, {"td", 1}};
  EXPECT_EQ(buildCanonicalXPointer(3, steps, 1, 0),
            "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
  EXPECT_EQ(buildCanonicalXPointer(3, steps, 0, -1),
            "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]");
  EXPECT_EQ(buildCanonicalXPointer(0, {}, 0, -1), "/body[1]/DocFragment[1]/body[1]");
}
