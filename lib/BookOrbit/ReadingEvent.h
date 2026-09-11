#pragma once

#include <cstddef>
#include <cstdint>

namespace bookorbit {

inline constexpr size_t kEventBytes = 16;

// One page-turn event, mirroring a row of KOReader's page_stat_data table so
// the upload payload needs no translation layer.
//
// `page` and `totalPages` are layout-independent reference pages, so they stay
// comparable across font and margin changes. `totalPages` is recorded on every
// event because the server rescales against it exactly as KOReader's page_stat
// view does.
struct ReadingEvent {
  uint32_t page = 0;
  uint32_t startTime = 0;  // unix epoch seconds; requires a valid RTC
  uint16_t durationSeconds = 0;
  uint16_t totalPages = 0;
  uint32_t reserved = 0;
};

static_assert(sizeof(ReadingEvent) == kEventBytes, "ReadingEvent must stay 16 bytes");

void encodeEvent(const ReadingEvent& event, uint8_t out[kEventBytes]);
bool decodeEvent(const uint8_t in[kEventBytes], ReadingEvent& out);

}  // namespace bookorbit
