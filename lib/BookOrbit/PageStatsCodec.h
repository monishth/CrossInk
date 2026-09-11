#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ReadingEvent.h"

namespace bookorbit {

// POST /koreader/plugin/page-stats accepts at most this many events.
inline constexpr size_t kStatsBatchSize = 500;

std::string encodePageStats(std::string_view hash, const std::vector<ReadingEvent>& events);

struct PageStatsAck {
  std::vector<std::string> unmatched;
  std::vector<std::pair<std::string, uint32_t>> watermarks;
};

bool decodePageStats(std::string_view json, PageStatsAck& out);

// Computes the watermark for the next round. Returns true when more events
// remain to send.
//
// A full batch may have been cut inside a group of events sharing one
// startTime, so the watermark backs off one second to re-fetch that boundary
// group. Re-sent events are idempotent server-side. Skipping this loses every
// event that shares a second with a batch boundary.
bool nextWatermark(const std::vector<ReadingEvent>& sent, size_t batchSize, uint32_t oldWatermark,
                   uint32_t serverWatermark, uint32_t& outWatermark);

}  // namespace bookorbit
