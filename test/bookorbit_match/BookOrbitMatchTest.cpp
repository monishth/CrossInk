#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitMatch.h"

using bookorbit::decodeMatchCheck;
using bookorbit::encodeMatchCheck;
using bookorbit::MatchCandidate;
using bookorbit::MatchResult;

TEST(BookOrbitMatch, EncodesHashesArray) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "0f0a792b00a37cf80baa5e50c078b31f";
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("hashes":["0f0a792b00a37cf80baa5e50c078b31f"])"), std::string::npos);
}

TEST(BookOrbitMatch, EncodesCandidateMetadataAsHints) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = "We Solve Murders";
  one.authors = "Richard Osman";
  one.lastOpen = 1787561453u;
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("title":"We Solve Murders")"), std::string::npos);
  EXPECT_NE(json.find(R"("authors":"Richard Osman")"), std::string::npos);
  EXPECT_NE(json.find(R"("lastOpen":1787561453)"), std::string::npos);
}

// Ambiguous metadata must be flagged, and title/authors withheld, so the
// server never associates the wrong book. Mirrors bookorbit_book_sync.lua.
TEST(BookOrbitMatch, AmbiguousMetadataIsFlaggedAndWithheld) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = "Ambiguous";
  one.authors = "Someone";
  one.metadataAmbiguous = true;
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("metadataAmbiguous":true)"), std::string::npos);
  EXPECT_EQ(json.find(R"("title":"Ambiguous")"), std::string::npos);
}

TEST(BookOrbitMatch, EscapesQuotesInTitles) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = R"(The "Quoted" Book)";
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"(The \"Quoted\" Book)"), std::string::npos);
}

TEST(BookOrbitMatch, DecodesMatchesAndLibraryVersion) {
  const std::string body = R"({
    "matches": [
      {"hash": "abc", "bookFileId": 11, "bookId": 7},
      {"hash": "def", "bookFileId": 12, "bookId": 8}
    ],
    "libraryVersion": "5e52cee3c2f603bf"
  })";

  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(body, results, libraryVersion));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].hash, "abc");
  EXPECT_EQ(results[0].bookFileId, 11u);
  EXPECT_EQ(results[0].bookId, 7u);
  EXPECT_EQ(results[1].hash, "def");
  EXPECT_EQ(libraryVersion, "5e52cee3c2f603bf");
}

TEST(BookOrbitMatch, DecodesEmptyMatchesAsNoResults) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(R"({"matches":[],"libraryVersion":"v1"})", results, libraryVersion));
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(libraryVersion, "v1");
}

TEST(BookOrbitMatch, RejectsMalformedJson) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  EXPECT_FALSE(decodeMatchCheck("{not json", results, libraryVersion));
}

TEST(BookOrbitMatch, MissingLibraryVersionLeavesItEmpty) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(R"({"matches":[{"hash":"abc","bookFileId":1,"bookId":2}]})", results, libraryVersion));
  EXPECT_EQ(results.size(), 1u);
  EXPECT_TRUE(libraryVersion.empty());
}
