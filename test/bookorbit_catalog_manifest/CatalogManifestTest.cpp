#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/CatalogManifest.h"
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
  bookorbit::ManifestEnumerator enumerator{api, "crossink-abc123"};
};

const char* kPageOne = R"({
  "manifestVersion": "lv-77",
  "hasNext": true,
  "nextCursor": "eyJpZCI6NDF9",
  "items": [
    {"bookId": 41, "fileId": 902, "hash": "0f0a792b00a37cf80baa5e50c078b31f",
     "title": "We Solve Murders", "filename": "we-solve-murders.epub",
     "format": "epub", "fileBytes": 1258291}
  ]
})";

const char* kPageTwo = R"({
  "manifestVersion": "lv-77",
  "hasNext": false,
  "items": [
    {"bookId": 42, "fileId": 903, "hash": "a1b2c3d4e5f60718293a4b5c6d7e8f90",
     "title": "Piranesi", "filename": "piranesi.epub", "format": "epub", "fileBytes": 654321}
  ]
})";

}  // namespace

using bookorbit::decodeManifestPage;
using bookorbit::ManifestPage;
using bookorbit::Status;

TEST(ManifestDecode, DecodesItemsAndCursor) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(kPageOne, page));
  EXPECT_TRUE(page.hasNext);
  EXPECT_EQ(page.nextCursor, "eyJpZCI6NDF9");
  EXPECT_EQ(page.manifestVersion, "lv-77");
  EXPECT_FALSE(page.restartRequired);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);
  EXPECT_EQ(page.items[0].fileId, 902u);
  EXPECT_EQ(page.items[0].hash, "0f0a792b00a37cf80baa5e50c078b31f");
  EXPECT_EQ(page.items[0].filename, "we-solve-murders.epub");
  EXPECT_EQ(page.items[0].fileBytes, 1258291u);
}

TEST(ManifestDecode, DecodesRestartRequiredFlag) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(R"({"restartRequired":true,"manifestVersion":"lv-78"})", page));
  EXPECT_TRUE(page.restartRequired);
  EXPECT_EQ(page.manifestVersion, "lv-78");
}

// hasNext without a cursor is not a next page: the walk must stop.
TEST(ManifestDecode, HasNextWithoutCursorIsNotANextPage) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(R"({"hasNext":true,"items":[]})", page));
  EXPECT_TRUE(page.hasNext);
  EXPECT_TRUE(page.nextCursor.empty());
}

TEST(ManifestDecode, MalformedJsonFails) {
  ManifestPage page;
  EXPECT_FALSE(decodeManifestPage(R"({"items":[{"bookId":)", page));
}

TEST(ManifestEnumerator, FirstRequestSendsDeviceIdAndNoCursor) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?deviceId=crossink-abc123&size=20");
}

TEST(ManifestEnumerator, SecondRequestCarriesTheCursor) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, kPageTwo});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_TRUE(f.enumerator.hasNext());
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?cursor=eyJpZCI6NDF9&deviceId=crossink-abc123&size=20");
  EXPECT_EQ(page.items[0].bookId, 42u);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, TracksTheManifestVersion) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.manifestVersion(), "lv-77");
}

// The cursor-restart path. A rejected cursor drops the cursor and re-enumerates
// from scratch within the same next() call, so the caller sees page one again
// rather than an error.
TEST(ManifestEnumerator, RejectedCursorRestartsEnumerationFromScratch) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-78"})"});
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_TRUE(f.enumerator.hasNext());

  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.restartCount(), 1u);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);

  ASSERT_EQ(f.transport.sent.size(), 3u);
  // The retry after the rejection carries no cursor at all.
  EXPECT_EQ(f.transport.sent[2].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?deviceId=crossink-abc123&size=20");
}

TEST(ManifestEnumerator, RestartAdoptsTheNewManifestVersion) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-78"})"});
  f.transport.queued.push_back({200, false, R"({"manifestVersion":"lv-78","hasNext":false,"items":[]})"});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.manifestVersion(), "lv-78");
}

// A server that rejects every cursor must not spin forever on a battery device.
TEST(ManifestEnumerator, RestartsAreBounded) {
  Fixture f;
  for (int i = 0; i < 8; ++i) {
    f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-79"})"});
  }

  ManifestPage page;
  const auto error = f.enumerator.next(page);
  EXPECT_EQ(error.status, Status::ClientError);
  EXPECT_EQ(f.enumerator.restartCount(), bookorbit::kMaxManifestRestarts);
  EXPECT_LE(f.transport.sent.size(), bookorbit::kMaxManifestRestarts + 1u);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, NotFoundDowngradesTheManifestCapability) {
  Fixture f;
  f.transport.queued.push_back({404, false, ""});

  ManifestPage page;
  EXPECT_EQ(f.enumerator.next(page).status, Status::NotFound);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, ResetClearsCursorAndRestartCount) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  f.enumerator.reset();
  EXPECT_EQ(f.enumerator.restartCount(), 0u);
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url, f.transport.sent[0].url);
}

TEST(ManifestEnumerator, TransportFailureStopsTheWalkWithoutRestarting) {
  Fixture f;
  f.transport.queued.push_back({0, true, ""});

  ManifestPage page;
  EXPECT_EQ(f.enumerator.next(page).status, Status::Transport);
  EXPECT_EQ(f.enumerator.restartCount(), 0u);
}
