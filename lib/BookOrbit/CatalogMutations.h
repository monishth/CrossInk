#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bookorbit {

// BookOrbit's catalog read-status vocabulary. Note it is "finished", not P3's
// book-states "complete": the two routes use different words for the same idea
// and the mapping lives here so nothing else has to remember that.
enum class ReadStatus : uint8_t {
  Unread,
  Reading,
  Finished,
  Abandoned,
};

const char* readStatusToWire(ReadStatus status);
bool readStatusFromWire(std::string_view wire, ReadStatus& out);

// {"status":"reading"}
std::string encodeReadStatus(ReadStatus status);

// {"rating":4} for 1..5; {"rating":null} for a clear. Clearing is explicit
// rather than an absent field, because an absent field means "unchanged"
// (bookorbit_api.lua:576-579). Values above the scale clamp to 5.
std::string encodeRating(int rating);

std::string readStatusPath(uint32_t bookId);
std::string ratingPath(uint32_t bookId);

}  // namespace bookorbit
