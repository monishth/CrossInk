# BookOrbit P5 — Catalog and Downloads Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the device browse a BookOrbit library — root, dashboard, sections, filtered book pages — and stream book files and thumbnails to the SD card, ending with the downloaded file's KOReader partial MD5 so a freshly downloaded book is immediately syncable by every other phase.

**Architecture:** All decoding, pagination and transfer policy lives in `lib/BookOrbit/` behind injected interfaces — `IHttpTransport` (P0), `IFileSink`, `IDocumentHasher` — so the whole phase compiles and runs under the native GoogleTest suite with no Arduino dependency. Device bindings (`SecureHttpClient` streaming GET, `FsFile` sink, `KOReaderDocumentId`) are the last two tasks. Catalog bodies approach the 900 KiB cap, so every response is decoded through `StreamingJsonParser` and never buffered into a DOM; every download is written through a `DataCallback` and never buffered at all.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), `lib/JsonParser/StreamingJsonParser` for response decoding, `freeink::SecureHttpClient` for streamed TLS transfers, FreeInkUI list components for the browse screen.

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md` — section "P5 — Catalog and downloads".

**Depends on:** `docs/superpowers/plans/2026-09-11-bookorbit-p0-client-and-state.md` (Tasks 1–9). P5 consumes `bookorbit::BookOrbitClient`, `bookorbit::Error`/`Status`, `bookorbit::CapabilityCache`, `bookorbit::IBlobStore` and `bookorbit::jsonEscape` unchanged.

## Global Constraints

- Repo guide is `AGENTS.md` (mirrored to `CLAUDE.md`). Its rules bind every task.
- Branch prefix `feat/`; commit messages `<type>: <short summary>`.
- All user-facing strings via `tr(STR_*)`. Logs may be hardcoded.
- No exceptions, no `abort()`. `LOG_ERR(...)` then `return false` on recoverable failure.
- `new` is not nothrow on ESP32. Use `new (std::nothrow)` or `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`.
- Local stack allocations over 256 bytes must be justified in a comment.
- Prefer `string_view`, `char[]`, `snprintf` over `std::string` in hot paths. `string_view::data()` is **not** null-terminated — never pass it to a C API.
- File I/O uses `FsFile`, never Arduino `File`. Always close explicitly.
- Shared code must stay within ESP32-C3 limits (~380 KB internal RAM, no PSRAM) unless capability-gated.
- Request bodies capped at **900 KiB** (`MAX_BODY_BYTES = 900 * 1024`).
- Base URL always normalized to end in `/api/v1`.
- Auth headers on every request: `accept: application/json`, `x-auth-user: <username>`, `x-auth-key: <lowercase hex md5(password)>`.
- Large responses are read through `lib/JsonParser/StreamingJsonParser` (512-byte token buffer, 32 nesting levels, constant memory), never ArduinoJson's whole-body-in-memory path.
- Capability names in use here: `"catalogDashboardSections"`. Tri-state rule from P0 Task 3: any 5xx or transport error yields *Unknown* and is never cached as a negative; only a definitive 4xx caches a negative.
- Do not edit generated files: `src/network/html/*.generated.h`, `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`, icon headers, hyphenation tries.
- Add a `CHANGELOG.md` entry for user-facing changes, grouped under Added/Changed/Fixed.
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally: `pio run -e x4-pro` and `pio run -e default`.

### P5-specific constraints

- **Query keys are URL-encoded and sorted alphabetically; `nil`/empty values are dropped.** Ported verbatim from `bookorbit_api.lua:266-283`.
- **Catalog browsing paginates by page number** (`page`, `size`, `q`, plus `libraryId`, `collectionId`, `author`, `seriesId`, `sort`, `order`, `readStatus`, `format`).
- **The bulk manifest paginates by cursor**: the response carries `{hasNext, nextCursor}`; a rejected or unknown cursor (`restartRequired: true`) restarts enumeration from scratch (`bookorbit_catalog_bulk_download.lua:780-810`).
- **Downloads stream to `"<name>.part"` and are published by atomic rename.** A `.part` file must never be mistaken for a complete book.
- **Same-origin redirects capped at 5 hops.** `SecureHttpClient::setFollowRedirects(5)`; a cross-origin or scheme-downgrading hop is refused.
- **A byte cap is enforced mid-stream**, derived from the server-reported size: `min(512 MiB, ceil(expected * 1.25) + 1 MiB)`; `512 MiB` when the size is unknown (`bookorbit_transfer_policy.lua:17-36`).
- **Thumbnail responses must carry a `Content-Type` starting with `image/`**, checked before the first byte is written.
- **The finished download is hashed with the existing `KOReaderDocumentId::calculate()`** — 12 offsets at `1024 << 2i`, 1024 bytes each. That 32-char lowercase hex string is BookOrbit's only book key: it is `MatchCandidate.hash` in P0's `match-check`, the key of `BookSyncState`, and the `hash` field of P1's page-stats, P2's progress, P3's book-states and P4's exchanges. Downloading a book therefore makes it syncable with no further identification step.

### Memory discipline (the binding constraint of this phase)

Every buffer introduced by P5 is justified here and re-justified at its definition:

| Buffer | Size | Justification |
|---|---|---|
| `StreamingJsonParser` token buffer | 512 B (existing) | Owned by the parser, already budgeted. One parser instance is alive at a time. |
| `KeyBuf::data` | 48 B | Longest catalog key is `progressPercentage` (18 chars). Fixed, no heap. |
| `CatalogPage::items` | ≤ `kMaxPageItems` (40) records | The device requests `size=20`; 40 is a defensive bound against a server ignoring `size`. Bounds a page at roughly 9 KB of records plus string payload, not the whole body. |
| Download chunk buffer | 2048 B, owned by `SecureHttpClient` | Matches `OPDS_DOWNLOAD_BUFFER_SIZE` in `OpdsBookBrowserActivity.cpp:36`. `PartFileWriter` adds **no** buffer of its own — bytes go callback → `FsFile::write`. |
| `KOReaderDocumentId` read chunk | 1024 B (existing) | Already in `lib/KOReaderSync`. Runs after the transfer, never concurrently with it. |

No catalog body, no download body, and no manifest page is ever held whole in RAM. The peak working set of a catalog fetch is the parser plus one bounded page of records; the peak of a download is one 2 KB socket chunk. Both fit the ESP32-C3's ~380 KB internal RAM, so nothing in `lib/BookOrbit/` needs capability gating.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/CatalogQuery.{h,cpp}` | Percent-encoding, sorted query assembly, empty-value dropping. Pure string logic. |
| `lib/BookOrbit/CatalogDecodeCommon.{h,cpp}` | Shared key buffer, depth bookkeeping, bounded number parsing for every streaming decoder. |
| `lib/BookOrbit/CatalogTypes.h` | `CatalogBook`, `CatalogPage`, `CatalogEntry`, `CatalogEntryPage`, `DashboardSummary`, `ManifestItem`, `ManifestPage`. Data only. |
| `lib/BookOrbit/CatalogDecode.{h,cpp}` | `StreamingJsonParser` decoders for book pages, entry pages and the dashboard. |
| `lib/BookOrbit/CatalogApi.{h,cpp}` | Endpoint methods over `BookOrbitClient`, including the `catalogDashboardSections` capability gate. |
| `lib/BookOrbit/CatalogManifest.{h,cpp}` | Manifest page decode plus the cursor enumerator and its restart rule. |
| `lib/BookOrbit/IFileSink.h` | Injected file interface: open / write / close / publish / remove / exists. No implementation. |
| `lib/BookOrbit/CatalogDownload.{h,cpp}` | `PartFileWriter`: `.part` streaming, byte cap, atomic publish, abandonment. |
| `lib/BookOrbit/CatalogSyncKey.{h,cpp}` | `IDocumentHasher` and `finalizeDownload()` — publish, then hash, then hand back the sync key. |
| `lib/BookOrbit/CatalogThumbnail.{h,cpp}` | `image/` content-type guard and thumbnail paths. |
| `lib/BookOrbit/CatalogMutations.{h,cpp}` | `read-status` / `rating` PUT bodies and paths. |
| `lib/BookOrbit/SameOrigin.{h,cpp}` | Redirect origin comparison. Pure string logic, host-tested. |
| `src/network/BookOrbitFileSink.{h,cpp}` | `IFileSink` over `FsFile` / `HalStorage`. Device-only. |
| `src/network/BookOrbitDocumentHasher.{h,cpp}` | `IDocumentHasher` over `KOReaderDocumentId::calculate()`. Device-only. |
| `src/network/BookOrbitStreamDownloader.{h,cpp}` | `SecureHttpClient` streaming GET with capped same-origin redirects and progress. Device-only. |
| `src/activities/bookorbit/BookOrbitCatalogActivity.{h,cpp}` | FreeInkUI browse/download screen, modelled on `OpdsBookBrowserActivity`. Device-only. |
| `test/bookorbit_catalog_*/` | One GoogleTest target per unit, registered in `test/CMakeLists.txt`. |

---

### Task 1: Catalog query string builder

**Files:**
- Create: `lib/BookOrbit/CatalogQuery.h`, `lib/BookOrbit/CatalogQuery.cpp`
- Create: `test/bookorbit_catalog_query/CMakeLists.txt`, `test/bookorbit_catalog_query/CatalogQueryTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(bookorbit_catalog_query)` beside the existing entries)

**Interfaces:**
- Consumes: nothing.
- Produces:
  `std::string bookorbit::urlEncodeComponent(std::string_view value)`;
  `class bookorbit::CatalogQuery` with
  `void set(std::string_view key, std::string_view value)`,
  `void set(std::string_view key, long value)`,
  `void clear(std::string_view key)`,
  `bool empty() const`,
  `std::string build(std::string_view path) const`.

Ported from `bookorbit_api.lua:266-283`: drop `nil`/empty values, sort the surviving keys, percent-encode both key and value.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_query/CatalogQueryTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogQuery.h"

using bookorbit::CatalogQuery;
using bookorbit::urlEncodeComponent;

TEST(UrlEncodeComponent, KeepsUnreservedCharacters) {
  EXPECT_EQ(urlEncodeComponent("abcXYZ091-_.~"), "abcXYZ091-_.~");
}

TEST(UrlEncodeComponent, EncodesSpacesAndPunctuation) {
  EXPECT_EQ(urlEncodeComponent("Richard Osman"), "Richard%20Osman");
  EXPECT_EQ(urlEncodeComponent("a&b=c"), "a%26b%3Dc");
  EXPECT_EQ(urlEncodeComponent("50%"), "50%25");
}

TEST(UrlEncodeComponent, EncodesHighBytesUppercaseHex) {
  // UTF-8 "é" is 0xC3 0xA9. Hex digits must be uppercase, as util.urlEncode emits.
  EXPECT_EQ(urlEncodeComponent("\xc3\xa9"), "%C3%A9");
}

TEST(CatalogQuery, PathIsReturnedUnchangedWhenNoParameters) {
  const CatalogQuery query;
  EXPECT_TRUE(query.empty());
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books");
}

// The ordering rule: keys sorted alphabetically, not in insertion order.
TEST(CatalogQuery, SortsKeysAlphabetically) {
  CatalogQuery query;
  query.set("sort", "recently_added");
  query.set("page", 2);
  query.set("author", "Osman");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"),
            "/koreader/plugin/catalog/books?author=Osman&page=2&sort=recently_added");
}

TEST(CatalogQuery, DropsEmptyValues) {
  CatalogQuery query;
  query.set("q", "");
  query.set("page", 1);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=1");
}

TEST(CatalogQuery, ClearRemovesAPreviouslySetKey) {
  CatalogQuery query;
  query.set("page", 3);
  query.set("q", "murders");
  query.clear("q");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=3");
}

TEST(CatalogQuery, SettingAKeyTwiceReplacesIt) {
  CatalogQuery query;
  query.set("page", 1);
  query.set("page", 4);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=4");
}

TEST(CatalogQuery, EncodesBothKeysAndValues) {
  CatalogQuery query;
  query.set("q", "We Solve Murders");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"),
            "/koreader/plugin/catalog/books?q=We%20Solve%20Murders");
}

TEST(CatalogQuery, CarriesTheFullCatalogFilterSet) {
  CatalogQuery query;
  query.set("libraryId", 3);
  query.set("collectionId", 7);
  query.set("author", "Osman");
  query.set("seriesId", 11);
  query.set("sort", "series");
  query.set("order", "asc");
  query.set("readStatus", "reading");
  query.set("format", "epub");
  query.set("page", 1);
  query.set("size", 20);
  query.set("q", "murder");
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"),
            "/koreader/plugin/catalog/books?author=Osman&collectionId=7&format=epub&libraryId=3&order=asc"
            "&page=1&q=murder&readStatus=reading&seriesId=11&size=20&sort=series");
}

TEST(CatalogQuery, NegativeAndZeroNumbersAreKept) {
  CatalogQuery query;
  query.set("page", 0);
  EXPECT_EQ(query.build("/koreader/plugin/catalog/books"), "/koreader/plugin/catalog/books?page=0");
}
```

Create `test/bookorbit_catalog_query/CMakeLists.txt`:

```cmake
add_executable(CatalogQueryTest
  CatalogQueryTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
)

target_include_directories(CatalogQueryTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogQueryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogQueryTest)
```

Add to `test/CMakeLists.txt`, after the last existing `add_subdirectory(...)` line:

