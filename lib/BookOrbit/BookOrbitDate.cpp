#include "BookOrbitDate.h"

#include <cstdio>

namespace bookorbit {
namespace {

bool isDigit(const char ch) { return ch >= '0' && ch <= '9'; }

bool isLeap(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeap(year)) return 29;
  return kDays[month - 1];
}

uint16_t digits(const std::string_view text, const size_t offset, const size_t count) {
  uint16_t value = 0;
  for (size_t i = 0; i < count; i++) {
    value = static_cast<uint16_t>(value * 10 + (text[offset + i] - '0'));
  }
  return value;
}

}  // namespace

bool parseDateOnly(const std::string_view text, DateOnly& out) {
  if (text.size() < 10) return false;
  for (size_t i = 0; i < 10; i++) {
    const bool wantDash = (i == 4 || i == 7);
    if (wantDash && text[i] != '-') return false;
    if (!wantDash && !isDigit(text[i])) return false;
  }

  const uint16_t year = digits(text, 0, 4);
  const uint16_t month = digits(text, 5, 2);
  const uint16_t day = digits(text, 8, 2);
  if (year == 0 || month < 1 || month > 12) return false;
  if (day < 1 || day > daysInMonth(year, static_cast<uint8_t>(month))) return false;

  out.year = year;
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  return true;
}

void formatDateOnly(const DateOnly& date, char* buf, const size_t len) {
  if (buf == nullptr || len == 0) return;
  if (!date.valid()) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, len, "%04u-%02u-%02u", static_cast<unsigned>(date.year), static_cast<unsigned>(date.month),
           static_cast<unsigned>(date.day));
}

int compareDateOnly(const DateOnly& lhs, const DateOnly& rhs) {
  if (lhs.year != rhs.year) return lhs.year < rhs.year ? -1 : 1;
  if (lhs.month != rhs.month) return lhs.month < rhs.month ? -1 : 1;
  if (lhs.day != rhs.day) return lhs.day < rhs.day ? -1 : 1;
  return 0;
}

Winner resolveByDate(const DateOnly& local, const DateOnly& server) {
  if (!local.valid()) return Winner::Server;
  if (!server.valid()) return Winner::Local;
  return compareDateOnly(local, server) > 0 ? Winner::Local : Winner::Server;
}

}  // namespace bookorbit
