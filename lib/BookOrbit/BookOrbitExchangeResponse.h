#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One server-side change. The same struct carries an annotation and a bookmark:
// a bookmark's "pos" decodes into pos0, and its label into title.
struct RemoteEntry {
  std::string serverId;  // echoed verbatim in the ack; number or string on the wire
  std::string key;       // md5(datetime|pos) identity, present on deletes
  std::string datetime;
  std::string datetimeUpdated;
  std::string drawer;
  std::string color;
  std::string text;
  std::string note;
  std::string chapter;
  std::string title;
  std::string posFormat;
  std::string pos0;
  std::string pos1;
  int32_t pageno = -1;
};

struct ExchangeBookResult {
  std::string hash;
  std::vector<RemoteEntry> add;
  std::vector<RemoteEntry> remove;  // "delete" on the wire; a C++ keyword here
  bool more = false;                // the server has further changes queued
};

struct ExchangeResponse {
  std::vector<std::string> unmatched;
  std::vector<ExchangeBookResult> results;
};

// Returns false only on malformed JSON. A well-formed response with nothing to
// apply is a success with empty vectors.
bool decodeExchangeResponse(std::string_view json, ExchangeResponse& out);

}  // namespace bookorbit
