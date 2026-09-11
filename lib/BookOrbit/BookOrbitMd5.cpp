#include "BookOrbitMd5.h"

#include <cstdint>
#include <cstring>

namespace bookorbit {
namespace {

// floor(2^32 * abs(sin(i + 1))), the RFC 1321 sine table. static const so it
// lives in flash rather than DRAM on device.
constexpr uint32_t kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

constexpr uint8_t kShift[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
                                14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                                4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

inline uint32_t rotateLeft(const uint32_t value, const uint32_t bits) {
  return (value << bits) | (value >> (32 - bits));
}

void transform(uint32_t state[4], const uint8_t block[64]) {
  // The block arrives as unaligned bytes; assemble words with shifts rather
  // than casting to uint32_t*, which would fault on a misaligned address.
  uint32_t m[16];
  for (int i = 0; i < 16; i++) {
    m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 16) | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];

  for (uint32_t i = 0; i < 64; i++) {
    uint32_t f = 0;
    uint32_t g = 0;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) % 16;
    }
    f += a + kSine[i] + m[g];
    a = d;
    d = c;
    c = b;
    b += rotateLeft(f, kShift[i]);
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

}  // namespace

std::string md5Hex(const std::string_view data) {
  uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
  const auto* bytes = reinterpret_cast<const uint8_t*>(data.data());

  size_t offset = 0;
  for (; offset + 64 <= data.size(); offset += 64) {
    transform(state, bytes + offset);
  }

  // Two blocks at most: the 0x80 marker plus the 8-byte length may not fit
  // beside a 56-63 byte remainder. 128 bytes of stack, justified by that.
  uint8_t tail[128] = {};
  const size_t rest = data.size() - offset;
  if (rest > 0) {
    std::memcpy(tail, bytes + offset, rest);
  }
  tail[rest] = 0x80;
  const size_t tailBlocks = (rest + 1 + 8 <= 64) ? 1u : 2u;
  const uint64_t bits = static_cast<uint64_t>(data.size()) * 8u;
  for (int i = 0; i < 8; i++) {
    tail[tailBlocks * 64 - 8 + i] = static_cast<uint8_t>((bits >> (8 * i)) & 0xFF);
  }
  for (size_t block = 0; block < tailBlocks; block++) {
    transform(state, tail + block * 64);
  }

  static const char kHex[] = "0123456789abcdef";
  std::string out(32, '0');
  for (int word = 0; word < 4; word++) {
    for (int byteIndex = 0; byteIndex < 4; byteIndex++) {
      const auto byte = static_cast<uint8_t>((state[word] >> (8 * byteIndex)) & 0xFF);
      out[word * 8 + byteIndex * 2] = kHex[byte >> 4];
      out[word * 8 + byteIndex * 2 + 1] = kHex[byte & 0x0F];
    }
  }
  return out;
}

}  // namespace bookorbit
