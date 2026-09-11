#include "ClampPolicy.h"

namespace bookorbit {

bool clampDwell(const uint32_t rawSeconds, const ClampSettings& settings, uint16_t& outSeconds) {
  if (rawSeconds < settings.minSec) {
    return false;
  }
  if (rawSeconds > settings.maxSec) {
    outSeconds = settings.maxSec;
    return true;
  }
  outSeconds = static_cast<uint16_t>(rawSeconds);
  return true;
}

}  // namespace bookorbit
