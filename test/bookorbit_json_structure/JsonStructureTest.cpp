#include <gtest/gtest.h>

#include "lib/BookOrbit/JsonStructure.h"

using bookorbit::jsonIsStructurallyComplete;

TEST(JsonStructure, AcceptsAWellFormedObject) {
  EXPECT_TRUE(jsonIsStructurallyComplete(R"({"a":1,"b":[1,2,{"c":null}]})"));
}

TEST(JsonStructure, AcceptsAWellFormedArrayRoot) {
  EXPECT_TRUE(jsonIsStructurallyComplete(R"([{"a":true},{"a":false}])"));
}

// The case that slipped past StreamingJsonParser::hasError() and made
// decodeMatchCheck report success on a truncated body.
TEST(JsonStructure, RejectsUnterminatedObject) {
  EXPECT_FALSE(jsonIsStructurallyComplete("{not json"));
  EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":1)"));
}

TEST(JsonStructure, RejectsUnterminatedArray) { EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":[1,2)")); }

TEST(JsonStructure, RejectsUnterminatedString) { EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":"unclosed})")); }

TEST(JsonStructure, RejectsMismatchedClosers) {
  EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":[1,2})"));
  EXPECT_FALSE(jsonIsStructurallyComplete(R"([1,2})"));
}

TEST(JsonStructure, RejectsTrailingCloser) { EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":1}})")); }

TEST(JsonStructure, RejectsEmptyOrWhitespaceOnly) {
  EXPECT_FALSE(jsonIsStructurallyComplete(""));
  EXPECT_FALSE(jsonIsStructurallyComplete("   \n\t "));
}

TEST(JsonStructure, RequiresAContainerRoot) {
  // BookOrbit responses are always an object or array; a bare scalar is not a
  // valid response body even though it is valid JSON.
  EXPECT_FALSE(jsonIsStructurallyComplete("42"));
  EXPECT_FALSE(jsonIsStructurallyComplete(R"("hello")"));
}

TEST(JsonStructure, EscapedQuotesDoNotTerminateAString) {
  EXPECT_TRUE(jsonIsStructurallyComplete(R"({"a":"he said \"hi\""})"));
  EXPECT_FALSE(jsonIsStructurallyComplete(R"({"a":"trailing backslash \)"));
}

TEST(JsonStructure, BracesInsideStringsAreNotStructural) { EXPECT_TRUE(jsonIsStructurallyComplete(R"({"a":"{[}]"})")); }

TEST(JsonStructure, RespectsTheNestingLimitOfTheStreamingParser) {
  // StreamingJsonParser supports 32 levels; anything deeper would overflow its
  // stack, so reject rather than hand it to the parser.
  std::string deep;
  for (int i = 0; i < 40; i++) deep += '[';
  for (int i = 0; i < 40; i++) deep += ']';
  EXPECT_FALSE(jsonIsStructurallyComplete(deep));

  std::string ok;
  for (int i = 0; i < 30; i++) ok += '[';
  for (int i = 0; i < 30; i++) ok += ']';
  EXPECT_TRUE(jsonIsStructurallyComplete(ok));
}
