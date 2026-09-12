#include "DeviceTime.h"

#include <HalClock.h>

namespace {

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm): exact,
// and free of any dependency on C library timezone state.
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + static_cast<long>(doe) - 719468L;
}

}  // namespace

namespace device_time {

bool unixTime(uint32_t& out) {
  out = 0;
  if (!halClock.isAvailable()) return false;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return false;

  // A year outside this range means the RTC has never been set (it reports
  // 1970 or 2000 from a cold cell) or has been corrupted. Either way the value
  // is not a wall clock, and an event stamped with it would be worse than no
  // event at all.
  if (year < 2025 || year > 2100 || month == 0 || month > 12 || day == 0 || day > 31) return false;

  const long days = daysFromCivil(year, month, day);
  out = static_cast<uint32_t>(days * 86400L + hour * 3600L + minute * 60L);
  return true;
}

}  // namespace device_time
