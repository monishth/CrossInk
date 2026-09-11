#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpResponse> queued;
  std::vector<bookorbit::HttpRequest> sent;
  size_t index = 0;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (index >= queued.size()) return {500, false, ""};
    return queued[index++];
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

struct Fixture {
  ScriptedTransport transport;
  bookorbit::CapabilityCache capabilities;
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "u", "k", identity()};
  bookorbit::CatalogApi api{client, capabilities};
};

}  // namespace

using bookorbit::Capability;
using bookorbit::CatalogEntryPage;
using bookorbit::CatalogPage;
using bookorbit::CatalogQuery;
using bookorbit::DashboardSummary;
using bookorbit::kCapDashboardSections;
using bookorbit::Status;

TEST(CatalogApi, RootHitsTheRootRoute) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[{"id":"libraries","title":"Libraries"}]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.root(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].method, "GET");
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/root");
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].title, "Libraries");
}

TEST(CatalogApi, DashboardAndDiscoverUseTheirOwnRoutes) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"totalBooks":9,"continueReading":[]})"});
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  DashboardSummary dashboard;
  ASSERT_EQ(f.api.dashboard(dashboard).status, Status::Ok);
  EXPECT_EQ(dashboard.totalBooks, 9u);
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard");

  CatalogEntryPage discover;
  ASSERT_EQ(f.api.discover(discover).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url, "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard/discover");
}

TEST(CatalogApi, SectionPathSegmentIsUrlEncoded) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  CatalogQuery query;
  query.set("page", 2);
  query.set("size", 20);
  CatalogEntryPage page;
  ASSERT_EQ(f.api.section("up next", query, page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/sections/up%20next?page=2&size=20");
}

TEST(CatalogApi, BooksCarriesTheSortedFilterQuery) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"page":1,"items":[{"id":41,"title":"T"}]})"});

  CatalogQuery query;
  query.set("sort", "recently_added");
  query.set("libraryId", 3);
  query.set("page", 1);
  query.set("size", 20);
  CatalogPage page;
  ASSERT_EQ(f.api.books(query, page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/books"
            "?libraryId=3&page=1&size=20&sort=recently_added");
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);
}

TEST(CatalogApi, DashboardSectionHitsTheEncodedTypeRoute) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({200, false, R"({"items":[{"id":"a","title":"A"}]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.dashboardSection("up-next-in-series", page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard/sections/up-next-in-series");
}

// The gate: a server that never advertised the capability is not asked at all.
TEST(CatalogApi, DashboardSectionIsSkippedWhenCapabilityIsUnsupported) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"bookmarkSync"});
  ASSERT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unsupported);

  CatalogEntryPage page;
  const auto error = f.api.dashboardSection("want-to-read", page);
  EXPECT_EQ(error.status, Status::NotFound);
  EXPECT_TRUE(f.transport.sent.empty());
}

// Unknown means "we do not know yet", so the request is attempted.
TEST(CatalogApi, DashboardSectionIsAttemptedWhenCapabilityIsUnknown) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent.size(), 1u);
}

// A confirmed 404 on the feature's own route downgrades it immediately.
TEST(CatalogApi, NotFoundDowngradesTheCapability) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({404, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::NotFound);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unsupported);
}

// The rule that matters most: a 5xx must leave the capability Unknown, never
// cache a negative. One blip must not permanently disable the feature.
TEST(CatalogApi, ServerErrorLeavesTheCapabilityUnknown) {
  Fixture f;
  f.transport.queued.push_back({503, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::ServerError);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unknown);
}

TEST(CatalogApi, TransportFailureLeavesTheCapabilityUnknown) {
  Fixture f;
  f.transport.queued.push_back({0, true, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::Transport);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unknown);
}

// A 5xx must not downgrade a capability the server previously advertised.
TEST(CatalogApi, ServerErrorDoesNotEraseAPreviouslyAdvertisedCapability) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({500, false, ""});

  CatalogEntryPage page;
  f.api.dashboardSection("want-to-read", page);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Supported);
}

TEST(CatalogApi, UndecodableBodyIsReportedAsInvalidJson) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[{"id":)"});

  CatalogPage page;
  EXPECT_EQ(f.api.books(CatalogQuery{}, page).status, Status::InvalidJson);
}

TEST(CatalogApi, AuthErrorIsPassedThroughUnchanged) {
  Fixture f;
  f.transport.queued.push_back({401, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.root(page).status, Status::Unauthorized);
}
