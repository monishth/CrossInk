#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogDecode.h"

using bookorbit::CatalogPage;
using bookorbit::decodeBookPage;

namespace {

const char* kTwoBookPage = R"({
  "page": 2,
  "size": 20,
  "hasNext": true,
  "query": "murder",
  "items": [
    {"id": 41, "title": "We Solve Murders", "authors": "Richard Osman",
     "series": "We Solve Murders", "seriesIndex": 1, "readStatus": "reading",
     "rating": 4, "progressPercentage": 37.5, "formats": ["epub", "mobi"],
     "fileId": 902, "fileBytes": 1258291, "filename": "we-solve-murders.epub"},
    {"id": 42, "title": "The Last Devil to Die", "authors": "Richard Osman",
     "readStatus": "finished", "rating": 5, "progressPercentage": 100,
     "formats": ["epub"], "fileId": 903, "fileBytes": 981234,
     "filename": "the-last-devil-to-die.epub"}
  ]
})";

}  // namespace

TEST(CatalogDecode, DecodesPageEnvelope) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  EXPECT_EQ(page.page, 2u);
  EXPECT_EQ(page.size, 20u);
  EXPECT_TRUE(page.hasNext);
  EXPECT_EQ(page.query, "murder");
}

TEST(CatalogDecode, DecodesEveryItem) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].bookId, 41u);
  EXPECT_EQ(page.items[0].title, "We Solve Murders");
  EXPECT_EQ(page.items[0].authors, "Richard Osman");
  EXPECT_EQ(page.items[0].series, "We Solve Murders");
  EXPECT_EQ(page.items[0].seriesIndex, 1u);
  EXPECT_EQ(page.items[0].readStatus, "reading");
  EXPECT_EQ(page.items[0].rating, 4);
  EXPECT_FLOAT_EQ(page.items[0].progressPercentage, 37.5f);
  EXPECT_EQ(page.items[0].fileId, 902u);
  EXPECT_EQ(page.items[0].fileBytes, 1258291u);
  EXPECT_EQ(page.items[0].filename, "we-solve-murders.epub");
  EXPECT_EQ(page.items[1].bookId, 42u);
  EXPECT_EQ(page.items[1].readStatus, "finished");
}

// The nested "formats" array must not terminate the "items" array. This is the
// bug the separate array-depth tracking exists to prevent.
TEST(CatalogDecode, NestedFormatsArrayDoesNotCloseItems) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].formats, "epub, mobi");
  EXPECT_EQ(page.items[1].formats, "epub");
}

TEST(CatalogDecode, MissingFieldsKeepDefaults) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"items":[{"id":7,"title":"Untitled"}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 7u);
  EXPECT_EQ(page.items[0].authors, "");
  EXPECT_EQ(page.items[0].rating, 0);
  EXPECT_EQ(page.items[0].fileId, 0u);
  EXPECT_FALSE(page.hasNext);
  EXPECT_EQ(page.page, 1u);
}

TEST(CatalogDecode, EmptyItemsArrayIsAValidPage) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"page":1,"hasNext":false,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
  EXPECT_FALSE(page.hasNext);
}

TEST(CatalogDecode, NullFieldsAreTolerated) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"items":[{"id":7,"series":null,"rating":null}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].series, "");
  EXPECT_EQ(page.items[0].rating, 0);
}

// A server ignoring "size" must not be able to grow the device's working set
// without bound. Extra records past the cap are dropped, not decoded.
TEST(CatalogDecode, ItemsAreCappedAtMaxPageItems) {
  std::string json = R"({"items":[)";
  for (size_t i = 0; i < bookorbit::kMaxPageItems + 25; ++i) {
    if (i > 0) json += ',';
    json += R"({"id":)" + std::to_string(i + 1) + R"(,"title":"t"})";
  }
  json += "]}";

  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(json, page));
  EXPECT_EQ(page.items.size(), bookorbit::kMaxPageItems);
  EXPECT_EQ(page.items.front().bookId, 1u);
}

TEST(CatalogDecode, MalformedJsonFails) {
  CatalogPage page;
  EXPECT_FALSE(decodeBookPage(R"({"items":[{"id":7,)", page));
}

TEST(CatalogDecode, EmptyBodyFails) {
  CatalogPage page;
  EXPECT_FALSE(decodeBookPage("", page));
}

// Decoding is fed in fragments the way the transport delivers them; the result
// must not depend on chunk boundaries.
TEST(CatalogDecode, ResultIsIndependentOfChunking) {
  CatalogPage whole;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, whole));

  CatalogPage chunked;
  ASSERT_TRUE(bookorbit::decodeBookPageChunked(kTwoBookPage, 7, chunked));
  ASSERT_EQ(chunked.items.size(), whole.items.size());
  EXPECT_EQ(chunked.items[0].title, whole.items[0].title);
  EXPECT_EQ(chunked.items[1].filename, whole.items[1].filename);
  EXPECT_EQ(chunked.hasNext, whole.hasNext);
}

TEST(CatalogDecode, DecodeResetsAnyPreviousContent) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_TRUE(decodeBookPage(R"({"page":1,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
  EXPECT_FALSE(page.hasNext);
  EXPECT_EQ(page.query, "");
}
