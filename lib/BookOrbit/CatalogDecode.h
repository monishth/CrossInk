#pragma once

#include <cstddef>
#include <string_view>

#include "CatalogTypes.h"

namespace bookorbit {

// Decodes a /catalog/books or /catalog/sections/{section} book page. The body
// is streamed through StreamingJsonParser and never assembled into a DOM, so
// peak memory is the parser's 512-byte token buffer plus the bounded item
// vector — not the body, which can approach the 900 KiB cap.
// Resets `out` before decoding. Returns false on malformed JSON.
bool decodeBookPage(std::string_view json, CatalogPage& out);

// Same decode, fed in fixed-size fragments. Exists so tests can prove the
// result does not depend on socket chunk boundaries.
bool decodeBookPageChunked(std::string_view json, size_t chunkSize, CatalogPage& out);

// Decodes a navigation page: /catalog/root, /catalog/dashboard/discover,
// /catalog/dashboard/sections/{type} and /catalog/sections/{section} all share
// the {items:[{id,title,kind,count}], page, hasNext} envelope. Numeric ids are
// kept as strings so a library id and a slug travel the same path.
// Resets `out`. Returns false on malformed JSON.
bool decodeEntryPage(std::string_view json, CatalogEntryPage& out);

// Decodes /catalog/dashboard: counters plus the Continue-reading book list and
// the configured shelf list. Streamed like every other catalog body.
bool decodeDashboard(std::string_view json, DashboardSummary& out);
}  // namespace bookorbit
