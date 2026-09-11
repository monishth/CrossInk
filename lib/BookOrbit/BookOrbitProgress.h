#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bookorbit {

// One reading position, in the shape BookOrbit's KOSync-compatible progress
// endpoints use in both directions.
struct ProgressRecord {
  std::string document;     // partial MD5, request only
  float percentage = 0.0f;  // 0..1
  std::string progress;     // canonical crengine xpointer
  std::string device;       // human-readable device name
  std::string deviceId;     // stable device identifier
  uint32_t timestamp = 0;   // unix epoch
};

// GET /koreader/syncs/progress/{digest}. Returns "" for an empty digest.
std::string progressGetPath(std::string_view digest);

// PUT /koreader/syncs/progress body. Returns "" when the record is incomplete —
// a record with no xpointer or no document is never sent, because a
// percentage-only push is exactly the silent degradation this phase forbids.
std::string encodePutProgress(const ProgressRecord& record);

// Decodes either endpoint's response. The progress field is normalized to the
// canonical xpointer form when it parses, and kept verbatim when it does not.
bool decodeProgressResponse(std::string_view json, ProgressRecord& out);

// Shared JSON string escaping, also used by the bulk codec.
void appendJsonString(std::string& out, std::string_view value);

// Shared percentage formatting: clamped to 0..1, six decimal places.
void appendPercentage(std::string& out, float percentage);

}  // namespace bookorbit