```cmake
add_subdirectory(bookorbit_catalog_query)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogQuery.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogQuery.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bookorbit {

// Percent-encodes one query component. Unreserved set is RFC 3986's
// ALPHA / DIGIT / "-" / "_" / "." / "~", matching KOReader's util.urlEncode,
// so a URL built here is byte-identical to the one the Lua plugin sends.
std::string urlEncodeComponent(std::string_view value);

// Catalog query parameters. Empty values are dropped, keys are sorted
// alphabetically, and both sides of every pair are percent-encoded — the three
// rules from bookorbit_api.lua:266-283. Sorting is what makes a URL stable
// enough to compare in a test and to use as a cache key.
class CatalogQuery {
 public:
  CatalogQuery() { params_.reserve(kTypicalParams); }

  void set(std::string_view key, std::string_view value);
  void set(std::string_view key, long value);
  void clear(std::string_view key);

  bool empty() const { return params_.empty(); }

  // Returns path unchanged when no parameter survived the empty-value drop.
  std::string build(std::string_view path) const;

 private:
  // The widest catalog request carries 11 parameters (see the filter set in
  // the spec). Reserving that avoids reallocation without a fixed array.
  static constexpr size_t kTypicalParams = 12;

  std::vector<std::pair<std::string, std::string>> params_;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogQuery.cpp`:

```cpp
#include "CatalogQuery.h"

#include <algorithm>
#include <cstdio>

namespace bookorbit {
namespace {

bool isUnreserved(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

constexpr char kHex[] = "0123456789ABCDEF";

}  // namespace

std::string urlEncodeComponent(const std::string_view value) {
  std::string encoded;
  encoded.reserve(value.size() + value.size() / 4);
  for (const char raw : value) {
    const auto c = static_cast<unsigned char>(raw);
    if (isUnreserved(c)) {
      encoded.push_back(raw);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[c >> 4]);
      encoded.push_back(kHex[c & 0x0F]);
    }
  }
  return encoded;
}

void CatalogQuery::set(const std::string_view key, const std::string_view value) {
  if (key.empty()) return;
  if (value.empty()) {
    clear(key);
    return;
  }
  for (auto& param : params_) {
    if (param.first == key) {
      param.second.assign(value);
      return;
    }
  }
  params_.emplace_back(std::string(key), std::string(value));
}

void CatalogQuery::set(const std::string_view key, const long value) {
  // 21 bytes holds any 64-bit decimal with sign and terminator; well under the
  // 256-byte stack guidance.
  char buffer[21];
  const int written = snprintf(buffer, sizeof(buffer), "%ld", value);
  if (written <= 0) return;
  set(key, std::string_view(buffer, static_cast<size_t>(written)));
}

void CatalogQuery::clear(const std::string_view key) {
  for (auto it = params_.begin(); it != params_.end(); ++it) {
    if (it->first == key) {
      params_.erase(it);
      return;
    }
  }
}

std::string CatalogQuery::build(const std::string_view path) const {
  if (params_.empty()) return std::string(path);

  std::vector<const std::pair<std::string, std::string>*> ordered;
  ordered.reserve(params_.size());
  for (const auto& param : params_) ordered.push_back(&param);
  std::sort(ordered.begin(), ordered.end(),
            [](const auto* lhs, const auto* rhs) { return lhs->first < rhs->first; });

  std::string url(path);
  url.reserve(path.size() + ordered.size() * 24);
  char separator = '?';
  for (const auto* param : ordered) {
    url.push_back(separator);
    separator = '&';
    url += urlEncodeComponent(param->first);
    url.push_back('=');
    url += urlEncodeComponent(param->second);
  }
  return url;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "CatalogQuery|UrlEncodeComponent" --output-on-failure
```

Expected: 12 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogQuery.h lib/BookOrbit/CatalogQuery.cpp test/bookorbit_catalog_query test/CMakeLists.txt
git commit -m "feat: add BookOrbit catalog query string builder"
```

---

### Task 2: Streaming book-page decoder

**Files:**
- Create: `lib/BookOrbit/CatalogDecodeCommon.h`, `lib/BookOrbit/CatalogDecodeCommon.cpp`
- Create: `lib/BookOrbit/CatalogTypes.h`
- Create: `lib/BookOrbit/CatalogDecode.h`, `lib/BookOrbit/CatalogDecode.cpp`
- Create: `test/bookorbit_catalog_decode/CMakeLists.txt`, `test/bookorbit_catalog_decode/CatalogDecodeTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamingJsonParser`, `JsonCallbacks` (`lib/JsonParser/StreamingJsonParser.h`).
- Produces:
  `struct bookorbit::KeyBuf`; `struct bookorbit::DecodeScope`;
  `long bookorbit::parseLong(const char* value, size_t len)`;
  `float bookorbit::parseFloat(const char* value, size_t len)`;
  `struct bookorbit::CatalogBook { uint32_t bookId, fileId, fileBytes, seriesIndex; float progressPercentage; uint8_t rating; std::string title, authors, series, readStatus, formats, filename; }`;
  `struct bookorbit::CatalogPage { std::vector<CatalogBook> items; uint32_t page, size; bool hasNext; std::string query; }`;
  `bool bookorbit::decodeBookPage(std::string_view json, CatalogPage& out)`;
  `constexpr size_t bookorbit::kCatalogPageSize = 20`; `constexpr size_t bookorbit::kMaxPageItems = 40`.

Decoding is single-pass over the body with the C-callback parser. The body is never assembled into a DOM: the only growing allocation is the bounded `items` vector.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_decode/CatalogDecodeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogDecode.h"

using bookorbit::CatalogPage;
using bookorbit::decodeBookPage;

namespace {

const char* kTwoBookPage = R"({
  "page": 2,
  "size": 20,
  "hasNext": true,
  "query": "murder",
  "items": [
    {"id": 41, "title": "We Solve Murders", "authors": "Richard Osman",
     "series": "We Solve Murders", "seriesIndex": 1, "readStatus": "reading",
     "rating": 4, "progressPercentage": 37.5, "formats": ["epub", "mobi"],
     "fileId": 902, "fileBytes": 1258291, "filename": "we-solve-murders.epub"},
    {"id": 42, "title": "The Last Devil to Die", "authors": "Richard Osman",
     "readStatus": "finished", "rating": 5, "progressPercentage": 100,
     "formats": ["epub"], "fileId": 903, "fileBytes": 981234,
     "filename": "the-last-devil-to-die.epub"}
  ]
})";

}  // namespace

TEST(CatalogDecode, DecodesPageEnvelope) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  EXPECT_EQ(page.page, 2u);
  EXPECT_EQ(page.size, 20u);
  EXPECT_TRUE(page.hasNext);
  EXPECT_EQ(page.query, "murder");
}

TEST(CatalogDecode, DecodesEveryItem) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].bookId, 41u);
  EXPECT_EQ(page.items[0].title, "We Solve Murders");
  EXPECT_EQ(page.items[0].authors, "Richard Osman");
  EXPECT_EQ(page.items[0].series, "We Solve Murders");
  EXPECT_EQ(page.items[0].seriesIndex, 1u);
  EXPECT_EQ(page.items[0].readStatus, "reading");
  EXPECT_EQ(page.items[0].rating, 4);
  EXPECT_FLOAT_EQ(page.items[0].progressPercentage, 37.5f);
  EXPECT_EQ(page.items[0].fileId, 902u);
  EXPECT_EQ(page.items[0].fileBytes, 1258291u);
  EXPECT_EQ(page.items[0].filename, "we-solve-murders.epub");
  EXPECT_EQ(page.items[1].bookId, 42u);
  EXPECT_EQ(page.items[1].readStatus, "finished");
}

// The nested "formats" array must not terminate the "items" array. This is the
// bug the separate array-depth tracking exists to prevent.
TEST(CatalogDecode, NestedFormatsArrayDoesNotCloseItems) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_EQ(page.items.size(), 2u);
  EXPECT_EQ(page.items[0].formats, "epub, mobi");
  EXPECT_EQ(page.items[1].formats, "epub");
}

TEST(CatalogDecode, MissingFieldsKeepDefaults) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"items":[{"id":7,"title":"Untitled"}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 7u);
  EXPECT_EQ(page.items[0].authors, "");
  EXPECT_EQ(page.items[0].rating, 0);
  EXPECT_EQ(page.items[0].fileId, 0u);
  EXPECT_FALSE(page.hasNext);
  EXPECT_EQ(page.page, 1u);
}

TEST(CatalogDecode, EmptyItemsArrayIsAValidPage) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"page":1,"hasNext":false,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
  EXPECT_FALSE(page.hasNext);
}

TEST(CatalogDecode, NullFieldsAreTolerated) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(R"({"items":[{"id":7,"series":null,"rating":null}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].series, "");
  EXPECT_EQ(page.items[0].rating, 0);
}

// A server ignoring "size" must not be able to grow the device's working set
// without bound. Extra records past the cap are dropped, not decoded.
TEST(CatalogDecode, ItemsAreCappedAtMaxPageItems) {
  std::string json = R"({"items":[)";
  for (size_t i = 0; i < bookorbit::kMaxPageItems + 25; ++i) {
    if (i > 0) json += ',';
    json += R"({"id":)" + std::to_string(i + 1) + R"(,"title":"t"})";
  }
  json += "]}";

  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(json, page));
  EXPECT_EQ(page.items.size(), bookorbit::kMaxPageItems);
  EXPECT_EQ(page.items.front().bookId, 1u);
}

TEST(CatalogDecode, MalformedJsonFails) {
  CatalogPage page;
  EXPECT_FALSE(decodeBookPage(R"({"items":[{"id":7,)", page));
}

TEST(CatalogDecode, EmptyBodyFails) {
  CatalogPage page;
  EXPECT_FALSE(decodeBookPage("", page));
}

// Decoding is fed in fragments the way the transport delivers them; the result
// must not depend on chunk boundaries.
TEST(CatalogDecode, ResultIsIndependentOfChunking) {
  CatalogPage whole;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, whole));

  CatalogPage chunked;
  ASSERT_TRUE(bookorbit::decodeBookPageChunked(kTwoBookPage, 7, chunked));
  ASSERT_EQ(chunked.items.size(), whole.items.size());
  EXPECT_EQ(chunked.items[0].title, whole.items[0].title);
  EXPECT_EQ(chunked.items[1].filename, whole.items[1].filename);
  EXPECT_EQ(chunked.hasNext, whole.hasNext);
}

TEST(CatalogDecode, DecodeResetsAnyPreviousContent) {
  CatalogPage page;
  ASSERT_TRUE(decodeBookPage(kTwoBookPage, page));
  ASSERT_TRUE(decodeBookPage(R"({"page":1,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
  EXPECT_FALSE(page.hasNext);
  EXPECT_EQ(page.query, "");
}
```

Create `test/bookorbit_catalog_decode/CMakeLists.txt`:

```cmake
add_executable(CatalogDecodeTest
  CatalogDecodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogDecodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogDecodeTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_decode)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogDecode.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogDecodeCommon.h`:

```cpp
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
```

Create `lib/BookOrbit/CatalogDecodeCommon.cpp`:

```cpp
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
```

Create `lib/BookOrbit/CatalogTypes.h`:

```cpp
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
```

Create `lib/BookOrbit/CatalogDecode.h`:

```cpp
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

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogDecode.cpp`:

```cpp
#include "CatalogDecode.h"

#include "CatalogDecodeCommon.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

struct BookPageCtx {
  CatalogPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogBook current;
  bool inFormats = false;
  bool sawRoot = false;
};

void assignBookString(CatalogBook& book, const KeyBuf& key, const std::string_view value) {
  if (key.is("title")) {
    book.title.assign(value);
  } else if (key.is("authors") || key.is("author")) {
    book.authors.assign(value);
  } else if (key.is("series")) {
    book.series.assign(value);
  } else if (key.is("readStatus")) {
    book.readStatus.assign(value);
  } else if (key.is("filename")) {
    book.filename.assign(value);
  }
}

void assignBookNumber(CatalogBook& book, const KeyBuf& key, const char* value, const size_t len) {
  if (key.is("id") || key.is("bookId")) {
    book.bookId = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("fileId")) {
    book.fileId = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("fileBytes") || key.is("bytes") || key.is("size")) {
    book.fileBytes = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("seriesIndex")) {
    book.seriesIndex = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("rating")) {
    const long rating = parseLong(value, len);
    book.rating = rating > 0 && rating <= 5 ? static_cast<uint8_t>(rating) : 0;
  } else if (key.is("progressPercentage")) {
    book.progressPercentage = parseFloat(value, len);
  }
}

void onKey(void* ctx, const char* key, const size_t len) {
  static_cast<BookPageCtx*>(ctx)->key.set(key, len);
}

void onObjectStart(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = CatalogBook{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onObjectEnd(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) {
      s->out->items.push_back(std::move(s->current));
    }
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onArrayStart(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  } else if (s->scope.inItem && s->key.is("formats")) {
    s->inFormats = true;
  }
}

void onArrayEnd(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->inFormats && s->scope.arrayDepth == s->scope.itemsArrayDepth + 1) {
    s->inFormats = false;
  } else if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  const std::string_view text(value, len);
  if (s->inFormats) {
    if (!s->current.formats.empty()) s->current.formats += ", ";
    s->current.formats.append(text);
    return;
  }
  if (s->scope.inItem) {
    assignBookString(s->current, s->key, text);
    return;
  }
  if (s->scope.objectDepth == 1 && s->key.is("query")) {
    s->out->query.assign(text);
  }
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.inItem) {
    assignBookNumber(s->current, s->key, value, len);
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("page")) {
    const long page = parseLong(value, len);
    s->out->page = page > 0 ? static_cast<uint32_t>(page) : 0u;
  } else if (s->key.is("size")) {
    s->out->size = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onBool(void* ctx, const bool value) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.objectDepth == 1 && !s->scope.inItem && s->key.is("hasNext")) {
    s->out->hasNext = value;
  }
}

// Nulls simply leave the field at its default, which is what the catalog means
// by a null rating or an absent series.
void onNull(void* ctx) { static_cast<BookPageCtx*>(ctx)->key.clear(); }

JsonCallbacks makeCallbacks(BookPageCtx& ctx) {
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onKey;
  callbacks.onString = onString;
  callbacks.onNumber = onNumber;
  callbacks.onBool = onBool;
  callbacks.onNull = onNull;
  callbacks.onObjectStart = onObjectStart;
  callbacks.onObjectEnd = onObjectEnd;
  callbacks.onArrayStart = onArrayStart;
  callbacks.onArrayEnd = onArrayEnd;
  return callbacks;
}

bool finish(const BookPageCtx& ctx, const StreamingJsonParser& parser) {
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

}  // namespace

bool decodeBookPage(const std::string_view json, CatalogPage& out) {
  return decodeBookPageChunked(json, json.size(), out);
}

bool decodeBookPageChunked(const std::string_view json, const size_t chunkSize, CatalogPage& out) {
  out = CatalogPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  BookPageCtx ctx;
  ctx.out = &out;
  const JsonCallbacks callbacks = makeCallbacks(ctx);
  StreamingJsonParser parser(callbacks);

  const size_t step = chunkSize > 0 ? chunkSize : json.size();
  for (size_t offset = 0; offset < json.size(); offset += step) {
    const size_t take = json.size() - offset < step ? json.size() - offset : step;
    parser.feed(json.data() + offset, take);
    if (parser.hasError()) return false;
  }
  return finish(ctx, parser);
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R CatalogDecode --output-on-failure
```

Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogDecodeCommon.h lib/BookOrbit/CatalogDecodeCommon.cpp lib/BookOrbit/CatalogTypes.h lib/BookOrbit/CatalogDecode.h lib/BookOrbit/CatalogDecode.cpp test/bookorbit_catalog_decode test/CMakeLists.txt
git commit -m "feat: add streaming BookOrbit catalog book-page decoder"
```

---

### Task 3: Entry-page and dashboard decoders

**Files:**
- Modify: `lib/BookOrbit/CatalogDecode.h`, `lib/BookOrbit/CatalogDecode.cpp`
- Create: `test/bookorbit_catalog_entries/CMakeLists.txt`, `test/bookorbit_catalog_entries/CatalogEntryDecodeTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `KeyBuf`, `DecodeScope`, `parseLong` (Task 2); `StreamingJsonParser`.
- Produces:
  `bool bookorbit::decodeEntryPage(std::string_view json, CatalogEntryPage& out)` — for `/catalog/root`, `/catalog/dashboard/discover`, `/catalog/dashboard/sections/{type}` and `/catalog/sections/{section}`;
  `bool bookorbit::decodeDashboard(std::string_view json, DashboardSummary& out)` — for `/catalog/dashboard`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_entries/CatalogEntryDecodeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/CatalogDecode.h"

