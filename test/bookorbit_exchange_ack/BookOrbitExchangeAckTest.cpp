#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitExchangeAck.h"

using bookorbit::AppliedAck;
using bookorbit::DeletedAck;
using bookorbit::encodeExchangeAck;

namespace {
constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
}

// The server declares serverId and version as @IsInt @Min(1)
// (koreader-exchange.dto.ts:63-70), so both must be emitted unquoted. Quoting
// them made the whole ack a 400, which is what these expectations previously
// enshrined.

TEST(BookOrbitExchangeAck, WrapsOneBookInABooksArray) {
  const std::string json = encodeExchangeAck(kHash, {}, {});
  EXPECT_EQ(json, R"({"books":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f","applied":[],"deleted":[]}]})");
}

// The ack reports only what ExchangeAckAppliedDto declares. `key` and
// `datetime` are not fields on it, and the server rejects unknown properties —
// so the local identity this test used to assert has nowhere to go on the wire.
TEST(BookOrbitExchangeAck, EncodesAnAppliedEntryWithItsPosition) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.version = 3;
  ack.key = "08759494897afa79aec0d37d83498d30";
  ack.datetime = "2026-09-11 14:03:00";
  ack.pos0 = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0";

  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"("serverId":4711)"), std::string::npos);
  EXPECT_NE(json.find(R"("version":3)"), std::string::npos);
  EXPECT_NE(json.find(R"("status":"applied")"), std::string::npos);
  EXPECT_NE(json.find(R"("pos0":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0")"), std::string::npos);
  // Anything the DTO does not declare is a 400, so these must never appear.
  EXPECT_EQ(json.find(R"("key")"), std::string::npos);
  EXPECT_EQ(json.find(R"("datetime")"), std::string::npos);
}

// A failed apply must be reported, not silently dropped: the server keeps the
// entry pending so a later sync can retry it.
TEST(BookOrbitExchangeAck, EncodesAFailedEntry) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.failed = true;
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"("status":"failed")"), std::string::npos);
}

// A failed apply has no local identity to report; those fields are omitted
// rather than sent empty.
TEST(BookOrbitExchangeAck, OmitsOptionalFieldsWhenTheyAreUnknown) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.failed = true;
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_EQ(json.find(R"("pos0")"), std::string::npos);
}

TEST(BookOrbitExchangeAck, EncodesDeletes) {
  const std::string json = encodeExchangeAck(kHash, {}, {{"9", false}, {"10", true}});
  // Deletes carry no version: ExchangeAckDeletedDto does not declare one, and
  // the server rejects unknown properties.
  EXPECT_NE(json.find(R"("deleted":[{"serverId":9,"status":"applied"},{"serverId":10,"status":"failed"}])"),
            std::string::npos);
  EXPECT_EQ(json.find(R"("deleted":[{"serverId":9,"version")"), std::string::npos);
}

TEST(BookOrbitExchangeAck, EncodesSeveralAppliedEntriesInOrder) {
  AppliedAck first;
  first.serverId = "1";
  AppliedAck second;
  second.serverId = "2";
  const std::string json = encodeExchangeAck(kHash, {first, second}, {});
  EXPECT_LT(json.find(R"("serverId":1)"), json.find(R"("serverId":2)"));
}

TEST(BookOrbitExchangeAck, EscapesTheIdentityFields) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.pos0 = R"(/body[1]/"quoted")";
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"(/body[1]/\"quoted\")"), std::string::npos);
}
