#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitExchangeResponse.h"

using bookorbit::decodeExchangeResponse;
using bookorbit::ExchangeResponse;

TEST(BookOrbitExchangeResponse, DecodesAnEmptyResult) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"unmatched":[],"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false}]})", response));
  EXPECT_TRUE(response.unmatched.empty());
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].hash, "abc");
  EXPECT_TRUE(response.results[0].add.empty());
  EXPECT_TRUE(response.results[0].remove.empty());
  EXPECT_FALSE(response.results[0].more);
}

TEST(BookOrbitExchangeResponse, DecodesUnmatchedHashes) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":["abc","def"],"results":[]})", response));
  ASSERT_EQ(response.unmatched.size(), 2u);
  EXPECT_EQ(response.unmatched[0], "abc");
  EXPECT_EQ(response.unmatched[1], "def");
}

TEST(BookOrbitExchangeResponse, DecodesAnAnnotationToAdd) {
  const std::string body = R"({
    "unmatched": [],
    "results": [{
      "hash": "abc",
      "toApply": {
        "add": [{
          "serverId": 4711,
          "datetime": "2026-09-11 14:03:00",
          "datetimeUpdated": "2026-09-11 15:00:00",
          "drawer": "underscore",
          "color": "yellow",
          "text": "from the web",
          "note": "a note",
          "chapter": "Chapter Three",
          "pageno": 42,
          "posFormat": "xpointer",
          "pos0": "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0",
          "pos1": "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].31"
        }],
        "delete": []
      },
      "more": false
    }]
  })";

  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(body, response));
  ASSERT_EQ(response.results.size(), 1u);
  ASSERT_EQ(response.results[0].add.size(), 1u);
  const auto& entry = response.results[0].add[0];
  EXPECT_EQ(entry.serverId, "4711");
  EXPECT_EQ(entry.datetime, "2026-09-11 14:03:00");
  EXPECT_EQ(entry.datetimeUpdated, "2026-09-11 15:00:00");
  EXPECT_EQ(entry.drawer, "underscore");
  EXPECT_EQ(entry.color, "yellow");
  EXPECT_EQ(entry.text, "from the web");
  EXPECT_EQ(entry.note, "a note");
  EXPECT_EQ(entry.chapter, "Chapter Three");
  EXPECT_EQ(entry.pageno, 42);
  EXPECT_EQ(entry.posFormat, "xpointer");
  EXPECT_EQ(entry.pos0, "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0");
  EXPECT_EQ(entry.pos1, "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].31");
}

TEST(BookOrbitExchangeResponse, DecodesDeletesByServerIdKeyAndDatetime) {
  const std::string body = R"({
    "unmatched": [],
    "results": [{
      "hash": "abc",
      "toApply": {
        "add": [],
        "delete": [{"serverId": "9", "key": "08759494897afa79aec0d37d83498d30", "datetime": "2026-09-11 14:03:00"}]
      },
      "more": true
    }]
  })";

  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(body, response));
  ASSERT_EQ(response.results[0].remove.size(), 1u);
  EXPECT_EQ(response.results[0].remove[0].serverId, "9");
  EXPECT_EQ(response.results[0].remove[0].key, "08759494897afa79aec0d37d83498d30");
  EXPECT_EQ(response.results[0].remove[0].datetime, "2026-09-11 14:03:00");
  EXPECT_TRUE(response.results[0].more);
}

// The server may send serverId as a JSON number or a string; both must land in
// the same field, because the ack has to echo it back verbatim.
TEST(BookOrbitExchangeResponse, AcceptsNumericAndStringServerIds) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"results":[{"hash":"abc","toApply":{"add":[{"serverId":12,"pos0":"/body[1]"},)"
                                     R"({"serverId":"13","pos0":"/body[1]"}],"delete":[]},"more":false}]})",
                                     response));
  ASSERT_EQ(response.results[0].add.size(), 2u);
  EXPECT_EQ(response.results[0].add[0].serverId, "12");
  EXPECT_EQ(response.results[0].add[1].serverId, "13");
}

// A bookmark's position arrives as "pos"; one struct serves both routes.
TEST(BookOrbitExchangeResponse, BookmarkPosLandsInPos0) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[{"serverId":1,"pos":"/body[1]/DocFragment[5]",)"
      R"("title":"Chapter 5"}],"delete":[]},"more":false}]})",
      response));
  ASSERT_EQ(response.results[0].add.size(), 1u);
  EXPECT_EQ(response.results[0].add[0].pos0, "/body[1]/DocFragment[5]");
  EXPECT_EQ(response.results[0].add[0].title, "Chapter 5");
}

TEST(BookOrbitExchangeResponse, DecodesSeveralBooks) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false},)"
                                     R"({"hash":"def","toApply":{"add":[],"delete":[]},"more":true}]})",
                                     response));
  ASSERT_EQ(response.results.size(), 2u);
  EXPECT_EQ(response.results[0].hash, "abc");
  EXPECT_FALSE(response.results[0].more);
  EXPECT_EQ(response.results[1].hash, "def");
  EXPECT_TRUE(response.results[1].more);
}

TEST(BookOrbitExchangeResponse, MissingSectionsDecodeAsEmpty) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"results":[{"hash":"abc","more":false}]})", response));
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_TRUE(response.results[0].add.empty());
  EXPECT_TRUE(response.results[0].remove.empty());
}

TEST(BookOrbitExchangeResponse, RejectsMalformedJson) {
  ExchangeResponse response;
  EXPECT_FALSE(decodeExchangeResponse("{not json", response));
}

TEST(BookOrbitExchangeResponse, ClearsPreviousContentBeforeDecoding) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":["stale"],"results":[]})", response));
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":[],"results":[]})", response));
  EXPECT_TRUE(response.unmatched.empty());
}

// An entry with no serverId cannot be acked, so it must not enter the apply
// list — an unackable entry would be re-sent forever.
TEST(BookOrbitExchangeResponse, DropsEntriesWithoutAServerId) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[{"pos0":"/body[1]"}],"delete":[]},"more":false}]})", response));
  EXPECT_TRUE(response.results[0].add.empty());
}