using bookorbit::CatalogEntryPage;
using bookorbit::DashboardSummary;
using bookorbit::decodeDashboard;
using bookorbit::decodeEntryPage;

TEST(CatalogEntryDecode, DecodesRootNavigation) {
  const char* json = R"({
    "items": [
      {"id": "libraries", "title": "Libraries", "kind": "libraries", "count": 3},
      {"id": "collections", "title": "Collections", "kind": "collections", "count": 12},
      {"id": "series", "title": "Series", "kind": "series", "count": 41}
    ]
  })";

  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(json, page));
  ASSERT_EQ(page.items.size(), 3u);
  EXPECT_EQ(page.items[0].id, "libraries");
  EXPECT_EQ(page.items[0].title, "Libraries");
  EXPECT_EQ(page.items[0].kind, "libraries");
  EXPECT_EQ(page.items[0].count, 3u);
  EXPECT_EQ(page.items[2].count, 41u);
  EXPECT_FALSE(page.hasNext);
}

TEST(CatalogEntryDecode, DecodesNumericIdsAsStrings) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"items":[{"id":17,"title":"Crime","seriesId":42}]})", page));
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].id, "17");
  EXPECT_EQ(page.items[0].seriesId, "42");
}

TEST(CatalogEntryDecode, CarriesPaginationEnvelope) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"page":3,"hasNext":true,"items":[{"id":"a","title":"A"}]})", page));
  EXPECT_EQ(page.page, 3u);
  EXPECT_TRUE(page.hasNext);
}

TEST(CatalogEntryDecode, EmptySectionIsValid) {
  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(R"({"page":1,"hasNext":false,"items":[]})", page));
  EXPECT_TRUE(page.items.empty());
}

TEST(CatalogEntryDecode, ItemsAreCappedAtMaxPageItems) {
  std::string json = R"({"items":[)";
  for (size_t i = 0; i < bookorbit::kMaxPageItems + 10; ++i) {
    if (i > 0) json += ',';
    json += R"({"id":")" + std::to_string(i) + R"(","title":"t"})";
  }
  json += "]}";

  CatalogEntryPage page;
  ASSERT_TRUE(decodeEntryPage(json, page));
  EXPECT_EQ(page.items.size(), bookorbit::kMaxPageItems);
}

TEST(CatalogEntryDecode, MalformedJsonFails) {
  CatalogEntryPage page;
  EXPECT_FALSE(decodeEntryPage(R"({"items":[{"id":)", page));
}

TEST(CatalogDashboardDecode, DecodesStatsAndContinueReading) {
  const char* json = R"({
    "totalBooks": 412,
    "finishedBooks": 96,
    "continueReading": [
      {"id": 41, "title": "We Solve Murders", "authors": "Richard Osman",
       "progressPercentage": 37.5, "fileId": 902},
      {"id": 55, "title": "Piranesi", "authors": "Susanna Clarke",
       "progressPercentage": 12.0, "fileId": 913}
    ],
    "sections": [
      {"id": "want-to-read", "title": "Want to read", "count": 8},
      {"id": "up-next-in-series", "title": "Up next in series", "count": 5}
    ]
  })";

  DashboardSummary dashboard;
  ASSERT_TRUE(decodeDashboard(json, dashboard));
  EXPECT_EQ(dashboard.totalBooks, 412u);
  EXPECT_EQ(dashboard.finishedBooks, 96u);
  ASSERT_EQ(dashboard.continueReading.size(), 2u);
  EXPECT_EQ(dashboard.continueReading[0].bookId, 41u);
  EXPECT_EQ(dashboard.continueReading[0].title, "We Solve Murders");
  EXPECT_FLOAT_EQ(dashboard.continueReading[1].progressPercentage, 12.0f);
  ASSERT_EQ(dashboard.sections.size(), 2u);
  EXPECT_EQ(dashboard.sections[1].id, "up-next-in-series");
  EXPECT_EQ(dashboard.sections[1].count, 5u);
}

TEST(CatalogDashboardDecode, AbsentSectionsIsValid) {
  DashboardSummary dashboard;
  ASSERT_TRUE(decodeDashboard(R"({"totalBooks":0,"continueReading":[]})", dashboard));
  EXPECT_TRUE(dashboard.continueReading.empty());
  EXPECT_TRUE(dashboard.sections.empty());
  EXPECT_EQ(dashboard.totalBooks, 0u);
}

TEST(CatalogDashboardDecode, MalformedJsonFails) {
  DashboardSummary dashboard;
  EXPECT_FALSE(decodeDashboard(R"({"continueReading":)", dashboard));
}
```

Create `test/bookorbit_catalog_entries/CMakeLists.txt`:

```cmake
add_executable(CatalogEntryDecodeTest
  CatalogEntryDecodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogEntryDecodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogEntryDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogEntryDecodeTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_entries)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'decodeEntryPage' is not a member of 'bookorbit'`.

- [ ] **Step 3: Write minimal implementation**

Append to `lib/BookOrbit/CatalogDecode.h`, inside `namespace bookorbit`:

```cpp
// Decodes a navigation page: /catalog/root, /catalog/dashboard/discover,
// /catalog/dashboard/sections/{type} and /catalog/sections/{section} all share
// the {items:[{id,title,kind,count}], page, hasNext} envelope. Numeric ids are
// kept as strings so a library id and a slug travel the same path.
// Resets `out`. Returns false on malformed JSON.
bool decodeEntryPage(std::string_view json, CatalogEntryPage& out);

// Decodes /catalog/dashboard: counters plus the Continue-reading book list and
// the configured shelf list. Streamed like every other catalog body.
bool decodeDashboard(std::string_view json, DashboardSummary& out);
```

Append to `lib/BookOrbit/CatalogDecode.cpp`, inside the anonymous namespace:

```cpp
struct EntryPageCtx {
  CatalogEntryPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogEntry current;
  bool sawRoot = false;
};

void onEntryKey(void* ctx, const char* key, const size_t len) {
  static_cast<EntryPageCtx*>(ctx)->key.set(key, len);
}

void onEntryObjectStart(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = CatalogEntry{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onEntryObjectEnd(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) s->out->items.push_back(std::move(s->current));
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onEntryArrayStart(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  }
}

void onEntryArrayEnd(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void assignEntryString(CatalogEntry& entry, const KeyBuf& key, const std::string_view value) {
  if (key.is("id")) {
    entry.id.assign(value);
  } else if (key.is("title") || key.is("text") || key.is("name")) {
    entry.title.assign(value);
  } else if (key.is("kind") || key.is("type")) {
    entry.kind.assign(value);
  } else if (key.is("seriesId")) {
    entry.seriesId.assign(value);
  }
}

void onEntryString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem) assignEntryString(s->current, s->key, std::string_view(value, len));
}

void onEntryNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem) {
    // A numeric id or seriesId is stored verbatim: the query builder encodes it
    // as a string either way, so keeping one representation avoids a lossy
    // round-trip through long.
    if (s->key.is("id")) {
      s->current.id.assign(value, len);
    } else if (s->key.is("seriesId")) {
      s->current.seriesId.assign(value, len);
    } else if (s->key.is("count")) {
      s->current.count = static_cast<uint32_t>(parseLong(value, len));
    }
    return;
  }
  if (s->scope.objectDepth == 1 && s->key.is("page")) {
    const long page = parseLong(value, len);
    s->out->page = page > 0 ? static_cast<uint32_t>(page) : 0u;
  }
}

void onEntryBool(void* ctx, const bool value) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.objectDepth == 1 && !s->scope.inItem && s->key.is("hasNext")) s->out->hasNext = value;
}

void onEntryNull(void* ctx) { static_cast<EntryPageCtx*>(ctx)->key.clear(); }

struct DashboardCtx {
  DashboardSummary* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogBook currentBook;
  CatalogEntry currentEntry;
  bool inBooks = false;
  bool inSections = false;
  bool sawRoot = false;
};

void onDashKey(void* ctx, const char* key, const size_t len) {
  static_cast<DashboardCtx*>(ctx)->key.set(key, len);
}

void onDashObjectStart(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.objectDepth == 2) {
    if (s->inBooks) {
      s->currentBook = CatalogBook{};
      s->scope.inItem = true;
    } else if (s->inSections) {
      s->currentEntry = CatalogEntry{};
      s->scope.inItem = true;
    }
  }
  s->key.clear();
}

void onDashObjectEnd(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->inBooks && s->out->continueReading.size() < kMaxPageItems) {
      s->out->continueReading.push_back(std::move(s->currentBook));
    } else if (s->inSections && s->out->sections.size() < kMaxPageItems) {
      s->out->sections.push_back(std::move(s->currentEntry));
    }
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onDashArrayStart(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  s->scope.arrayDepth++;
  if (s->scope.objectDepth == 1 && s->scope.arrayDepth == 1) {
    if (s->key.is("continueReading") || s->key.is("items")) {
      s->inBooks = true;
      s->scope.itemsArrayDepth = s->scope.arrayDepth;
    } else if (s->key.is("sections") || s->key.is("shelves")) {
      s->inSections = true;
      s->scope.itemsArrayDepth = s->scope.arrayDepth;
    }
  }
}

void onDashArrayEnd(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->inBooks = false;
    s->inSections = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onDashString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (!s->scope.inItem) return;
  const std::string_view text(value, len);
  if (s->inBooks) {
    assignBookString(s->currentBook, s->key, text);
  } else if (s->inSections) {
    assignEntryString(s->currentEntry, s->key, text);
  }
}

void onDashNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.inItem) {
    if (s->inBooks) {
      assignBookNumber(s->currentBook, s->key, value, len);
    } else if (s->inSections) {
      if (s->key.is("id")) {
        s->currentEntry.id.assign(value, len);
      } else if (s->key.is("count")) {
        s->currentEntry.count = static_cast<uint32_t>(parseLong(value, len));
      }
    }
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("totalBooks")) {
    s->out->totalBooks = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("finishedBooks")) {
    s->out->finishedBooks = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onDashBool(void*, bool) {}

void onDashNull(void* ctx) { static_cast<DashboardCtx*>(ctx)->key.clear(); }
```

Append to `lib/BookOrbit/CatalogDecode.cpp`, outside the anonymous namespace:

```cpp
bool decodeEntryPage(const std::string_view json, CatalogEntryPage& out) {
  out = CatalogEntryPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  EntryPageCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onEntryKey;
  callbacks.onString = onEntryString;
  callbacks.onNumber = onEntryNumber;
  callbacks.onBool = onEntryBool;
  callbacks.onNull = onEntryNull;
  callbacks.onObjectStart = onEntryObjectStart;
  callbacks.onObjectEnd = onEntryObjectEnd;
  callbacks.onArrayStart = onEntryArrayStart;
  callbacks.onArrayEnd = onEntryArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

bool decodeDashboard(const std::string_view json, DashboardSummary& out) {
  out = DashboardSummary{};
  if (json.empty()) return false;
  out.continueReading.reserve(8);
  out.sections.reserve(8);

  DashboardCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onDashKey;
  callbacks.onString = onDashString;
  callbacks.onNumber = onDashNumber;
  callbacks.onBool = onDashBool;
  callbacks.onNull = onDashNull;
  callbacks.onObjectStart = onDashObjectStart;
  callbacks.onObjectEnd = onDashObjectEnd;
  callbacks.onArrayStart = onDashArrayStart;
  callbacks.onArrayEnd = onDashArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "CatalogEntryDecode|CatalogDashboardDecode" --output-on-failure
```

Expected: 9 tests PASS. Re-run `-R CatalogDecode` to confirm Task 2's 11 tests still pass.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogDecode.h lib/BookOrbit/CatalogDecode.cpp test/bookorbit_catalog_entries test/CMakeLists.txt
git commit -m "feat: add BookOrbit catalog entry and dashboard decoders"
```

---

### Task 4: Catalog API and the dashboard-sections capability gate

**Files:**
- Create: `lib/BookOrbit/CatalogApi.h`, `lib/BookOrbit/CatalogApi.cpp`
- Create: `test/bookorbit_catalog_api/CMakeLists.txt`, `test/bookorbit_catalog_api/CatalogApiTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::BookOrbitClient`, `bookorbit::IHttpTransport`, `bookorbit::HttpRequest`, `bookorbit::HttpResponse`, `bookorbit::Error`, `bookorbit::Status`, `bookorbit::classify` (P0 Tasks 2, 6); `bookorbit::CapabilityCache`, `bookorbit::Capability` (P0 Task 3); `CatalogQuery` (Task 1); `decodeBookPage`, `decodeEntryPage`, `decodeDashboard` (Tasks 2–3).
- Produces:
  `constexpr char bookorbit::kCapDashboardSections[] = "catalogDashboardSections"`;
  `class bookorbit::CatalogApi` with
  `CatalogApi(BookOrbitClient& client, CapabilityCache& capabilities)`,
  `Error root(CatalogEntryPage& out)`,
  `Error dashboard(DashboardSummary& out)`,
  `Error discover(CatalogEntryPage& out)`,
  `Error dashboardSection(std::string_view type, CatalogEntryPage& out)`,
  `Error section(std::string_view section, const CatalogQuery& query, CatalogEntryPage& out)`,
  `Error books(const CatalogQuery& query, CatalogPage& out)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_api/CatalogApiTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpResponse> queued;
  std::vector<bookorbit::HttpRequest> sent;
  size_t index = 0;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (index >= queued.size()) return {500, false, ""};
    return queued[index++];
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

struct Fixture {
  ScriptedTransport transport;
  bookorbit::CapabilityCache capabilities;
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "u", "k", identity()};
  bookorbit::CatalogApi api{client, capabilities};
};

}  // namespace

using bookorbit::Capability;
using bookorbit::CatalogEntryPage;
using bookorbit::CatalogPage;
using bookorbit::CatalogQuery;
using bookorbit::DashboardSummary;
using bookorbit::kCapDashboardSections;
using bookorbit::Status;

TEST(CatalogApi, RootHitsTheRootRoute) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[{"id":"libraries","title":"Libraries"}]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.root(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].method, "GET");
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/root");
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].title, "Libraries");
}

TEST(CatalogApi, DashboardAndDiscoverUseTheirOwnRoutes) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"totalBooks":9,"continueReading":[]})"});
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  DashboardSummary dashboard;
  ASSERT_EQ(f.api.dashboard(dashboard).status, Status::Ok);
  EXPECT_EQ(dashboard.totalBooks, 9u);
  EXPECT_EQ(f.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard");

  CatalogEntryPage discover;
  ASSERT_EQ(f.api.discover(discover).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard/discover");
}

TEST(CatalogApi, SectionPathSegmentIsUrlEncoded) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  CatalogQuery query;
  query.set("page", 2);
  query.set("size", 20);
  CatalogEntryPage page;
  ASSERT_EQ(f.api.section("up next", query, page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/sections/up%20next?page=2&size=20");
}

TEST(CatalogApi, BooksCarriesTheSortedFilterQuery) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"page":1,"items":[{"id":41,"title":"T"}]})"});

  CatalogQuery query;
  query.set("sort", "recently_added");
  query.set("libraryId", 3);
  query.set("page", 1);
  query.set("size", 20);
  CatalogPage page;
  ASSERT_EQ(f.api.books(query, page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/books"
            "?libraryId=3&page=1&size=20&sort=recently_added");
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);
}

TEST(CatalogApi, DashboardSectionHitsTheEncodedTypeRoute) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({200, false, R"({"items":[{"id":"a","title":"A"}]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.dashboardSection("up-next-in-series", page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/dashboard/sections/up-next-in-series");
}

// The gate: a server that never advertised the capability is not asked at all.
TEST(CatalogApi, DashboardSectionIsSkippedWhenCapabilityIsUnsupported) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"bookmarkSync"});
  ASSERT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unsupported);

  CatalogEntryPage page;
  const auto error = f.api.dashboardSection("want-to-read", page);
  EXPECT_EQ(error.status, Status::NotFound);
  EXPECT_TRUE(f.transport.sent.empty());
}

// Unknown means "we do not know yet", so the request is attempted.
TEST(CatalogApi, DashboardSectionIsAttemptedWhenCapabilityIsUnknown) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[]})"});

  CatalogEntryPage page;
  ASSERT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent.size(), 1u);
}

// A confirmed 404 on the feature's own route downgrades it immediately.
TEST(CatalogApi, NotFoundDowngradesTheCapability) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({404, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::NotFound);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unsupported);
}

// The rule that matters most: a 5xx must leave the capability Unknown, never
// cache a negative. One blip must not permanently disable the feature.
TEST(CatalogApi, ServerErrorLeavesTheCapabilityUnknown) {
  Fixture f;
  f.transport.queued.push_back({503, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::ServerError);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unknown);
}

TEST(CatalogApi, TransportFailureLeavesTheCapabilityUnknown) {
  Fixture f;
  f.transport.queued.push_back({0, true, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.dashboardSection("want-to-read", page).status, Status::Transport);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Unknown);
}

// A 5xx must not downgrade a capability the server previously advertised.
TEST(CatalogApi, ServerErrorDoesNotEraseAPreviouslyAdvertisedCapability) {
  Fixture f;
  f.capabilities.rememberFromVersionResponse({"catalogDashboardSections"});
  f.transport.queued.push_back({500, false, ""});

  CatalogEntryPage page;
  f.api.dashboardSection("want-to-read", page);
  EXPECT_EQ(f.capabilities.get(kCapDashboardSections), Capability::Supported);
}

TEST(CatalogApi, UndecodableBodyIsReportedAsInvalidJson) {
  Fixture f;
  f.transport.queued.push_back({200, false, R"({"items":[{"id":)"});

  CatalogPage page;
  EXPECT_EQ(f.api.books(CatalogQuery{}, page).status, Status::InvalidJson);
}

TEST(CatalogApi, AuthErrorIsPassedThroughUnchanged) {
  Fixture f;
  f.transport.queued.push_back({401, false, ""});

  CatalogEntryPage page;
  EXPECT_EQ(f.api.root(page).status, Status::Unauthorized);
}
```

Create `test/bookorbit_catalog_api/CMakeLists.txt`:

```cmake
add_executable(CatalogApiTest
  CatalogApiTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogApiTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogApiTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogApiTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_api)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogApi.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogApi.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "BookOrbitError.h"
#include "CatalogQuery.h"
#include "CatalogTypes.h"

namespace bookorbit {

// The one capability P5 gates on. Servers older than the dashboard-sections
// feature answer its route with 404.
constexpr char kCapDashboardSections[] = "catalogDashboardSections";

// Read side of the BookOrbit catalog. Every method issues one GET through the
// P0 client and decodes the body with the streaming decoders, so no response
// is ever held whole in memory.
class CatalogApi {
 public:
  CatalogApi(BookOrbitClient& client, CapabilityCache& capabilities)
      : client_(client), capabilities_(capabilities) {}

  Error root(CatalogEntryPage& out);
  Error dashboard(DashboardSummary& out);
  Error discover(CatalogEntryPage& out);

  // Capability-gated on kCapDashboardSections. Returns {NotFound, 404} without
  // issuing a request when the capability is known-Unsupported. A 404 from the
  // route downgrades the capability; a 5xx or transport failure leaves it
  // Unknown, because a blip carries no information about server support.
  Error dashboardSection(std::string_view type, CatalogEntryPage& out);

  Error section(std::string_view section, const CatalogQuery& query, CatalogEntryPage& out);
  Error books(const CatalogQuery& query, CatalogPage& out);

  // Exposed so CatalogManifest (Task 5) can reuse the same request plumbing.
  Error getBody(std::string_view path, std::string& outBody);

 private:
  BookOrbitClient& client_;
  CapabilityCache& capabilities_;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogApi.cpp`:

```cpp
#include "CatalogApi.h"

#include "CatalogDecode.h"

namespace bookorbit {
namespace {

constexpr char kCatalogBase[] = "/koreader/plugin/catalog";

std::string catalogPath(const std::string_view suffix) {
  std::string path(kCatalogBase);
  path.append(suffix);
  return path;
}

}  // namespace

Error CatalogApi::getBody(const std::string_view path, std::string& outBody) {
  return client_.get(path, outBody);
}

Error CatalogApi::root(CatalogEntryPage& out) {
  std::string body;
  const Error error = getBody(catalogPath("/root"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::dashboard(DashboardSummary& out) {
  std::string body;
  const Error error = getBody(catalogPath("/dashboard"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeDashboard(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::discover(CatalogEntryPage& out) {
  std::string body;
  const Error error = getBody(catalogPath("/dashboard/discover"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::dashboardSection(const std::string_view type, CatalogEntryPage& out) {
  if (capabilities_.get(kCapDashboardSections) == Capability::Unsupported) {
    // Known absent: skip the round trip entirely rather than collect a 404 on
    // every dashboard build.
    return {Status::NotFound, 404};
  }

  std::string path = catalogPath("/dashboard/sections/");
  path += urlEncodeComponent(type);

  std::string body;
  const Error error = getBody(path, body);
  if (error.status == Status::NotFound) {
    // A definitive 404 on the feature's own route is a confirmed downgrade.
    capabilities_.markUnsupported(kCapDashboardSections);
    return error;
  }
  // Any 5xx or transport failure falls through untouched: the tri-state stays
  // Unknown (or keeps whatever /version said), never caching a negative.
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::section(const std::string_view section, const CatalogQuery& query, CatalogEntryPage& out) {
  std::string path = catalogPath("/sections/");
  path += urlEncodeComponent(section);

  std::string body;
  const Error error = getBody(query.build(path), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::books(const CatalogQuery& query, CatalogPage& out) {
  std::string body;
  const Error error = getBody(query.build(catalogPath("/books")), body);
  if (error.status != Status::Ok) return error;
  if (!decodeBookPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R CatalogApi --output-on-failure
```

Expected: 13 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogApi.h lib/BookOrbit/CatalogApi.cpp test/bookorbit_catalog_api test/CMakeLists.txt
git commit -m "feat: add BookOrbit catalog API with dashboard-sections capability gate"
```

---

### Task 5: Manifest cursor pagination and the restart rule

**Files:**
- Create: `lib/BookOrbit/CatalogManifest.h`, `lib/BookOrbit/CatalogManifest.cpp`
- Create: `test/bookorbit_catalog_manifest/CMakeLists.txt`, `test/bookorbit_catalog_manifest/CatalogManifestTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `CatalogApi::getBody` (Task 4); `CatalogQuery` (Task 1); `KeyBuf`, `DecodeScope`, `parseLong` (Task 2); `Error`, `Status`, `CapabilityCache`.
- Produces:
  `struct bookorbit::ManifestItem { uint32_t bookId, fileId, fileBytes; std::string hash, title, filename, format; }`;
  `struct bookorbit::ManifestPage { std::vector<ManifestItem> items; bool hasNext; bool restartRequired; std::string nextCursor, manifestVersion; }`;
  `bool bookorbit::decodeManifestPage(std::string_view json, ManifestPage& out)`;
  `class bookorbit::ManifestEnumerator` with
  `ManifestEnumerator(CatalogApi& api, std::string deviceId)`,
  `Error next(ManifestPage& out)`,
  `bool hasNext() const`,
  `uint32_t restartCount() const`,
  `const std::string& manifestVersion() const`,
  `void reset()`;
  `constexpr uint32_t bookorbit::kMaxManifestRestarts = 3`.

Cursor contract from `bookorbit_catalog_bulk_download.lua:780-810`: the page carries `{hasNext, nextCursor}`; `restartRequired: true` means the server rejected the cursor, so the enumerator drops it and re-enumerates from the beginning. Restarting is safe — books already on device are not transferred again — but it is bounded so a server that always rejects cannot loop forever.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_manifest/CatalogManifestTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/CatalogManifest.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpResponse> queued;
  std::vector<bookorbit::HttpRequest> sent;
  size_t index = 0;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (index >= queued.size()) return {500, false, ""};
    return queued[index++];
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

struct Fixture {
  ScriptedTransport transport;
  bookorbit::CapabilityCache capabilities;
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "u", "k", identity()};
  bookorbit::CatalogApi api{client, capabilities};
  bookorbit::ManifestEnumerator enumerator{api, "crossink-abc123"};
};

const char* kPageOne = R"({
  "manifestVersion": "lv-77",
  "hasNext": true,
  "nextCursor": "eyJpZCI6NDF9",
  "items": [
    {"bookId": 41, "fileId": 902, "hash": "0f0a792b00a37cf80baa5e50c078b31f",
     "title": "We Solve Murders", "filename": "we-solve-murders.epub",
     "format": "epub", "fileBytes": 1258291}
  ]
})";

const char* kPageTwo = R"({
  "manifestVersion": "lv-77",
  "hasNext": false,
  "items": [
    {"bookId": 42, "fileId": 903, "hash": "a1b2c3d4e5f60718293a4b5c6d7e8f90",
     "title": "Piranesi", "filename": "piranesi.epub", "format": "epub", "fileBytes": 654321}
  ]
})";

}  // namespace

using bookorbit::decodeManifestPage;
using bookorbit::ManifestPage;
using bookorbit::Status;

