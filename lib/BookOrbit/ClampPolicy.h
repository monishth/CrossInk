#pragma once

#include <cstdint>

namespace bookorbit {

// KOReader's DEFAULT_MIN_READ_SEC / DEFAULT_MAX_READ_SEC, from
// plugins/statistics.koplugin/main.lua:29-30. Adopting these exactly is what
// makes on-device statistics comparable to KOReader's for the same reading.
struct ClampSettings {
  uint16_t minSec = 5;
  uint16_t maxSec = 120;
};

// Applies KOReader's rules to one page dwell:
//   below minSec          -> discarded (returns false)
//   minSec..maxSec        -> credited in full
//   above maxSec          -> CLAMPED to maxSec (returns true)
//
// The clamp is the point. CrossInk currently discards long dwells outright,
// which silently loses reading time KOReader would have credited.
bool clampDwell(uint32_t rawSeconds, const ClampSettings& settings, uint16_t& outSeconds);

}  // namespace bookorbit
