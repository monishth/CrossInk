#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogQuery.h"

using bookorbit::CatalogQuery;
using bookorbit::urlEncodeComponent;

TEST(UrlEncodeComponent, KeepsUnreservedCharacters) { EXPECT_EQ(urlEncodeComponent("abcXYZ091-_.~"), "abcXYZ091-_.~"); }

TEST(UrlEncodeComponent, EncodesSpacesAndPunctuation) {
  EXPECT_EQ(urlEncodeComponent("Richard Osman"), "Richard%20Osman");
  EXPECT_EQ(urlEncodeComponent("a&b=c"), "a%26b%3Dc");
  EXPECT_EQ(urlEncodeComponent("50%"), "50%25");
}

TEST(UrlEncodeComponent, EncodesHighBytesUppercaseHex) {
  // UTF-8 "é" is 0xC3 0xA9. Hex digits must be uppercase, as util.urlEncode emits.
  EXPECT_EQ(urlEncodeComponent("\xc3\xa9"), "%C3%A9");
}

TEST(CatalogQuery, PathIsReturnedUnchangedWhenNoParameters) {
  const CatalogQuery query;
  EXPECT_TRUE(query.empty());
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books");
}

// The ordering rule: keys sorted alphabetically, not in insertion order.
TEST(CatalogQuery, SortsKeysAlphabetically) {
  CatalogQuery query;
  query.set("sort", "recently_added");
  query.set("page", 2);
  query.set("author", "Osman");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"),
            "/koreader/plugin/catalog/books?author=Osman&page=2&sort=recently_added");
}

TEST(CatalogQuery, DropsEmptyValues) {
  CatalogQuery query;
  query.set("q", "");
  query.set("page", 1);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=1");
}

TEST(CatalogQuery, ClearRemovesAPreviouslySetKey) {
  CatalogQuery query;
  query.set("page", 3);
  query.set("q", "murders");
  query.clear("q");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=3");
}

TEST(CatalogQuery, SettingAKeyTwiceReplacesIt) {
  CatalogQuery query;
  query.set("page", 1);
  query.set("page", 4);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=4");
}

TEST(CatalogQuery, EncodesBothKeysAndValues) {
  CatalogQuery query;
  query.set("q", "We Solve Murders");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?q=We%20Solve%20Murders");
}

TEST(CatalogQuery, CarriesTheFullCatalogFilterSet) {
  CatalogQuery query;
  query.set("libraryId", 3);
  query.set("collectionId", 7);
  query.set("author", "Osman");
  query.set("seriesId", 11);
  query.set("sort", "series");
  query.set("order", "asc");
  query.set("readStatus", "reading");
  query.set("format", "epub");
  query.set("page", 1);
  query.set("size", 20);
  query.set("q", "murder");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"),
            "/koreader/plugin/catalog/books?author=Osman&collectionId=7&format=epub&libraryId=3&order=asc"
            "&page=1&q=murder&readStatus=reading&seriesId=11&size=20&sort=series");
}

TEST(CatalogQuery, NegativeAndZeroNumbersAreKept) {
  CatalogQuery query;
  query.set("page", 0);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=0");
}