TEST(ManifestDecode, DecodesItemsAndCursor) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(kPageOne, page));
  EXPECT_TRUE(page.hasNext);
  EXPECT_EQ(page.nextCursor, "eyJpZCI6NDF9");
  EXPECT_EQ(page.manifestVersion, "lv-77");
  EXPECT_FALSE(page.restartRequired);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);
  EXPECT_EQ(page.items[0].fileId, 902u);
  EXPECT_EQ(page.items[0].hash, "0f0a792b00a37cf80baa5e50c078b31f");
  EXPECT_EQ(page.items[0].filename, "we-solve-murders.epub");
  EXPECT_EQ(page.items[0].fileBytes, 1258291u);
}

TEST(ManifestDecode, DecodesRestartRequiredFlag) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(R"({"restartRequired":true,"manifestVersion":"lv-78"})", page));
  EXPECT_TRUE(page.restartRequired);
  EXPECT_EQ(page.manifestVersion, "lv-78");
}

// hasNext without a cursor is not a next page: the walk must stop.
TEST(ManifestDecode, HasNextWithoutCursorIsNotANextPage) {
  ManifestPage page;
  ASSERT_TRUE(decodeManifestPage(R"({"hasNext":true,"items":[]})", page));
  EXPECT_TRUE(page.hasNext);
  EXPECT_TRUE(page.nextCursor.empty());
}

TEST(ManifestDecode, MalformedJsonFails) {
  ManifestPage page;
  EXPECT_FALSE(decodeManifestPage(R"({"items":[{"bookId":)", page));
}

TEST(ManifestEnumerator, FirstRequestSendsDeviceIdAndNoCursor) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?deviceId=crossink-abc123&size=20");
}

TEST(ManifestEnumerator, SecondRequestCarriesTheCursor) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, kPageTwo});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_TRUE(f.enumerator.hasNext());
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?cursor=eyJpZCI6NDF9&deviceId=crossink-abc123&size=20");
  EXPECT_EQ(page.items[0].bookId, 42u);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, TracksTheManifestVersion) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.manifestVersion(), "lv-77");
}

// The cursor-restart path. A rejected cursor drops the cursor and re-enumerates
// from scratch within the same next() call, so the caller sees page one again
// rather than an error.
TEST(ManifestEnumerator, RejectedCursorRestartsEnumerationFromScratch) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-78"})"});
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_TRUE(f.enumerator.hasNext());

  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.restartCount(), 1u);
  ASSERT_EQ(page.items.size(), 1u);
  EXPECT_EQ(page.items[0].bookId, 41u);

  ASSERT_EQ(f.transport.sent.size(), 3u);
  // The retry after the rejection carries no cursor at all.
  EXPECT_EQ(f.transport.sent[2].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/manifest"
            "?deviceId=crossink-abc123&size=20");
}

TEST(ManifestEnumerator, RestartAdoptsTheNewManifestVersion) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-78"})"});
  f.transport.queued.push_back({200, false, R"({"manifestVersion":"lv-78","hasNext":false,"items":[]})"});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.enumerator.manifestVersion(), "lv-78");
}

// A server that rejects every cursor must not spin forever on a battery device.
TEST(ManifestEnumerator, RestartsAreBounded) {
  Fixture f;
  for (int i = 0; i < 8; ++i) {
    f.transport.queued.push_back({200, false, R"({"restartRequired":true,"manifestVersion":"lv-79"})"});
  }

  ManifestPage page;
  const auto error = f.enumerator.next(page);
  EXPECT_EQ(error.status, Status::ClientError);
  EXPECT_EQ(f.enumerator.restartCount(), bookorbit::kMaxManifestRestarts);
  EXPECT_LE(f.transport.sent.size(), bookorbit::kMaxManifestRestarts + 1u);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, NotFoundDowngradesTheManifestCapability) {
  Fixture f;
  f.transport.queued.push_back({404, false, ""});

  ManifestPage page;
  EXPECT_EQ(f.enumerator.next(page).status, Status::NotFound);
  EXPECT_FALSE(f.enumerator.hasNext());
}

TEST(ManifestEnumerator, ResetClearsCursorAndRestartCount) {
  Fixture f;
  f.transport.queued.push_back({200, false, kPageOne});
  f.transport.queued.push_back({200, false, kPageOne});

  ManifestPage page;
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  f.enumerator.reset();
  EXPECT_EQ(f.enumerator.restartCount(), 0u);
  ASSERT_EQ(f.enumerator.next(page).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[1].url, f.transport.sent[0].url);
}

TEST(ManifestEnumerator, TransportFailureStopsTheWalkWithoutRestarting) {
  Fixture f;
  f.transport.queued.push_back({0, true, ""});

  ManifestPage page;
  EXPECT_EQ(f.enumerator.next(page).status, Status::Transport);
  EXPECT_EQ(f.enumerator.restartCount(), 0u);
}
```

Create `test/bookorbit_catalog_manifest/CMakeLists.txt`:

```cmake
add_executable(CatalogManifestTest
  CatalogManifestTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogManifest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogManifestTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogManifestTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogManifestTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_manifest)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogManifest.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogManifest.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitError.h"
#include "CatalogApi.h"

namespace bookorbit {

// A server that keeps rejecting the cursor must not spin a battery device.
// Three restarts is generous for a library edited mid-run and still bounded.
constexpr uint32_t kMaxManifestRestarts = 3;

struct ManifestItem {
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  uint32_t fileBytes = 0;
  std::string hash;  // server-side partial MD5; matches what P0 match-check uses
  std::string title;
  std::string filename;
  std::string format;
};

struct ManifestPage {
  std::vector<ManifestItem> items;
  bool hasNext = false;
  bool restartRequired = false;
  std::string nextCursor;
  std::string manifestVersion;
};

// Streaming decode of one /catalog/manifest page. Resets `out`.
bool decodeManifestPage(std::string_view json, ManifestPage& out);

// Cursor-paginated walk over the bulk manifest. Only one page is ever held:
// the enumerator keeps a cursor string, not an accumulated list.
class ManifestEnumerator {
 public:
  ManifestEnumerator(CatalogApi& api, std::string deviceId)
      : api_(api), deviceId_(std::move(deviceId)) {}

  // Fetches the next page. When the server answers restartRequired the cursor
  // is dropped and enumeration restarts from the beginning inside this same
  // call — safe, because books already on device are never re-transferred.
  // Bounded by kMaxManifestRestarts, after which {ClientError, 409} is
  // returned and the walk stops.
  Error next(ManifestPage& out);

  bool hasNext() const { return hasNext_; }
  uint32_t restartCount() const { return restarts_; }
  const std::string& manifestVersion() const { return manifestVersion_; }

  void reset();

 private:
  Error fetch(ManifestPage& out);

