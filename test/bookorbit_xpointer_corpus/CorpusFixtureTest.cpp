#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"

using corpus::Fixture;
using corpus::kLatestDomVersion;
using corpus::kNormalizedDomVersion;
using corpus::kOldestDomVersion;
using corpus::loadFixture;

namespace {

// Total rows the generator produces across all 13 EPUBs at one DOM version.
// Corpus size depends on the fixture EPUBs; record the real figure on first run.
constexpr size_t kRowsPerDomVersion = 202;

std::vector<Fixture> loadAll(const int domVersion) {
  std::vector<Fixture> fixtures;
  for (const auto& path : corpus::fixturePaths(domVersion)) {
    Fixture fixture;
    EXPECT_TRUE(loadFixture(path, fixture)) << path;
    fixtures.push_back(fixture);
  }
  return fixtures;
}

}  // namespace

TEST(CorpusFixture, EveryEpubHasAFixtureAtBothDomVersions) {
  for (const auto& name : corpus::epubNames()) {
    Fixture oldest;
    Fixture latest;
    EXPECT_TRUE(loadFixture(corpus::fixturePath(name, kOldestDomVersion), oldest)) << name;
    EXPECT_TRUE(loadFixture(corpus::fixturePath(name, kLatestDomVersion), latest)) << name;
  }
}

TEST(CorpusFixture, HeadersRecordTheDomVersion) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    EXPECT_EQ(fixture.domVersion, kLatestDomVersion) << fixture.epub;
    EXPECT_EQ(fixture.domVersionWithNormalizedXPointers, kNormalizedDomVersion) << fixture.epub;
  }
}

TEST(CorpusFixture, TotalRowCountMatchesTheGenerator) {
  size_t rows = 0;
  for (const auto& fixture : loadAll(kLatestDomVersion)) rows += fixture.rows.size();
  EXPECT_EQ(rows, kRowsPerDomVersion);
}

// The guard that matters. The original hand-written corpus degenerated to a
// single cover-page xpointer repeated twice; if the generator ever regresses to
// that, this fails loudly rather than leaving a corpus that proves nothing.
// The invariant is per-fixture, not global. Distinct books legitimately share a
// structurally identical first page: 11 of the 13 fixture EPUBs open on
// "/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0", so the corpus has 175
// distinct xpointers across 202 rows. Within one book, every page must land
// somewhere different — that is what catches a degenerate corpus.
TEST(CorpusFixture, EveryPageWithinAFixtureIsADistinctXpointer) {
  for (const int dom : {kOldestDomVersion, kLatestDomVersion}) {
    for (const auto& fixture : loadAll(dom)) {
      std::set<std::string> distinct;
      for (const auto& row : fixture.rows) distinct.insert(row.xpointer);
      EXPECT_EQ(distinct.size(), fixture.rows.size()) << fixture.epub << " dom " << dom;
    }
  }
}

TEST(CorpusFixture, CorpusIsOverwhelminglyDistinctOverall) {
  std::set<std::string> distinct;
  size_t rows = 0;
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      distinct.insert(row.xpointer);
      rows++;
    }
  }
  ASSERT_GT(rows, 150u);
  EXPECT_GT(distinct.size(), 150u);
}

TEST(CorpusFixture, NoSingleFixtureIsAllOneXpointer) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    if (fixture.rows.size() < 3) continue;
    std::set<std::string> distinct;
    for (const auto& row : fixture.rows) distinct.insert(row.xpointer);
    EXPECT_GT(distinct.size(), 1u) << fixture.epub;
  }
}

TEST(CorpusFixture, PagesAreOneBasedAndAscending) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    int previous = 0;
    for (const auto& row : fixture.rows) {
      EXPECT_GT(row.page, previous) << fixture.epub;
      previous = row.page;
    }
    EXPECT_EQ(fixture.rows.front().page, 1) << fixture.epub;
  }
}

// The two DOM versions differ in shape, which is the whole reason both are
// generated: the oldest omits [1] indices, the latest never does.
TEST(CorpusFixture, OldestDomVersionCarriesUnindexedSteps) {
  size_t unindexed = 0;
  for (const auto& fixture : loadAll(kOldestDomVersion)) {
    for (const auto& row : fixture.rows) {
      if (row.xpointer.rfind("/body/DocFragment[", 0) == 0) unindexed++;
    }
  }
  EXPECT_GT(unindexed, 100u);
}

TEST(CorpusFixture, LatestDomVersionIsFullyIndexed) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_EQ(row.xpointer.rfind("/body[1]/DocFragment[", 0), 0u) << row.xpointer;
    }
  }
}

TEST(CorpusFixture, EverySpineFileReferencedByTheCorpusExists) {
  for (const auto& name : corpus::epubNames()) {
    Fixture fixture;
    ASSERT_TRUE(loadFixture(corpus::fixturePath(name, kLatestDomVersion), fixture));
    for (const auto& row : fixture.rows) {
      const auto open = row.xpointer.find("DocFragment[");
      ASSERT_NE(open, std::string::npos) << row.xpointer;
      const int frag = std::atoi(row.xpointer.c_str() + open + 12);
      ASSERT_GT(frag, 0) << row.xpointer;
      EXPECT_FALSE(corpus::readFile(corpus::spinePath(name, frag)).empty()) << corpus::spinePath(name, frag);
    }
  }
}

TEST(CorpusFixture, SidecarRegressionEntriesArePresent) {
  ASSERT_EQ(corpus::kSidecarEntryCount, 2u);
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_STREQ(corpus::kSidecarEntries[i].raw, "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0");
  }
}

TEST(CorpusFixture, MissingFixtureIsReportedNotCrashed) {
  Fixture fixture;
  EXPECT_FALSE(loadFixture(corpus::fixtureDir() + "/does_not_exist_dom1.csv", fixture));
}
