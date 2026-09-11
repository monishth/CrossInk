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
    json += R"({"serverId":")";
    json += jsonEscape(ack.serverId);
    json += R"(","status":")";
    json += ack.failed ? "failed" : "applied";
    json += '"';
    appendOptionalString(json, "key", ack.key);
    appendOptionalString(json, "datetime", ack.datetime);
    appendOptionalString(json, "pos0", ack.pos0);
    json += '}';
  }
  json += R"(],"deleted":[)";
  for (size_t i = 0; i < deleted.size(); i++) {
    if (i > 0) json += ',';
    json += R"({"serverId":")";
    json += jsonEscape(deleted[i].serverId);
    json += R"(","status":")";
    json += deleted[i].failed ? "failed" : "applied";
    json += "\"}";
  }
  json += "]}]}";
  return json;
}

}  // namespace bookorbit
