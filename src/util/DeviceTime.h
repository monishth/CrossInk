#pragma once

#include <cstdint>

namespace device_time {

/**
 * Wall-clock unix time from the RTC, or false when it cannot be trusted.
 *
 * Deliberately not std::time(nullptr): on the ESP32 the C library clock starts
 * at the epoch and only advances by uptime until something syncs it, so an
 * unsynced device reports timestamps in January 1970. Those look like ordinary
 * values, upload happily, and permanently skew anything derived from them.
 *
 * The RTC holds UTC, which is what unix time wants, so no offset is applied.
 * Returns false when the RTC is absent, unreadable, or reporting a year outside
 * a plausible range.
 */
bool unixTime(uint32_t& out);

}  // namespace device_time
