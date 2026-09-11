#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace bookorbit {

// Fixed key buffer. The longest key in the catalog protocol is
// "progressPercentage" (18 chars); 48 bytes leaves room for future fields
// without ever touching the heap. Keeping this fixed is what makes decoding
// O(1) in the body size rather than O(n).
struct KeyBuf {
  char data[48] = {0};
  size_t len = 0;

  void set(const char* key, const size_t n) {
    len = n < sizeof(data) ? n : sizeof(data) - 1;
    memcpy(data, key, len);
    data[len] = '\0';
  }
  void clear() {
    len = 0;
    data[0] = '\0';
  }
  bool is(const std::string_view name) const { return std::string_view(data, len) == name; }
};

// Nesting bookkeeping shared by every catalog decoder. The root object is
// objectDepth 1 and a page item is objectDepth 2. Array depth is tracked
// separately, and the depth at which the item array opened is remembered, so a
// nested array inside an item ("formats") can never be mistaken for the end of
// the item array.
struct DecodeScope {
  int objectDepth = 0;
  int arrayDepth = 0;
  int itemsArrayDepth = -1;
  bool inItems = false;
  bool inItem = false;
};

// Bounded numeric parsing over a non-null-terminated token. Returns 0 when the
// token does not fit the scratch buffer or does not parse.
long parseLong(const char* value, size_t len);
float parseFloat(const char* value, size_t len);

}  // namespace bookorbit
