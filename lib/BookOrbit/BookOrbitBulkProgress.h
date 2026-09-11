#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// POST /koreader/plugin/progress accepts 100 items per request.
inline constexpr size_t kBulkProgressBatchSize = 100;

struct BulkProgressItem {
  std::string hash;         // partial MD5
  float percentage = 0.0f;  // 0..1
  std::string progress;     // canonical crengine xpointer
  uint32_t timestamp = 0;   // unix epoch
};

// Encodes one batch starting at offset, consuming at most kBulkProgressBatchSize
// items. consumed reports how many input items the batch covered, including any
// skipped for want of an xpointer, so the caller can advance without looping.
// Returns "" when nothing was emitted.
//
// The four device fields required on every /koreader/plugin/* POST are injected
// by BookOrbitClient's withDevice path, not here.
std::string encodeBulkProgress(const std::vector<BulkProgressItem>& items, size_t offset, size_t& consumed);

// Decodes {"unmatched":[hash]}. A hash listed here is not in the server's
// library, so its book must not advance any watermark.
bool decodeBulkProgressResponse(std::string_view json, std::vector<std::string>& unmatched);

}  // namespace bookorbit
