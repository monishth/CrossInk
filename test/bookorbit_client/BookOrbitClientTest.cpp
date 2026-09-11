#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class RecordingTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  bookorbit::HttpResponse next;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    return next;
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

std::string headerValue(const bookorbit::HttpRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers) {
    if (key == name) return value;
  }
  return {};
}

}  // namespace

using bookorbit::BookOrbitClient;
using bookorbit::Status;

TEST(BookOrbitClient, SendsAuthHeadersOnEveryRequest) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "monish", "5f4dcc3b5aa765d61d8327deb882cf99",
                         identity());

  std::string body;
  ASSERT_EQ(client.get("/koreader/users/auth", body).status, Status::Ok);
  ASSERT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(headerValue(transport.sent[0], "x-auth-user"), "monish");
  EXPECT_EQ(headerValue(transport.sent[0], "x-auth-key"), "5f4dcc3b5aa765d61d8327deb882cf99");
  EXPECT_EQ(headerValue(transport.sent[0], "accept"), "application/json");
}

TEST(BookOrbitClient, JoinsPathOntoBaseUrl) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.get("/koreader/users/auth", body);
  EXPECT_EQ(transport.sent[0].url, "https://books.example.com/api/v1/koreader/users/auth");
}

TEST(BookOrbitClient, PostSetsContentTypeAndBody) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.postJson("/koreader/plugin/match-check", R"({"hashes":[]})", body);
  EXPECT_EQ(transport.sent[0].method, "POST");
  EXPECT_EQ(headerValue(transport.sent[0], "content-type"), "application/json");
  EXPECT_EQ(transport.sent[0].body, R"({"hashes":[]})");
}

TEST(BookOrbitClient, PutUsesPutMethod) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.putJson("/koreader/syncs/progress", "{}", body);
  EXPECT_EQ(transport.sent[0].method, "PUT");
}

// The body cap is a client-side guard: an oversized body is never sent.
TEST(BookOrbitClient, OversizedBodyIsRejectedWithoutSending) {
  RecordingTransport transport;
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  const std::string huge(900 * 1024 + 1, 'x');
  std::string body;
  EXPECT_EQ(client.postJson("/koreader/plugin/page-stats", huge, body).status, Status::BodyTooLarge);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitClient, BodyExactlyAtCapIsSent) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  const std::string atCap(900 * 1024, 'x');
  std::string body;
  EXPECT_EQ(client.postJson("/koreader/plugin/page-stats", atCap, body).status, Status::Ok);
  EXPECT_EQ(transport.sent.size(), 1u);
}

TEST(BookOrbitClient, TransportFailureIsClassified) {
  RecordingTransport transport;
  transport.next = {0, true, ""};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  EXPECT_EQ(client.get("/koreader/users/auth", body).status, Status::Transport);
}

TEST(BookOrbitClient, UnauthorizedIsClassified) {
  RecordingTransport transport;
  transport.next = {401, false, ""};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  EXPECT_EQ(client.get("/koreader/users/auth", body).status, Status::Unauthorized);
}

TEST(BookOrbitClient, ResponseBodyIsReturned) {
  RecordingTransport transport;
  transport.next = {200, false, R"({"authorized":"OK"})"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  ASSERT_EQ(client.get("/koreader/users/auth", body).status, Status::Ok);
  EXPECT_EQ(body, R"({"authorized":"OK"})");
}

TEST(BookOrbitClient, WithDeviceInjectsIdentityFields) {
  const std::string merged = BookOrbitClient::withDevice(R"({"hashes":["abc"]})", identity(), "2026-09-11 14:03:00");
  EXPECT_NE(merged.find(R"("hashes":["abc"])"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceModel":"Xteink X4 Pro")"), std::string::npos);
  EXPECT_NE(merged.find(R"("pluginVersion":"0.1.0")"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceTime":"2026-09-11 14:03:00")"), std::string::npos);
}

TEST(BookOrbitClient, WithDeviceHandlesEmptyObject) {
  const std::string merged = BookOrbitClient::withDevice("{}", identity(), "2026-09-11 14:03:00");
  EXPECT_EQ(merged.front(), '{');
  EXPECT_EQ(merged.back(), '}');
  EXPECT_NE(merged.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  // No stray comma after the opening brace.
  EXPECT_EQ(merged.find("{,"), std::string::npos);
}