  CatalogApi& api_;
  std::string deviceId_;
  std::string cursor_;
  std::string manifestVersion_;
  bool hasNext_ = true;   // true until a page says otherwise
  bool started_ = false;
  uint32_t restarts_ = 0;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogManifest.cpp`:

```cpp
#include "CatalogManifest.h"

#include "CatalogDecodeCommon.h"
#include "CatalogQuery.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

constexpr char kManifestPath[] = "/koreader/plugin/catalog/manifest";

struct ManifestCtx {
  ManifestPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  ManifestItem current;
  bool sawRoot = false;
};

void onKey(void* ctx, const char* key, const size_t len) {
  static_cast<ManifestCtx*>(ctx)->key.set(key, len);
}

void onObjectStart(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = ManifestItem{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onObjectEnd(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) s->out->items.push_back(std::move(s->current));
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onArrayStart(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  }
}

void onArrayEnd(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  const std::string_view text(value, len);
  if (s->scope.inItem) {
    if (s->key.is("hash") || s->key.is("partialMd5")) {
      s->current.hash.assign(text);
    } else if (s->key.is("title")) {
      s->current.title.assign(text);
    } else if (s->key.is("filename") || s->key.is("devicePath")) {
      s->current.filename.assign(text);
    } else if (s->key.is("format") || s->key.is("filetype")) {
      s->current.format.assign(text);
    }
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("nextCursor")) {
    s->out->nextCursor.assign(text);
  } else if (s->key.is("manifestVersion") || s->key.is("libraryVersion")) {
    s->out->manifestVersion.assign(text);
  }
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (!s->scope.inItem) return;
  if (s->key.is("bookId") || s->key.is("id")) {
    s->current.bookId = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("fileId")) {
    s->current.fileId = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("fileBytes") || s->key.is("bytes") || s->key.is("size")) {
    s->current.fileBytes = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onBool(void* ctx, const bool value) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.objectDepth != 1 || s->scope.inItem) return;
  if (s->key.is("hasNext")) {
    s->out->hasNext = value;
  } else if (s->key.is("restartRequired")) {
    s->out->restartRequired = value;
  }
}

void onNull(void* ctx) { static_cast<ManifestCtx*>(ctx)->key.clear(); }

}  // namespace

bool decodeManifestPage(const std::string_view json, ManifestPage& out) {
  out = ManifestPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  ManifestCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onKey;
  callbacks.onString = onString;
  callbacks.onNumber = onNumber;
  callbacks.onBool = onBool;
  callbacks.onNull = onNull;
  callbacks.onObjectStart = onObjectStart;
  callbacks.onObjectEnd = onObjectEnd;
  callbacks.onArrayStart = onArrayStart;
  callbacks.onArrayEnd = onArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

void ManifestEnumerator::reset() {
  cursor_.clear();
  hasNext_ = true;
  started_ = false;
  restarts_ = 0;
}

Error ManifestEnumerator::fetch(ManifestPage& out) {
  CatalogQuery query;
  query.set("deviceId", deviceId_);
  query.set("size", static_cast<long>(kCatalogPageSize));
  query.set("cursor", cursor_);  // empty cursor is dropped by CatalogQuery

  std::string body;
  const Error error = api_.getBody(query.build(kManifestPath), body);
  if (error.status != Status::Ok) return error;
  if (!decodeManifestPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error ManifestEnumerator::next(ManifestPage& out) {
  if (!hasNext_ && started_) {
    out = ManifestPage{};
    return {Status::Ok, 200};
  }

  for (;;) {
    const Error error = fetch(out);
    if (error.status != Status::Ok) {
      // Any failure stops the walk. There are no retry loops in this client:
      // the next sync trigger starts over.
      hasNext_ = false;
      return error;
    }

    if (!out.manifestVersion.empty()) manifestVersion_ = out.manifestVersion;

    if (out.restartRequired) {
      if (restarts_ >= kMaxManifestRestarts) {
        hasNext_ = false;
        return {Status::ClientError, 409};
      }
      restarts_++;
      // Drop the cursor and enumerate from the beginning. Books already on
      // device are not transferred again, so a restart costs listing work only.
      cursor_.clear();
      started_ = false;
      continue;
    }

    started_ = true;
    hasNext_ = out.hasNext && !out.nextCursor.empty();
    cursor_ = hasNext_ ? out.nextCursor : std::string();
    return error;
  }
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "ManifestDecode|ManifestEnumerator" --output-on-failure
```

Expected: 13 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogManifest.h lib/BookOrbit/CatalogManifest.cpp test/bookorbit_catalog_manifest test/CMakeLists.txt
git commit -m "feat: add BookOrbit manifest cursor enumerator with restart handling"
```

---

### Task 6: Streaming `.part` download writer with byte cap and atomic publish

**Files:**
- Create: `lib/BookOrbit/IFileSink.h`, `lib/BookOrbit/CatalogDownload.h`, `lib/BookOrbit/CatalogDownload.cpp`
- Create: `test/bookorbit_catalog_download/CMakeLists.txt`, `test/bookorbit_catalog_download/CatalogDownloadTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from P0 beyond the coding rules.
- Produces:
  `class bookorbit::IFileSink` with pure virtuals
  `bool open(std::string_view path)`, `bool write(const uint8_t* data, size_t len)`,
  `bool close()`, `bool publish(std::string_view from, std::string_view to)`,
  `bool remove(std::string_view path)`, `bool exists(std::string_view path)`;
  `size_t bookorbit::maxBytesForExpected(size_t expectedBytes)`;
  `std::string bookorbit::partPathFor(std::string_view finalPath)`;
  `class bookorbit::PartFileWriter` with
  `PartFileWriter(IFileSink& sink, std::string finalPath, size_t maxBytes)`,
  `bool begin()`, `bool onData(const uint8_t* data, size_t len)`,
  `bool commit()`, `void abandon()`,
  `bool capExceeded() const`, `size_t bytes() const`, `const std::string& partPath() const`;
  `constexpr size_t bookorbit::kMaxTransferBytes = 512u * 1024u * 1024u`.

`PartFileWriter` owns **no** buffer. Bytes arrive from `SecureHttpClient`'s `DataCallback` in the transport's own 2 KB chunk and go straight to `IFileSink::write`. Nothing about a download scales with file size in RAM, which is what makes a 40 MB book safe on a C3.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_download/CatalogDownloadTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

// In-memory IFileSink with fault injection, so publish-safety can be tested
// without a filesystem.
class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  bool failOpen = false;
  bool failWrite = false;
  bool failPublish = false;
  int closes = 0;

  bool open(const std::string_view path) override {
    if (failOpen) return false;
    openPath.assign(path);
    files[openPath] = "";  // truncates any leftover of the same name
    isOpen = true;
    return true;
  }

  bool write(const uint8_t* data, const size_t len) override {
    if (!isOpen || failWrite) return false;
    files[openPath].append(reinterpret_cast<const char*>(data), len);
    return true;
  }

  bool close() override {
    closes++;
    isOpen = false;
    return true;
  }

  bool publish(const std::string_view from, const std::string_view to) override {
    if (failPublish) return false;
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }

  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }

  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

bool feed(bookorbit::PartFileWriter& writer, const std::string& payload) {
  return writer.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

}  // namespace

using bookorbit::kMaxTransferBytes;
using bookorbit::maxBytesForExpected;
using bookorbit::PartFileWriter;
using bookorbit::partPathFor;

TEST(PartPath, AppendsPartSuffix) {
  EXPECT_EQ(partPathFor("/books/we-solve-murders.epub"), "/books/we-solve-murders.epub.part");
}

TEST(MaxBytesForExpected, UnknownSizeUsesTheHardCeiling) {
  EXPECT_EQ(maxBytesForExpected(0), kMaxTransferBytes);
}

TEST(MaxBytesForExpected, AddsMarginAndSlackToAKnownSize) {
  // 1,000,000 * 1.25 = 1,250,000, plus 1 MiB of slack for container overhead.
  EXPECT_EQ(maxBytesForExpected(1000000), 1250000u + 1024u * 1024u);
}

TEST(MaxBytesForExpected, NeverExceedsTheHardCeiling) {
  EXPECT_EQ(maxBytesForExpected(kMaxTransferBytes), kMaxTransferBytes);
}

TEST(PartFileWriter, WritesToThePartPathNotTheFinalPath) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "PK\x03\x04payload"));

  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_EQ(writer.bytes(), 12u);
}

TEST(PartFileWriter, CommitPublishesAtomicallyAndRemovesThePart) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "hello"));
  ASSERT_TRUE(writer.commit());

  EXPECT_TRUE(sink.exists("/books/a.epub"));
  EXPECT_FALSE(sink.exists("/books/a.epub.part"));
  EXPECT_EQ(sink.files["/books/a.epub"], "hello");
  EXPECT_EQ(sink.closes, 1);
}

// The interrupted-download guarantee: whatever happened, a half-transferred
// book is only ever visible as a ".part" file. A .part must never be mistaken
// for a complete book.
TEST(PartFileWriter, AbandonLeavesOnlyThePartFile) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "half a book"));
  writer.abandon();

  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_EQ(sink.closes, 1);
}

TEST(PartFileWriter, AbandonedTransferCannotBeCommittedAfterwards) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "half a book"));
  writer.abandon();

  EXPECT_FALSE(writer.commit());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

// A leftover .part from a previous power loss is truncated, never appended to.
TEST(PartFileWriter, StalePartFromAPreviousRunIsDiscarded) {
  FakeFileSink sink;
  sink.files["/books/a.epub.part"] = "garbage from a battery pull";

  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "fresh"));
  ASSERT_TRUE(writer.commit());
  EXPECT_EQ(sink.files["/books/a.epub"], "fresh");
}

TEST(PartFileWriter, ExceedingTheByteCapAbortsAndPublishesNothing) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 8);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "12345678"));
  EXPECT_FALSE(feed(writer, "9"));

  EXPECT_TRUE(writer.capExceeded());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
  EXPECT_FALSE(writer.commit());
}

// The cap is checked before the write, so not one byte past it reaches the card.
TEST(PartFileWriter, NoBytesPastTheCapAreWritten) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 4);
  ASSERT_TRUE(writer.begin());
  EXPECT_FALSE(feed(writer, "123456"));
  EXPECT_EQ(sink.files["/books/a.epub.part"], "");
  EXPECT_EQ(writer.bytes(), 0u);
}

TEST(PartFileWriter, WriteFailureAbortsWithoutPublishing) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  sink.failWrite = true;
  EXPECT_FALSE(feed(writer, "hello"));
  EXPECT_FALSE(writer.commit());
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

TEST(PartFileWriter, OpenFailureIsReportedByBegin) {
  FakeFileSink sink;
  sink.failOpen = true;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  EXPECT_FALSE(writer.begin());
}

TEST(PartFileWriter, PublishFailureLeavesThePartInPlace) {
  FakeFileSink sink;
  sink.failPublish = true;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(feed(writer, "hello"));

  EXPECT_FALSE(writer.commit());
  EXPECT_TRUE(sink.exists("/books/a.epub.part"));
  EXPECT_FALSE(sink.exists("/books/a.epub"));
}

TEST(PartFileWriter, ZeroByteResponseStillPublishes) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  ASSERT_TRUE(writer.begin());
  ASSERT_TRUE(writer.commit());
  EXPECT_TRUE(sink.exists("/books/a.epub"));
  EXPECT_EQ(writer.bytes(), 0u);
}

TEST(PartFileWriter, DataBeforeBeginIsRejected) {
  FakeFileSink sink;
  PartFileWriter writer(sink, "/books/a.epub", 1024);
  EXPECT_FALSE(feed(writer, "hello"));
}
```

Create `test/bookorbit_catalog_download/CMakeLists.txt`:

```cmake
add_executable(CatalogDownloadTest
  CatalogDownloadTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDownload.cpp
)

target_include_directories(CatalogDownloadTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogDownloadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogDownloadTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_download)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogDownload.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/IFileSink.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// Narrow file interface for streamed transfers. Deliberately smaller than
// IBlobStore (P0 Task 4): a download is written incrementally and never held in
// a buffer, so this exposes an open/write/close cursor rather than a
// whole-blob write.
class IFileSink {
 public:
  virtual ~IFileSink() = default;

  // Opens `path` for writing, truncating anything already there.
  virtual bool open(std::string_view path) = 0;
  virtual bool write(const uint8_t* data, size_t len) = 0;
  // Flushes and closes the current file. Safe to call when nothing is open.
  virtual bool close() = 0;
  // Atomic rename. `from` must be closed first.
  virtual bool publish(std::string_view from, std::string_view to) = 0;
  virtual bool remove(std::string_view path) = 0;
  virtual bool exists(std::string_view path) = 0;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogDownload.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "IFileSink.h"

namespace bookorbit {

// Ceiling for a transfer whose size the server did not record
// (bookorbit_transfer_policy.lua:17).
constexpr size_t kMaxTransferBytes = 512u * 1024u * 1024u;

// Byte cap for one transfer: min(512 MiB, ceil(expected * 1.25) + 1 MiB).
// The margin exists because servers may add container overhead, so the
// recorded size is a bound rather than an exact length.
size_t maxBytesForExpected(size_t expectedBytes);

// "<name>.part" — the temp path a transfer writes to before publication.
std::string partPathFor(std::string_view finalPath);

// Streams a download into "<final>.part" and publishes it with an atomic
// rename. The invariant: the final path only ever appears once the whole body
// has arrived within the cap, so a ".part" left by a battery pull can never be
// mistaken for a complete book.
//
// Holds no buffer of its own. Bytes arrive in the transport's own chunk (2 KB
// for SecureHttpClient) and go straight to the sink, so RAM use is independent
// of file size — the property that keeps a large book safe on an ESP32-C3.
class PartFileWriter {
 public:
  PartFileWriter(IFileSink& sink, std::string finalPath, const size_t maxBytes)
      : sink_(sink),
        finalPath_(std::move(finalPath)),
        partPath_(partPathFor(finalPath_)),
        maxBytes_(maxBytes > 0 ? maxBytes : kMaxTransferBytes) {}

  // Opens the .part file, truncating any leftover from an interrupted run.
  bool begin();

  // Feed one body chunk. Returns false to abort the transfer: the cap would be
  // exceeded, or the card rejected the write. A false return leaves the writer
  // permanently failed — commit() will refuse.
  bool onData(const uint8_t* data, size_t len);

  // Closes the .part and renames it onto the final path. Returns false if the
  // transfer failed, was abandoned, or the rename did not succeed.
  bool commit();

  // Closes the .part and leaves it in place. Used on cancel or transport loss.
  void abandon();

  bool capExceeded() const { return capExceeded_; }
  bool failed() const { return failed_; }
  size_t bytes() const { return bytes_; }
  const std::string& partPath() const { return partPath_; }
  const std::string& finalPath() const { return finalPath_; }

 private:
  IFileSink& sink_;
  std::string finalPath_;
  std::string partPath_;
  size_t maxBytes_;
  size_t bytes_ = 0;
  bool started_ = false;
  bool closed_ = false;
  bool failed_ = false;
  bool capExceeded_ = false;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogDownload.cpp`:

```cpp
#include "CatalogDownload.h"

namespace bookorbit {
namespace {

// Servers may add container overhead, so the recorded size is a margin rather
// than an exact ceiling (bookorbit_transfer_policy.lua:19-21).
constexpr size_t kSizeMarginNumerator = 5;   // 1.25 == 5/4, done in integers so
constexpr size_t kSizeMarginDenominator = 4;  // no float rounding creeps in
constexpr size_t kSizeSlackBytes = 1024u * 1024u;

}  // namespace

size_t maxBytesForExpected(const size_t expectedBytes) {
  if (expectedBytes == 0) return kMaxTransferBytes;
  // Guard the multiply before it happens rather than detecting the wrap after.
  if (expectedBytes > kMaxTransferBytes / kSizeMarginNumerator) return kMaxTransferBytes;
  const size_t scaled =
      (expectedBytes * kSizeMarginNumerator + kSizeMarginDenominator - 1) / kSizeMarginDenominator;
  if (scaled > kMaxTransferBytes - kSizeSlackBytes) return kMaxTransferBytes;
  return scaled + kSizeSlackBytes;
}

std::string partPathFor(const std::string_view finalPath) {
  std::string path(finalPath);
  path += ".part";
  return path;
}

bool PartFileWriter::begin() {
  if (started_) return false;
  // open() truncates, so a .part left behind by an interrupted run is
  // discarded rather than appended to.
  if (!sink_.open(partPath_)) {
    failed_ = true;
    return false;
  }
  started_ = true;
  return true;
}

bool PartFileWriter::onData(const uint8_t* data, const size_t len) {
  if (!started_ || closed_ || failed_) return false;
  if (len == 0) return true;

  // Checked before the write: not one byte past the cap reaches the card.
  if (len > maxBytes_ - bytes_) {
    capExceeded_ = true;
    failed_ = true;
    return false;
  }
  if (!sink_.write(data, len)) {
    failed_ = true;
    return false;
  }
  bytes_ += len;
  return true;
}

bool PartFileWriter::commit() {
  if (!started_ || failed_) {
    return false;
  }
  if (!closed_) {
    sink_.close();
    closed_ = true;
  }
  if (!sink_.publish(partPath_, finalPath_)) {
    // The .part stays on disk; the final path never appeared, so nothing
    // downstream can mistake this for a complete book.
    failed_ = true;
    return false;
  }
  return true;
}

void PartFileWriter::abandon() {
  if (started_ && !closed_) {
    sink_.close();
    closed_ = true;
  }
  failed_ = true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "PartFileWriter|PartPath|MaxBytesForExpected" --output-on-failure
```

Expected: 16 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/IFileSink.h lib/BookOrbit/CatalogDownload.h lib/BookOrbit/CatalogDownload.cpp test/bookorbit_catalog_download test/CMakeLists.txt
git commit -m "feat: add streamed BookOrbit download writer with atomic publish"
```

---

### Task 7: Partial MD5 on the finished file — the sync-key handoff

**Files:**
- Create: `lib/BookOrbit/CatalogSyncKey.h`, `lib/BookOrbit/CatalogSyncKey.cpp`
- Create: `test/bookorbit_catalog_synckey/CMakeLists.txt`, `test/bookorbit_catalog_synckey/CatalogSyncKeyTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `IFileSink`, `PartFileWriter` (Task 6); `ManifestItem` (Task 5).
- Produces:
  `class bookorbit::IDocumentHasher { virtual std::string partialMd5(std::string_view path) = 0; }`;
  `struct bookorbit::DownloadedBook { std::string path, hash; uint32_t bookId, fileId; size_t bytes; }`;
  `bool bookorbit::isPartialMd5(std::string_view value)`;
  `bool bookorbit::finalizeDownload(IFileSink& sink, IDocumentHasher& hasher, PartFileWriter& writer, const ManifestItem& item, DownloadedBook& out)`.

**Why this task exists.** `KOReaderDocumentId::calculate()` (`lib/KOReaderSync/KOReaderDocumentId.h:27`) already implements KOReader's partial MD5 exactly — 12 offsets at `1024 << 2i`, 1024 bytes each. That 32-character lowercase hex string is BookOrbit's *only* book key. It is `MatchCandidate.hash` in P0's `match-check`, the key of `BookSyncState`, and the `hash` field of P1's page-stats, P2's progress, P3's book-states and P4's annotation and bookmark exchanges. Hashing the file at the end of the download is therefore what closes the loop: **a freshly downloaded book is immediately syncable, with no separate identification pass and no dependence on the filename.**

The hash is computed on the **published** file, never on the `.part`. Hashing the `.part` would key the book by a path that is about to stop existing, and a failed publish would mint a sync key for a book that is not on the device.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_synckey/CatalogSyncKeyTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/CatalogSyncKey.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  bool failPublish = false;

  bool open(const std::string_view path) override {
    openPath.assign(path);
    files[openPath] = "";
    isOpen = true;
    return true;
  }
  bool write(const uint8_t* data, const size_t len) override {
    if (!isOpen) return false;
    files[openPath].append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  bool close() override {
    isOpen = false;
    return true;
  }
  bool publish(const std::string_view from, const std::string_view to) override {
    if (failPublish) return false;
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

// Records which path it was asked to hash, so the test can prove the finished
// file is hashed and not the .part.
class RecordingHasher : public bookorbit::IDocumentHasher {
 public:
  std::vector<std::string> hashed;
  std::string result = "0f0a792b00a37cf80baa5e50c078b31f";

  std::string partialMd5(const std::string_view path) override {
    hashed.emplace_back(path);
    return result;
  }
};

bookorbit::ManifestItem item() {
  bookorbit::ManifestItem entry;
  entry.bookId = 41;
  entry.fileId = 902;
  entry.fileBytes = 5;
  entry.filename = "we-solve-murders.epub";
  return entry;
}

bookorbit::PartFileWriter completedWriter(FakeFileSink& sink) {
  bookorbit::PartFileWriter writer(sink, "/books/we-solve-murders.epub", 1024);
  writer.begin();
  const std::string payload = "hello";
  writer.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  return writer;
}

}  // namespace

using bookorbit::DownloadedBook;
using bookorbit::finalizeDownload;
using bookorbit::isPartialMd5;

TEST(PartialMd5Shape, AcceptsThirtyTwoLowercaseHexDigits) {
  EXPECT_TRUE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31f"));
}

TEST(PartialMd5Shape, RejectsWrongLengthUppercaseAndNonHex) {
  EXPECT_FALSE(isPartialMd5(""));
  EXPECT_FALSE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31"));
  EXPECT_FALSE(isPartialMd5("0F0A792B00A37CF80BAA5E50C078B31F"));
  EXPECT_FALSE(isPartialMd5("0f0a792b00a37cf80baa5e50c078b31z"));
}

// The core guarantee: the hash is taken from the published file, not the .part.
TEST(FinalizeDownload, HashesThePublishedFileNotThePart) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, item(), book));
  ASSERT_EQ(hasher.hashed.size(), 1u);
  EXPECT_EQ(hasher.hashed[0], "/books/we-solve-murders.epub");
}

TEST(FinalizeDownload, ProducesTheSyncKeyAndIdentity) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_EQ(book.hash, "0f0a792b00a37cf80baa5e50c078b31f");
  EXPECT_EQ(book.path, "/books/we-solve-murders.epub");
  EXPECT_EQ(book.bookId, 41u);
  EXPECT_EQ(book.fileId, 902u);
  EXPECT_EQ(book.bytes, 5u);
  EXPECT_TRUE(sink.exists("/books/we-solve-murders.epub"));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub.part"));
}

// An unhashable file cannot be synced by any later phase, so it is removed
// rather than left as a book the device can never identify.
TEST(FinalizeDownload, UnhashableFileIsRemovedAndReportedAsFailure) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "";

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
  EXPECT_TRUE(book.hash.empty());
}

TEST(FinalizeDownload, MalformedHashIsTreatedAsUnhashable) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "not-a-hash";

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
}

// An interrupted transfer must never be hashed: it is not a book.
TEST(FinalizeDownload, AbandonedTransferIsNeverHashed) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  writer.abandon();
  RecordingHasher hasher;

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_TRUE(hasher.hashed.empty());
  EXPECT_TRUE(sink.exists("/books/we-solve-murders.epub.part"));
  EXPECT_FALSE(sink.exists("/books/we-solve-murders.epub"));
}

