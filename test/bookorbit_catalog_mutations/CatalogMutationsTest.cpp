#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/CatalogMutations.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpResponse> queued;
  std::vector<bookorbit::HttpRequest> sent;
  size_t index = 0;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (index >= queued.size()) return {200, false, "{}"};
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

using bookorbit::encodeRating;
using bookorbit::encodeReadStatus;
using bookorbit::ratingPath;
using bookorbit::ReadStatus;
using bookorbit::readStatusFromWire;
using bookorbit::readStatusPath;
using bookorbit::readStatusToWire;
using bookorbit::Status;

TEST(ReadStatusWire, MapsEveryValue) {
  EXPECT_STREQ(readStatusToWire(ReadStatus::Unread), "unread");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Reading), "reading");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Finished), "finished");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Abandoned), "abandoned");
}

TEST(ReadStatusWire, ParsesBackFromTheWire) {
  ReadStatus status = ReadStatus::Unread;
  ASSERT_TRUE(readStatusFromWire("reading", status));
  EXPECT_EQ(status, ReadStatus::Reading);
  ASSERT_TRUE(readStatusFromWire("abandoned", status));
  EXPECT_EQ(status, ReadStatus::Abandoned);
}

TEST(ReadStatusWire, RejectsUnknownValues) {
  ReadStatus status = ReadStatus::Reading;
  EXPECT_FALSE(readStatusFromWire("complete", status));
  EXPECT_FALSE(readStatusFromWire("", status));
  EXPECT_EQ(status, ReadStatus::Reading);
}

TEST(EncodeMutations, ReadStatusBody) { EXPECT_EQ(encodeReadStatus(ReadStatus::Finished), R"({"status":"finished"})"); }

TEST(EncodeMutations, RatingBody) {
  EXPECT_EQ(encodeRating(4), R"({"rating":4})");
  EXPECT_EQ(encodeRating(1), R"({"rating":1})");
  EXPECT_EQ(encodeRating(5), R"({"rating":5})");
}

// Clearing is explicit: null, not an absent field. An absent field means
// "unchanged" on the server.
TEST(EncodeMutations, ClearingARatingSendsNull) {
  EXPECT_EQ(encodeRating(0), R"({"rating":null})");
  EXPECT_EQ(encodeRating(-1), R"({"rating":null})");
}

TEST(EncodeMutations, OutOfRangeRatingIsClampedToTheTopOfTheScale) { EXPECT_EQ(encodeRating(9), R"({"rating":5})"); }

TEST(MutationPaths, AreBuiltFromTheBookId) {
  EXPECT_EQ(readStatusPath(41), "/koreader/plugin/catalog/books/41/read-status");
  EXPECT_EQ(ratingPath(41), "/koreader/plugin/catalog/books/41/rating");
}

TEST(CatalogApiMutations, SetReadStatusPutsTheBody) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setReadStatus(41, ReadStatus::Reading).status, Status::Ok);
  ASSERT_EQ(f.transport.sent.size(), 1u);
  EXPECT_EQ(f.transport.sent[0].method, "PUT");
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/books/41/read-status");
  EXPECT_EQ(f.transport.sent[0].body, R"({"status":"reading"})");
}

TEST(CatalogApiMutations, SetRatingPutsTheBody) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setRating(41, 4).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].method, "PUT");
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/books/41/rating");
  EXPECT_EQ(f.transport.sent[0].body, R"({"rating":4})");
}

TEST(CatalogApiMutations, ClearRatingPutsNull) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setRating(41, 0).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].body, R"({"rating":null})");
}

TEST(CatalogApiMutations, ServerErrorIsReportedAndNotSwallowed) {
  Fixture f;
  f.transport.queued.push_back({500, false, ""});
  EXPECT_EQ(f.api.setRating(41, 3).status, Status::ServerError);
}

TEST(CatalogApiMutations, AuthErrorIsReported) {
  Fixture f;
  f.transport.queued.push_back({401, false, ""});
  EXPECT_EQ(f.api.setReadStatus(41, ReadStatus::Finished).status, Status::Unauthorized);
}
