#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitExchangeRequest.h"

using bookorbit::Annotation;
using bookorbit::AnnotationKey;
using bookorbit::Bookmark;
using bookorbit::BookmarkKey;
using bookorbit::encodeAnnotationExchange;
using bookorbit::encodeBookmarkExchange;

namespace {

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr char kPos[] = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17";

Annotation highlight() {
  Annotation entry;
  entry.datetime = "2026-09-11 14:03:00";
  entry.datetimeUpdated = "2026-09-11 15:00:00";
  entry.drawer = "lighten";
  entry.color = "yellow";
  entry.text = "a highlight";
  entry.note = "a note";
  entry.chapter = "Chapter Three";
  entry.pageno = 42;
  entry.posFormat = "xpointer";
  entry.pos0 = kPos;
  entry.pos1 = kPos;
  return entry;
}

Bookmark dogear() {
  Bookmark entry;
  entry.datetime = "2026-09-01 08:00:00";
  entry.pos = kPos;
  entry.chapter = "Chapter Three";
  entry.note = "start here";
  entry.pageno = 7;
  return entry;
}

}  // namespace

TEST(BookOrbitExchangeRequest, WrapsOneBookInABooksArray) {
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {});
  EXPECT_EQ(json.rfind(R"({"books":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f")", 0), 0u);
  EXPECT_EQ(json.substr(json.size() - 3), "}]}");
}

TEST(BookOrbitExchangeRequest, EncodesKeysAsKAndDt) {
  const std::vector<AnnotationKey> keys = {{"08759494897afa79aec0d37d83498d30", "2026-09-11 14:03:00"}};
  const std::string json = encodeAnnotationExchange(kHash, keys, true, {});
  EXPECT_NE(json.find(R"("keys":[{"k":"08759494897afa79aec0d37d83498d30","dt":"2026-09-11 14:03:00"}])"),
            std::string::npos);
  EXPECT_NE(json.find(R"("keysComplete":true)"), std::string::npos);
}

// keysComplete false means "this is not my entire key set". Sending a partial
// set with it would invite the server to delete what it cannot see.
TEST(BookOrbitExchangeRequest, IncompleteKeySetOmitsKeysEntirely) {
  const std::vector<AnnotationKey> keys = {{"abc", "2026-09-11 14:03:00"}};
  const std::string json = encodeAnnotationExchange(kHash, keys, false, {});
  EXPECT_NE(json.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(json.find(R"("k":"abc")"), std::string::npos);
  EXPECT_NE(json.find(R"("keys":[])"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesEveryAnnotationField) {
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {highlight()});
  EXPECT_NE(json.find(R"("datetime":"2026-09-11 14:03:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("datetimeUpdated":"2026-09-11 15:00:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("drawer":"lighten")"), std::string::npos);
  EXPECT_NE(json.find(R"("color":"yellow")"), std::string::npos);
  EXPECT_NE(json.find(R"("text":"a highlight")"), std::string::npos);
  EXPECT_NE(json.find(R"("note":"a note")"), std::string::npos);
  EXPECT_NE(json.find(R"("chapter":"Chapter Three")"), std::string::npos);
  EXPECT_NE(json.find(R"("pageno":42)"), std::string::npos);
  EXPECT_NE(json.find(R"("posFormat":"xpointer")"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos0":")") + kPos + '"'), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos1":")") + kPos + '"'), std::string::npos);
}

TEST(BookOrbitExchangeRequest, OmitsEmptyOptionalFields) {
  Annotation bare = highlight();
  bare.datetimeUpdated.clear();
  bare.color.clear();
  bare.note.clear();
  bare.chapter.clear();
  bare.pageno = -1;

  const std::string json = encodeAnnotationExchange(kHash, {}, true, {bare});
  EXPECT_EQ(json.find(R"("datetimeUpdated")"), std::string::npos);
  EXPECT_EQ(json.find(R"("color")"), std::string::npos);
  EXPECT_EQ(json.find(R"("note")"), std::string::npos);
  EXPECT_EQ(json.find(R"("chapter")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pageno")"), std::string::npos);
  // The required identity fields survive.
  EXPECT_NE(json.find(R"("datetime":"2026-09-11 14:03:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("drawer":"lighten")"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EscapesQuotesAndNewlinesInText) {
  Annotation quoted = highlight();
  quoted.text = "he said \"no\"\nand left";
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {quoted});
  EXPECT_NE(json.find(R"(he said \"no\"\nand left)"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesSeveralChangesInOrder) {
  Annotation second = highlight();
  second.datetime = "2026-09-12 10:00:00";
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {highlight(), second});
  const size_t first = json.find(R"("datetime":"2026-09-11 14:03:00")");
  const size_t next = json.find(R"("datetime":"2026-09-12 10:00:00")");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(next, std::string::npos);
  EXPECT_LT(first, next);
}

TEST(BookOrbitExchangeRequest, EmptyChangesEncodeAsAnEmptyArray) {
  EXPECT_NE(encodeAnnotationExchange(kHash, {}, true, {}).find(R"("changes":[])"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesBookmarkFields) {
  const std::vector<BookmarkKey> keys = {{"deadbeef", "2026-09-01 08:00:00"}};
  const std::string json = encodeBookmarkExchange(kHash, keys, true, {dogear()});
  EXPECT_NE(json.find(R"("datetime":"2026-09-01 08:00:00")"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos":")") + kPos + '"'), std::string::npos);
  EXPECT_NE(json.find(R"("pageno":7)"), std::string::npos);
  EXPECT_NE(json.find(R"("chapter":"Chapter Three")"), std::string::npos);
  EXPECT_NE(json.find(R"("note":"start here")"), std::string::npos);
  EXPECT_NE(json.find(R"("k":"deadbeef")"), std::string::npos);
}

// A bookmark has no drawer, colour, text or pos1. Sending them would make the
// server treat a dogear as a highlight.
TEST(BookOrbitExchangeRequest, BookmarkEncodingCarriesNoHighlightFields) {
  const std::string json = encodeBookmarkExchange(kHash, {}, true, {dogear()});
  EXPECT_EQ(json.find(R"("drawer")"), std::string::npos);
  EXPECT_EQ(json.find(R"("color")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pos0")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pos1")"), std::string::npos);
  EXPECT_EQ(json.find(R"("posFormat")"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, CapsAreTheSpecValues) {
  EXPECT_EQ(bookorbit::kUploadChunk, 50u);
  EXPECT_EQ(bookorbit::kMaxPullRounds, 10u);
  EXPECT_EQ(bookorbit::kMaxAnnotationKeysPerBook, 5000u);
  EXPECT_EQ(bookorbit::kMaxBookmarkKeysPerBook, 500u);
}

TEST(BookOrbitExchangeRequest, PathsAreTheSpecRoutes) {
  EXPECT_STREQ(bookorbit::kAnnotationExchangePath, "/koreader/plugin/annotations/exchange");
  EXPECT_STREQ(bookorbit::kAnnotationAckPath, "/koreader/plugin/annotations/exchange-ack");
  EXPECT_STREQ(bookorbit::kBookmarkExchangePath, "/koreader/plugin/bookmarks/exchange");
  EXPECT_STREQ(bookorbit::kBookmarkAckPath, "/koreader/plugin/bookmarks/exchange-ack");
}
