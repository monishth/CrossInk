#pragma once

#include <cstdint>

#include "util/DeviceTime.h"

namespace bookorbit_time {

// Wall-clock unix time, or false when the RTC cannot be trusted. The clippings
// and bookmarks stores need the same guarantee for their own timestamps, so the
// implementation lives in util/DeviceTime.h; this name stays for the BookOrbit
// call sites that read as "the time we are allowed to stamp an upload with".
inline bool deviceUnixTime(uint32_t& out) { return device_time::unixTime(out); }

}  // namespace bookorbit_time
