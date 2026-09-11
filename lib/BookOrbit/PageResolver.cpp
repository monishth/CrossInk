#include "PageResolver.h"

namespace bookorbit {
namespace {

uint16_t clampTotal(const size_t total) {
  if (total > 0xFFFF) return 0xFFFF;
  return static_cast<uint16_t>(total);
}

}  // namespace

bool resolvePage(const PageSource& source, uint32_t& outPage, uint16_t& outTotal) {
  if (source.hasStablePages) {
    if (source.referencePageCount == 0 || source.referencePage == 0) return false;
    outPage = source.referencePage;
    outTotal = clampTotal(source.referencePageCount);
    return true;
  }

  if (source.bookSize == 0) return false;

  const size_t total = (source.bookSize + kNominalPageBytes - 1) / kNominalPageBytes;
  if (total == 0) return false;

  float progress = source.sizeProgress;
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;

  size_t page = static_cast<size_t>(progress * static_cast<float>(total)) + 1;
  if (page > total) page = total;  // progress == 1.0 must not overshoot

  outPage = static_cast<uint32_t>(page);
  outTotal = clampTotal(total);
  return true;
}

}  // namespace bookorbit