TEST(FinalizeDownload, PublishFailureIsNeverHashed) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  sink.failPublish = true;
  RecordingHasher hasher;

  DownloadedBook book;
  EXPECT_FALSE(finalizeDownload(sink, hasher, writer, item(), book));
  EXPECT_TRUE(hasher.hashed.empty());
}

// The device-computed hash is authoritative. A server hash that disagrees is
// recorded for diagnosis but never substituted: every other phase keys on what
// KOReaderDocumentId produced from the bytes actually on this card.
TEST(FinalizeDownload, DeviceHashWinsOverAServerSuppliedHash) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;
  hasher.result = "1111111111111111111111111111aaaa";

  auto entry = item();
  entry.hash = "2222222222222222222222222222bbbb";

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, entry, book));
  EXPECT_EQ(book.hash, "1111111111111111111111111111aaaa");
  EXPECT_TRUE(book.serverHashMismatch);
}

TEST(FinalizeDownload, MatchingServerHashIsNotFlagged) {
  FakeFileSink sink;
  auto writer = completedWriter(sink);
  RecordingHasher hasher;

  auto entry = item();
  entry.hash = "0f0a792b00a37cf80baa5e50c078b31f";

  DownloadedBook book;
  ASSERT_TRUE(finalizeDownload(sink, hasher, writer, entry, book));
  EXPECT_FALSE(book.serverHashMismatch);
}
```

Create `test/bookorbit_catalog_synckey/CMakeLists.txt`:

```cmake
add_executable(CatalogSyncKeyTest
  CatalogSyncKeyTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogSyncKey.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDownload.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogManifest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogSyncKeyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogSyncKeyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogSyncKeyTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_synckey)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogSyncKey.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogSyncKey.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "CatalogDownload.h"
#include "CatalogManifest.h"
#include "IFileSink.h"

