#include "ReadingEvent.h"

namespace bookorbit {
namespace {

void putU32(uint8_t* out, const uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void putU16(uint8_t* out, const uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFF);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) |
         (static_cast<uint32_t>(in[3]) << 24);
}

uint16_t getU16(const uint8_t* in) {
  return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8));
}

}  // namespace

void encodeEvent(const ReadingEvent& event, uint8_t out[kEventBytes]) {
  putU32(out + 0, event.page);
  putU32(out + 4, event.startTime);
  putU16(out + 8, event.durationSeconds);
  putU16(out + 10, event.totalPages);
  putU32(out + 12, event.reserved);
}

bool decodeEvent(const uint8_t in[kEventBytes], ReadingEvent& out) {
  out.page = getU32(in + 0);
  out.startTime = getU32(in + 4);
  out.durationSeconds = getU16(in + 8);
  out.totalPages = getU16(in + 10);
  out.reserved = getU32(in + 12);
  return true;
}

}  // namespace bookorbit
