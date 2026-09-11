#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bookorbit {

// Approach B's optional device-position blob. Mirrors KOReaderRichPosition
// (lib/KOReaderSync/KOReaderSyncClient.h:23-30) field for field, so
// ProgressMapper::fromRichPosition consumes it unchanged.
//
// KOReader clients ignore this object and use the xpointer, so adding it is
// strictly additive and needs no migration.
struct NativePosition {
  uint32_t pctQ = 0;  // percentage quantized to 0..1,000,000
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pages = 1;
  uint16_t para = 0;
  std::string xpath;
  bool present = false;
};

// Appends "position":{...} to a body already under construction. Appends
// nothing when the position is absent.
void appendNativePosition(std::string& out, const NativePosition& position);

// Reads the position object out of a progress response. A response without one
// decodes successfully with present == false.
bool decodeNativePosition(std::string_view json, NativePosition& out);

}  // namespace bookorbit
