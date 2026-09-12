#include "BookOrbitExchangeAck.h"

#include "BookOrbitMatch.h"  // jsonEscape

namespace bookorbit {
namespace {

void appendOptionalString(std::string& json, const char* name, const std::string& value) {
  if (value.empty()) return;
  json += ",\"";
  json += name;
  json += "\":\"";
  json += jsonEscape(value);
  json += '"';
}

}  // namespace

std::string encodeExchangeAck(const std::string_view hash, const std::vector<AppliedAck>& applied,
                              const std::vector<DeletedAck>& deleted) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += R"(","applied":[)";
  for (size_t i = 0; i < applied.size(); i++) {
    const auto& ack = applied[i];
    if (i > 0) json += ',';
    // serverId and version are @IsInt on the server; quoting them is a 400.
    json += R"({"serverId":)";
    json += ack.serverId.empty() ? "0" : ack.serverId;
    json += R"(,"version":)";
    json += std::to_string(ack.version);
    json += R"(,"status":")";
    json += ack.failed ? "failed" : "applied";
    json += '"';
    // ExchangeAckAppliedDto (koreader-exchange.dto.ts:63-101) declares exactly:
    // serverId, version, status, verified, corrected, pos0, pos1, pageno,
    // datetimeUpdated. `key` and `datetime` are NOT among them, and the server
    // runs forbidNonWhitelisted — so sending either rejects the whole ack with
    // a 400 that names no field. The local identity the design doc wanted to
    // report back simply has nowhere to go in this schema.
    appendOptionalString(json, "pos0", ack.pos0);
    json += '}';
  }
  json += R"(],"deleted":[)";
  for (size_t i = 0; i < deleted.size(); i++) {
    if (i > 0) json += ',';
    // ExchangeAckDeletedDto declares only serverId and status
    // (koreader-exchange.dto.ts:104-111), and the server rejects unknown
    // properties outright — so no version here, unlike the applied entries.
    json += R"({"serverId":)";
    json += deleted[i].serverId.empty() ? "0" : deleted[i].serverId;
    json += R"(,"status":")";
    json += deleted[i].failed ? "failed" : "applied";
    json += "\"}";
  }
  json += "]}]}";
  return json;
}

}  // namespace bookorbit
