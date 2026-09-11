#pragma once

#include <cstddef>
#include <cstdint>

namespace bookorbit {

// Nominal page size for books with no x-locations. Arbitrary but must stay
// fixed: getBookSize() is constant for a file, so this yields a totalPages
// that never moves across sessions or font changes.
inline constexpr size_t kNominalPageBytes = 2048;

// A snapshot of what Epub knows about the current position. Passed as plain
// data so this unit needs no Epub dependency and stays host-testable.
//
// Populate from Epub::hasStablePageNumbers(), Epub::resolveReferencePage(),
// Epub::calculateSizeProgress(), and Epub::getBookSize().
struct PageSource {
  bool hasStablePages = false;
  uint32_t referencePage = 0;  // 1-based
  uint32_t referencePageCount = 0;
  float sizeProgress = 0.0f;  // 0..1, used only in the fallback
  size_t bookSize = 0;        // bytes, used only in the fallback
};

// Produces the layout-independent (page, totalPages) pair recorded on every
// event. Returns false when neither source can yield a usable page.
bool resolvePage(const PageSource& source, uint32_t& outPage, uint16_t& outTotal);

}  // namespace bookorbit
