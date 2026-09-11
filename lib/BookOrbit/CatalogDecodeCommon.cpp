#include "CatalogDecodeCommon.h"

#include <cstdlib>

namespace bookorbit {
namespace {

// 32 bytes holds any JSON number the catalog emits (ids, byte counts,
// percentages). Well under the 256-byte stack guidance.
constexpr size_t kNumberScratch = 32;

}  // namespace

long parseLong(const char* value, const size_t len) {
  if (value == nullptr || len == 0 || len >= kNumberScratch) return 0;
  char scratch[kNumberScratch];
  memcpy(scratch, value, len);
  scratch[len] = '\0';
  char* end = nullptr;
  const long parsed = strtol(scratch, &end, 10);
  if (end == scratch) return 0;
  return parsed;
}

float parseFloat(const char* value, const size_t len) {
  if (value == nullptr || len == 0 || len >= kNumberScratch) return 0.0f;
  char scratch[kNumberScratch];
  memcpy(scratch, value, len);
  scratch[len] = '\0';
  char* end = nullptr;
  const float parsed = strtof(scratch, &end);
  if (end == scratch) return 0.0f;
  return parsed;
}

}  // namespace bookorbit
