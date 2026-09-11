#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogDecode.h"

using bookorbit::CatalogEntryPage;
using bookorbit::DashboardSummary;
using bookorbit::decodeDashboard;
using bookorbit::decodeEntryPage;

TEST(CatalogEntryDecode, DecodesRootNavigation) {
  const char* json = R"({
    "items": [
      {"id": "libraries", "title": "Libraries", "kind": "libraries", "count": 3},
      {"id": "collections", "title": "Collections", "kind": "collections", "count": 12},
      {"id": "series", "title": "Series", "kind": "series", "count": 41}
    ]
  })";

  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(json, page));
  ASSERT_EQ(page.items.size(), 3u);
  EXPECT_EQ(page.items[0].id, "libraries");
  EXPECT_EQ(page.items[0].title, "Libraries");
  EXPECT_EQ(page.items[0].kind, "libraries");
  EXPECT_EQ(page.items[0].count, 3u);
  EXPECT_EQ(page.items[2].count, 41u);
  EXPECT_FALSE(page.hasNext);
}

TEST(CatalogEntryDecode, DecodesNumericIdsAsStrings) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"items":[{"id":17,"title":"Crime","seriesId":42}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].id, "17");
  EXPECT_EQ(page.items[0].seriesId, "42");
}

TEST(CatalogEntryDecode, CarriesPaginationEnvelope) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"page":3,"hasNext":true,"items":[{"id":"a","title":"A"}]})", page));
  EXPECT_EQ(page.page, 3u);
  EXPECT_TRUE(page.hasNext);
}

TEST(CatalogEntryDecode, EmptySectionIsValid) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"page":1,"hasNext":false,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
}

TEST(CatalogEntryDecode, ItemsAreCappedAtMaxPageItems) {
  std::string json = R"({"items":[)";
  for (size_t i = 0; i < bookorbit::kMaxPageItems + 10; ++i) {
    if (i > 0) json += ',';
    json += R"({"id":")" + std::to_string(i) + R"(","title":"t"})";
  }
  json += "]}";

  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(json, page));
  EXPECT_EQ(page.items.size(), bookorbit::kMaxPageItems);
}

TEST(CatalogEntryDecode, MalformedJsonFails) {
  CatalogEntryPage page;
  EXPECT_FALSE(decodeEntryPage(R"({"items":[{"id":)", page));
}

TEST(CatalogDashboardDecode, DecodesStatsAndContinueReading) {
  const char* json = R"({
    "totalBooks": 412,
    "finishedBooks": 96,
    "continueReading": [
      {"id": 41, "title": "We Solve Murders", "authors": "Richard Osman",
       "progressPercentage": 37.5, "fileId": 902},
      {"id": 55, "title": "Piranesi", "authors": "Susanna Clarke",
       "progressPercentage": 12.0, "fileId": 913}
    ],
    "sections": [
      {"id": "want-to-read", "title": "Want to read", "count": 8},
      {"id": "up-next-in-series", "title": "Up next in series", "count": 5}
    ]
  })";

  DashboardSummary dashboard;
  ASSERT_TRUE(decodeDashboard(json, dashboard));
  EXPECT_EQ(dashboard.totalBooks, 412u);
  EXPECT_EQ(dashboard.finishedBooks, 96u);
  ASSERT_EQ(dashboard.continueReading.size(), 2u);
  EXPECT_EQ(dashboard.continueReading[0].bookId, 41u);
  EXPECT_EQ(dashboard.continueReading[0].title, "We Solve Murders");
  EXPECT_FLOAT_EQ(dashboard.continueReading[1].progressPercentage, 12.0f);
  ASSERT_EQ(dashboard.sections.size(), 2u);
  EXPECT_EQ(dashboard.sections[1].id, "up-next-in-series");
  EXPECT_EQ(dashboard.sections[1].count, 5u);
}

TEST(CatalogDashboardDecode, AbsentSectionsIsValid) {
  DashboardSummary dashboard;
  ASSERT_TRUE(decodeDashboard(R"({"totalBooks":0,"continueReading":[]})", dashboard));
  EXPECT_TRUE(dashboard.continueReading.empty());
  EXPECT_TRUE(dashboard.sections.empty());
  EXPECT_EQ(dashboard.totalBooks, 0u);
}

TEST(CatalogDashboardDecode, MalformedJsonFails) {
  DashboardSummary dashboard;
  EXPECT_FALSE(decodeDashboard(R"({"continueReading":)", dashboard));
}