namespace bookorbit {

// Injected hasher. The device binding wraps KOReaderDocumentId::calculate()
// (lib/KOReaderSync/KOReaderDocumentId.h:27), which already implements
// KOReader's partial MD5 exactly: 12 offsets at 1024 << 2i, 1024 bytes each.
// Injecting it keeps this logic host-testable without an SD card.
class IDocumentHasher {
 public:
  virtual ~IDocumentHasher() = default;
  // Returns a 32-character lowercase hex digest, or "" on failure.
  virtual std::string partialMd5(std::string_view path) = 0;
};

// A book that finished downloading and now has a sync key.
//
// `hash` is BookOrbit's only book identity: it is MatchCandidate.hash in P0's
// match-check, the key of BookSyncState, and the "hash" field of P1's
// page-stats, P2's progress, P3's book-states and P4's exchanges. Producing it
// here is what makes a freshly downloaded book immediately syncable.
struct DownloadedBook {
  std::string path;
  std::string hash;
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  size_t bytes = 0;
  // The server's own hash disagreed with the device's. Diagnostic only: the
  // device hash is always the one used, because it describes the bytes that
  // are actually on this card.
  bool serverHashMismatch = false;
};

// True for exactly 32 lowercase hex digits — the shape KOReaderDocumentId emits
// and the only shape BookOrbit accepts as a book key.
bool isPartialMd5(std::string_view value);

// Publishes the completed transfer and hashes the published file.
//
// Order matters: publish first, then hash the final path. Hashing the .part
// would key the book by a path that is about to disappear, and a failed publish
// would mint a sync key for a book that is not on the device.
//
// On an empty or malformed digest the published file is removed: a book the
// device cannot identify can never be synced by any later phase, so leaving it
// would create a permanently invisible library entry.
bool finalizeDownload(IFileSink& sink, IDocumentHasher& hasher, PartFileWriter& writer,
                      const ManifestItem& item, DownloadedBook& out);

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogSyncKey.cpp`:

```cpp
#include "CatalogSyncKey.h"

namespace bookorbit {
namespace {

bool isLowerHex(const char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

constexpr size_t kPartialMd5Length = 32;

}  // namespace

bool isPartialMd5(const std::string_view value) {
  if (value.size() != kPartialMd5Length) return false;
  for (const char c : value) {
    if (!isLowerHex(c)) return false;
  }
  return true;
}

bool finalizeDownload(IFileSink& sink, IDocumentHasher& hasher, PartFileWriter& writer,
                      const ManifestItem& item, DownloadedBook& out) {
  out = DownloadedBook{};

  // A failed or abandoned transfer is not a book. Its .part stays on disk and
  // is never hashed, so nothing downstream can mistake it for one.
  if (!writer.commit()) return false;

  const std::string hash = hasher.partialMd5(writer.finalPath());
  if (!isPartialMd5(hash)) {
    // Unidentifiable: remove it rather than leave a book no phase can sync.
    sink.remove(writer.finalPath());
    return false;
  }

  out.path = writer.finalPath();
  out.hash = hash;
  out.bookId = item.bookId;
  out.fileId = item.fileId;
  out.bytes = writer.bytes();
  out.serverHashMismatch = !item.hash.empty() && item.hash != hash;
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "FinalizeDownload|PartialMd5Shape" --output-on-failure
```

Expected: 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogSyncKey.h lib/BookOrbit/CatalogSyncKey.cpp test/bookorbit_catalog_synckey test/CMakeLists.txt
git commit -m "feat: hash downloaded BookOrbit books into their sync key"
```

---

### Task 8: Thumbnail transfer with an `image/` content-type guard

**Files:**
- Create: `lib/BookOrbit/CatalogThumbnail.h`, `lib/BookOrbit/CatalogThumbnail.cpp`
- Create: `test/bookorbit_catalog_thumbnail/CMakeLists.txt`, `test/bookorbit_catalog_thumbnail/CatalogThumbnailTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `PartFileWriter`, `IFileSink` (Task 6).
- Produces:
  `bool bookorbit::isImageContentType(std::string_view contentType)`;
  `std::string bookorbit::thumbnailPath(uint32_t bookId)`;
  `constexpr char bookorbit::kThumbnailAccept[] = "image/jpeg,image/*"`;
  `constexpr size_t bookorbit::kMaxThumbnailBytes = 512u * 1024u`;
  `class bookorbit::ThumbnailGate` with
  `ThumbnailGate(PartFileWriter& writer)`,
  `bool onHeaders(std::string_view contentType)`,
  `bool onData(const uint8_t* data, size_t len)`,
  `bool rejected() const`.

The guard runs on headers, before the first byte is written, so a login page or JSON error served in place of a cover never reaches the card and never opens a `.part` at all.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_thumbnail/CatalogThumbnailTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>

#include "lib/BookOrbit/CatalogDownload.h"
#include "lib/BookOrbit/CatalogThumbnail.h"
#include "lib/BookOrbit/IFileSink.h"

namespace {

class FakeFileSink : public bookorbit::IFileSink {
 public:
  std::map<std::string, std::string> files;
  std::string openPath;
  bool isOpen = false;
  int opens = 0;

  bool open(const std::string_view path) override {
    opens++;
    openPath.assign(path);
    files[openPath] = "";
    isOpen = true;
    return true;
  }
  bool write(const uint8_t* data, const size_t len) override {
    if (!isOpen) return false;
    files[openPath].append(reinterpret_cast<const char*>(data), len);
    return true;
  }
  bool close() override {
    isOpen = false;
    return true;
  }
  bool publish(const std::string_view from, const std::string_view to) override {
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

bool feed(bookorbit::ThumbnailGate& gate, const std::string& payload) {
  return gate.onData(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
}

}  // namespace

using bookorbit::isImageContentType;
using bookorbit::PartFileWriter;
using bookorbit::ThumbnailGate;
using bookorbit::thumbnailPath;

TEST(ImageContentType, AcceptsAnyImageSubtype) {
  EXPECT_TRUE(isImageContentType("image/jpeg"));
  EXPECT_TRUE(isImageContentType("image/png"));
  EXPECT_TRUE(isImageContentType("image/webp"));
}

TEST(ImageContentType, IsCaseInsensitive) {
  EXPECT_TRUE(isImageContentType("IMAGE/JPEG"));
  EXPECT_TRUE(isImageContentType("Image/Png"));
}

TEST(ImageContentType, IgnoresParametersAndLeadingSpace) {
  EXPECT_TRUE(isImageContentType("image/jpeg; charset=binary"));
  EXPECT_TRUE(isImageContentType("  image/jpeg"));
}

TEST(ImageContentType, RejectsEverythingElse) {
  EXPECT_FALSE(isImageContentType("text/html"));
  EXPECT_FALSE(isImageContentType("application/json"));
  EXPECT_FALSE(isImageContentType("application/octet-stream"));
  EXPECT_FALSE(isImageContentType(""));
  // A subtype-only prefix match must not pass: "imagex/..." is not an image.
  EXPECT_FALSE(isImageContentType("imagex/jpeg"));
}

TEST(ThumbnailPath, IsDerivedFromTheBookId) {
  EXPECT_EQ(thumbnailPath(41), "/.crosspoint/bookorbit/thumbs/41.jpg");
}

TEST(ThumbnailGate, ImageResponseWritesAndPublishes) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  ASSERT_TRUE(gate.onHeaders("image/jpeg"));
  ASSERT_TRUE(feed(gate, "\xff\xd8\xff\xe0jpegbytes"));
  ASSERT_TRUE(writer.commit());
  EXPECT_TRUE(sink.exists("/.crosspoint/bookorbit/thumbs/41.jpg"));
  EXPECT_FALSE(gate.rejected());
}

// The guard: a non-image response is refused before anything is opened, so no
// .part is ever created for it.
TEST(ThumbnailGate, NonImageResponseIsRefusedBeforeAnyFileIsOpened) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(gate.onHeaders("text/html; charset=utf-8"));
  EXPECT_TRUE(gate.rejected());
  EXPECT_EQ(sink.opens, 0);
  EXPECT_TRUE(sink.files.empty());
}

TEST(ThumbnailGate, DataAfterRejectionIsDiscarded) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  ASSERT_FALSE(gate.onHeaders("application/json"));
  EXPECT_FALSE(feed(gate, R"({"error":"unauthorized"})"));
  EXPECT_TRUE(sink.files.empty());
  EXPECT_FALSE(writer.commit());
}

TEST(ThumbnailGate, MissingContentTypeIsRefused) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(gate.onHeaders(""));
  EXPECT_TRUE(gate.rejected());
  EXPECT_EQ(sink.opens, 0);
}

TEST(ThumbnailGate, DataBeforeHeadersIsRefused) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), bookorbit::kMaxThumbnailBytes);
  ThumbnailGate gate(writer);

  EXPECT_FALSE(feed(gate, "\xff\xd8"));
  EXPECT_EQ(sink.opens, 0);
}

// An oversized cover is still capped, and still leaves nothing published.
TEST(ThumbnailGate, OversizedThumbnailIsCapped) {
  FakeFileSink sink;
  PartFileWriter writer(sink, thumbnailPath(41), 16);
  ThumbnailGate gate(writer);

  ASSERT_TRUE(gate.onHeaders("image/png"));
  EXPECT_FALSE(feed(gate, std::string(17, 'x')));
  EXPECT_TRUE(writer.capExceeded());
  EXPECT_FALSE(sink.exists("/.crosspoint/bookorbit/thumbs/41.jpg"));
}
```

Create `test/bookorbit_catalog_thumbnail/CMakeLists.txt`:

```cmake
add_executable(CatalogThumbnailTest
  CatalogThumbnailTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogThumbnail.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDownload.cpp
)

target_include_directories(CatalogThumbnailTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(CatalogThumbnailTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogThumbnailTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_thumbnail)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogThumbnail.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogThumbnail.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "CatalogDownload.h"

namespace bookorbit {

// Sent as the Accept header on the thumbnail route, matching the Lua client
// (bookorbit_api.lua:603).
constexpr char kThumbnailAccept[] = "image/jpeg,image/*";

// Covers are display assets, not books. Half a megabyte is far more than an
// 800x480 1-bit panel can use and still bounds a misbehaving server.
constexpr size_t kMaxThumbnailBytes = 512u * 1024u;

// True when the media type is image/*. Case-insensitive, tolerant of leading
// space and of parameters after ";". An empty or absent type is false: a
// thumbnail route that does not say it is sending an image is not trusted.
bool isImageContentType(std::string_view contentType);

std::string thumbnailPath(uint32_t bookId);

// Wraps a PartFileWriter with the content-type check. The writer is opened
// only once the headers prove the body is an image, so a login page or a JSON
// error served in place of a cover never creates a .part at all.
class ThumbnailGate {
 public:
  explicit ThumbnailGate(PartFileWriter& writer) : writer_(writer) {}

  // Call once, with the response Content-Type, before any body byte.
  // Returns false to abort the transfer.
  bool onHeaders(std::string_view contentType);

  // Body chunk. Refuses everything until onHeaders has accepted.
  bool onData(const uint8_t* data, size_t len);

  bool rejected() const { return rejected_; }

 private:
  PartFileWriter& writer_;
  bool accepted_ = false;
  bool rejected_ = false;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/CatalogThumbnail.cpp`:

```cpp
#include "CatalogThumbnail.h"

#include <cstdio>

namespace bookorbit {
namespace {

constexpr char kImagePrefix[] = "image/";
constexpr size_t kImagePrefixLen = sizeof(kImagePrefix) - 1;

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

bool isImageContentType(std::string_view contentType) {
  while (!contentType.empty() && (contentType.front() == ' ' || contentType.front() == '\t')) {
    contentType.remove_prefix(1);
  }
  if (contentType.size() < kImagePrefixLen + 1) return false;
  for (size_t i = 0; i < kImagePrefixLen; ++i) {
    if (lower(contentType[i]) != kImagePrefix[i]) return false;
  }
  // Require a subtype so "image/" alone, or "imagex/...", cannot slip through.
  const char subtypeFirst = contentType[kImagePrefixLen];
  return subtypeFirst != ';' && subtypeFirst != ' ';
}

std::string thumbnailPath(const uint32_t bookId) {
  // 64 bytes covers the fixed prefix plus a 10-digit id and ".jpg"; well under
  // the 256-byte stack guidance.
  char buffer[64];
  const int written = snprintf(buffer, sizeof(buffer), "/.crosspoint/bookorbit/thumbs/%u.jpg", bookId);
  if (written <= 0) return {};
  return std::string(buffer, static_cast<size_t>(written));
}

bool ThumbnailGate::onHeaders(const std::string_view contentType) {
  if (!isImageContentType(contentType)) {
    rejected_ = true;
    return false;
  }
  if (!writer_.begin()) {
    rejected_ = true;
    return false;
  }
  accepted_ = true;
  return true;
}

bool ThumbnailGate::onData(const uint8_t* data, const size_t len) {
  if (!accepted_ || rejected_) return false;
  return writer_.onData(data, len);
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "ImageContentType|ThumbnailPath|ThumbnailGate" --output-on-failure
```

Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogThumbnail.h lib/BookOrbit/CatalogThumbnail.cpp test/bookorbit_catalog_thumbnail test/CMakeLists.txt
git commit -m "feat: add BookOrbit thumbnail transfer with content-type guard"
```

---

### Task 9: read-status and rating mutations

**Files:**
- Create: `lib/BookOrbit/CatalogMutations.h`, `lib/BookOrbit/CatalogMutations.cpp`
- Modify: `lib/BookOrbit/CatalogApi.h`, `lib/BookOrbit/CatalogApi.cpp`
- Create: `test/bookorbit_catalog_mutations/CMakeLists.txt`, `test/bookorbit_catalog_mutations/CatalogMutationsTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::jsonEscape` (P0 Task 7); `BookOrbitClient::putJson`, `Error`, `Status` (P0 Tasks 2, 6).
- Produces:
  `enum class bookorbit::ReadStatus { Unread, Reading, Finished, Abandoned }`;
  `const char* bookorbit::readStatusToWire(ReadStatus status)`;
  `bool bookorbit::readStatusFromWire(std::string_view wire, ReadStatus& out)`;
  `std::string bookorbit::encodeReadStatus(ReadStatus status)`;
  `std::string bookorbit::encodeRating(int rating)`;
  `std::string bookorbit::readStatusPath(uint32_t bookId)`;
  `std::string bookorbit::ratingPath(uint32_t bookId)`;
  and on `CatalogApi`:
  `Error setReadStatus(uint32_t bookId, ReadStatus status)`,
  `Error setRating(uint32_t bookId, int rating)`.

Clearing a rating is explicit (`{"rating":null}`, `bookorbit_api.lua:576-579`), never an absent field — an absent field means "unchanged".

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_catalog_mutations/CatalogMutationsTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/CatalogApi.h"
#include "lib/BookOrbit/CatalogMutations.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpResponse> queued;
  std::vector<bookorbit::HttpRequest> sent;
  size_t index = 0;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (index >= queued.size()) return {200, false, "{}"};
    return queued[index++];
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

struct Fixture {
  ScriptedTransport transport;
  bookorbit::CapabilityCache capabilities;
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "u", "k", identity()};
  bookorbit::CatalogApi api{client, capabilities};
};

}  // namespace

using bookorbit::encodeRating;
using bookorbit::encodeReadStatus;
using bookorbit::ratingPath;
using bookorbit::ReadStatus;
using bookorbit::readStatusFromWire;
using bookorbit::readStatusPath;
using bookorbit::readStatusToWire;
using bookorbit::Status;

TEST(ReadStatusWire, MapsEveryValue) {
  EXPECT_STREQ(readStatusToWire(ReadStatus::Unread), "unread");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Reading), "reading");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Finished), "finished");
  EXPECT_STREQ(readStatusToWire(ReadStatus::Abandoned), "abandoned");
}

TEST(ReadStatusWire, ParsesBackFromTheWire) {
  ReadStatus status = ReadStatus::Unread;
  ASSERT_TRUE(readStatusFromWire("reading", status));
  EXPECT_EQ(status, ReadStatus::Reading);
  ASSERT_TRUE(readStatusFromWire("abandoned", status));
  EXPECT_EQ(status, ReadStatus::Abandoned);
}

TEST(ReadStatusWire, RejectsUnknownValues) {
  ReadStatus status = ReadStatus::Reading;
  EXPECT_FALSE(readStatusFromWire("complete", status));
  EXPECT_FALSE(readStatusFromWire("", status));
  EXPECT_EQ(status, ReadStatus::Reading);
}

TEST(EncodeMutations, ReadStatusBody) {
  EXPECT_EQ(encodeReadStatus(ReadStatus::Finished), R"({"status":"finished"})");
}

TEST(EncodeMutations, RatingBody) {
  EXPECT_EQ(encodeRating(4), R"({"rating":4})");
  EXPECT_EQ(encodeRating(1), R"({"rating":1})");
  EXPECT_EQ(encodeRating(5), R"({"rating":5})");
}

// Clearing is explicit: null, not an absent field. An absent field means
// "unchanged" on the server.
TEST(EncodeMutations, ClearingARatingSendsNull) {
  EXPECT_EQ(encodeRating(0), R"({"rating":null})");
  EXPECT_EQ(encodeRating(-1), R"({"rating":null})");
}

TEST(EncodeMutations, OutOfRangeRatingIsClampedToTheTopOfTheScale) {
  EXPECT_EQ(encodeRating(9), R"({"rating":5})");
}

TEST(MutationPaths, AreBuiltFromTheBookId) {
  EXPECT_EQ(readStatusPath(41), "/koreader/plugin/catalog/books/41/read-status");
  EXPECT_EQ(ratingPath(41), "/koreader/plugin/catalog/books/41/rating");
}

TEST(CatalogApiMutations, SetReadStatusPutsTheBody) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setReadStatus(41, ReadStatus::Reading).status, Status::Ok);
  ASSERT_EQ(f.transport.sent.size(), 1u);
  EXPECT_EQ(f.transport.sent[0].method, "PUT");
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/books/41/read-status");
  EXPECT_EQ(f.transport.sent[0].body, R"({"status":"reading"})");
}

TEST(CatalogApiMutations, SetRatingPutsTheBody) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setRating(41, 4).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].method, "PUT");
  EXPECT_EQ(f.transport.sent[0].url,
            "https://books.example.com/api/v1/koreader/plugin/catalog/books/41/rating");
  EXPECT_EQ(f.transport.sent[0].body, R"({"rating":4})");
}

TEST(CatalogApiMutations, ClearRatingPutsNull) {
  Fixture f;
  f.transport.queued.push_back({200, false, "{}"});

  ASSERT_EQ(f.api.setRating(41, 0).status, Status::Ok);
  EXPECT_EQ(f.transport.sent[0].body, R"({"rating":null})");
}

TEST(CatalogApiMutations, ServerErrorIsReportedAndNotSwallowed) {
  Fixture f;
  f.transport.queued.push_back({500, false, ""});
  EXPECT_EQ(f.api.setRating(41, 3).status, Status::ServerError);
}

TEST(CatalogApiMutations, AuthErrorIsReported) {
  Fixture f;
  f.transport.queued.push_back({401, false, ""});
  EXPECT_EQ(f.api.setReadStatus(41, ReadStatus::Finished).status, Status::Unauthorized);
}
```

Create `test/bookorbit_catalog_mutations/CMakeLists.txt`:

```cmake
add_executable(CatalogMutationsTest
  CatalogMutationsTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogMutations.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogApi.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecode.cpp
  ${REPO_ROOT}/lib/BookOrbit/CatalogDecodeCommon.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(CatalogMutationsTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(CatalogMutationsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CatalogMutationsTest)
```

Add to `test/CMakeLists.txt`:

```cmake
add_subdirectory(bookorbit_catalog_mutations)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/CatalogMutations.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/CatalogMutations.h`:

```cpp
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
```

Create `lib/BookOrbit/CatalogMutations.cpp`:

```cpp
#include "CatalogMutations.h"

#include <cstdio>

namespace bookorbit {
namespace {

// 96 bytes covers the fixed route plus a 10-digit id; under the 256-byte
// stack guidance.
constexpr size_t kPathScratch = 96;

std::string formatPath(const char* format, const uint32_t bookId) {
  char buffer[kPathScratch];
  const int written = snprintf(buffer, sizeof(buffer), format, bookId);
  if (written <= 0) return {};
  return std::string(buffer, static_cast<size_t>(written));
}

}  // namespace

const char* readStatusToWire(const ReadStatus status) {
  switch (status) {
    case ReadStatus::Reading:
      return "reading";
    case ReadStatus::Finished:
      return "finished";
    case ReadStatus::Abandoned:
      return "abandoned";
    case ReadStatus::Unread:
    default:
      return "unread";
  }
}

bool readStatusFromWire(const std::string_view wire, ReadStatus& out) {
  if (wire == "unread") {
    out = ReadStatus::Unread;
  } else if (wire == "reading") {
    out = ReadStatus::Reading;
  } else if (wire == "finished") {
    out = ReadStatus::Finished;
  } else if (wire == "abandoned") {
    out = ReadStatus::Abandoned;
  } else {
    return false;
  }
  return true;
}

std::string encodeReadStatus(const ReadStatus status) {
  std::string json = R"({"status":")";
  json += readStatusToWire(status);
  json += R"("})";
  return json;
}

std::string encodeRating(const int rating) {
  if (rating <= 0) return R"({"rating":null})";
  const int clamped = rating > 5 ? 5 : rating;
  std::string json = R"({"rating":)";
  json += static_cast<char>('0' + clamped);
  json += '}';
  return json;
}

std::string readStatusPath(const uint32_t bookId) {
  return formatPath("/koreader/plugin/catalog/books/%u/read-status", bookId);
}

std::string ratingPath(const uint32_t bookId) {
  return formatPath("/koreader/plugin/catalog/books/%u/rating", bookId);
}

}  // namespace bookorbit
```

Append to `lib/BookOrbit/CatalogApi.h`, in the public section of `CatalogApi`:

```cpp
  // Write side. Both routes answer with a small JSON envelope that the catalog
  // UI does not need, so the body is decoded only far enough to report status.
  Error setReadStatus(uint32_t bookId, ReadStatus status);

  // rating <= 0 clears the rating explicitly; 1..5 sets it.
  Error setRating(uint32_t bookId, int rating);
```

Add `#include "CatalogMutations.h"` to `lib/BookOrbit/CatalogApi.h`, and append to `lib/BookOrbit/CatalogApi.cpp`:

```cpp
Error CatalogApi::setReadStatus(const uint32_t bookId, const ReadStatus status) {
  std::string body;
  return client_.putJson(readStatusPath(bookId), encodeReadStatus(status), body);
}

Error CatalogApi::setRating(const uint32_t bookId, const int rating) {
  std::string body;
  return client_.putJson(ratingPath(bookId), encodeRating(rating), body);
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R "ReadStatusWire|EncodeMutations|MutationPaths|CatalogApiMutations" --output-on-failure
```

Expected: 13 tests PASS. Re-run `-R CatalogApi` to confirm Task 4's 13 tests still pass.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/CatalogMutations.h lib/BookOrbit/CatalogMutations.cpp lib/BookOrbit/CatalogApi.h lib/BookOrbit/CatalogApi.cpp test/bookorbit_catalog_mutations test/CMakeLists.txt
git commit -m "feat: add BookOrbit catalog read-status and rating mutations"
```

---
