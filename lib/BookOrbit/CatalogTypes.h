#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bookorbit {

// The page size the device asks for. Twenty rows is roughly two screens at the
// Small UI scale, so one fetch fills the list and one more prefetches it.
constexpr size_t kCatalogPageSize = 20;

// Hard bound on records retained from one page, regardless of what the server
// sends. A server ignoring "size" must not be able to grow the device's
// working set: extra records are dropped rather than decoded.
constexpr size_t kMaxPageItems = 40;

struct CatalogBook {
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  uint32_t fileBytes = 0;
  uint32_t seriesIndex = 0;
  float progressPercentage = 0.0f;
  uint8_t rating = 0;
  std::string title;
  std::string authors;
  std::string series;
  std::string readStatus;  // "unread" | "reading" | "finished" | "abandoned"
  std::string formats;     // joined with ", " for direct display
  std::string filename;
};

struct CatalogPage {
  std::vector<CatalogBook> items;
  uint32_t page = 1;
  uint32_t size = 0;
  bool hasNext = false;
  std::string query;
};

// A navigation row: a library, collection, series or dashboard shelf.
struct CatalogEntry {
  std::string id;
  std::string title;
  std::string kind;
  uint32_t count = 0;
  std::string seriesId;
};

struct CatalogEntryPage {
  std::vector<CatalogEntry> items;
  uint32_t page = 1;
  bool hasNext = false;
};

struct DashboardSummary {
  std::vector<CatalogBook> continueReading;
  std::vector<CatalogEntry> sections;
  uint32_t totalBooks = 0;
  uint32_t finishedBooks = 0;
};

}  // namespace bookorbit
