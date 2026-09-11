# BookOrbit P4 — Bookmarks and Highlights Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two-way annotation sync between CrossInk's local highlights (`ClippingStore`) and dogears (`BookmarkStore`) and a BookOrbit server, over the three-legged `exchange` / `exchange-ack` protocol, with server-side deletion detection and a change signature that keeps an unchanged book off the wire entirely.

> **BLOCKING DEPENDENCY — P2 must land first.** BookOrbit identifies every annotation by its `pos0`/`pos1` crengine xpointers, and the server-side key is `md5(datetime + "|" + pos0)`. A pos0 that is not byte-identical to what the server hashed produces a different key, which the server reads as a *different annotation* — duplicates on every sync. P4 therefore canonicalizes every position through **P2's `lib/BookOrbit/XPointer.{h,cpp}`** before hashing, encoding, or comparing it. Do not write xpointer parsing in this phase. The exact API this phase consumes is pinned under *Consumed from P2* below; if P2 shipped a different signature, fix the call sites here rather than reimplementing parsing.

**Architecture:** All protocol logic lives in `lib/BookOrbit/`, driven by the P0 interfaces (`IHttpTransport`, `IBlobStore`) and one new injected interface, `IAnnotationApplier`, so the whole exchange state machine — including crash-safety between exchange and ack — runs under the native GoogleTest suite with no Arduino dependency. Exactly one device-only file maps CrossInk's stores onto the wire shapes.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), `lib/JsonParser/StreamingJsonParser` for response decoding, P0's `BookOrbitClient` for transport.

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md` — section "P4 — Bookmarks and highlights".

**Protocol source of truth:** `koreader/plugins/bookorbit.koplugin/bookorbit_annotations.lua`, `bookorbit_bookmarks.lua`, `bookorbit_sidecar.lua` (`normalizeAnnotations`, `normalizeBookmarks`).

## Global Constraints

- Repo guide is `AGENTS.md` (mirrored to `CLAUDE.md`). Its rules bind every task.
- Branch prefix `feat/`; commit messages `<type>: <short summary>`.
- All user-facing strings via `tr(STR_*)`. Logs may be hardcoded. Keys go in `lib/I18n/translations/english.yaml`, regenerated with `python3 scripts/gen_i18n.py`. Never hand-edit `lib/I18n/I18nKeys.h` or `I18nStrings.{h,cpp}`.
- No exceptions, no `abort()`. `LOG_ERR(...)` then `return false` on recoverable failure.
- `new` is not nothrow on ESP32. Use `new (std::nothrow)` or `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`.
- Local stack allocations over 256 bytes must be justified in a comment.
- Prefer `string_view`, `char[]`, `snprintf` over `std::string` in hot paths. `string_view::data()` is **not** null-terminated — never pass it to a C API.
- File I/O uses `FsFile`, never Arduino `File`. Always close explicitly.
- Shared code must stay within ESP32-C3 limits (~380 KB internal RAM, no PSRAM) unless capability-gated.
- Request bodies capped at **900 KiB** (`kMaxBodyBytes = 900 * 1024`, P0).
- Batch and cap limits, copied verbatim from the spec:
  - `UPLOAD_CHUNK` = **50** changes per exchange request.
  - `MAX_PULL_ROUNDS` = **10** apply/ack rounds per book per sync.
  - `MAX_KEYS_PER_BOOK` = **5000** for annotations, **500** for bookmarks. Over the cap, send `keysComplete:false` and omit `keys` — deletion detection degrades gracefully rather than deleting wrongly.
- Normalization, copied verbatim from the spec:
  - Annotations: `{datetime, datetimeUpdated, drawer, color, text, note, chapter, pageno, posFormat, pos0, pos1}`, `drawer ∈ {lighten, underscore, strikeout, invert}`, `posFormat = "xpointer"`.
  - Truncation: `text` ≤ 10000, `note` ≤ 5000, `chapter` ≤ 500, `pos0`/`pos1` ≤ 4000, `color` ≤ 30.
  - Bookmarks: `{datetime, datetimeUpdated, pos, pageno, chapter, note}`.
- Change-detection signature `"count:maxDatetime:hash1:hash2"`, stored per book in P0's `BookSyncState.annSignature` / `bmSignature` (`char[48]`), compared before exchanging. Unchanged ⇒ the exchange is skipped entirely.
- `bookmarkSync` is capability-gated through P0's `bookorbit::CapabilityCache`. **The tri-state rule is the single most important invariant in this phase:** a 5xx or a transport failure must leave the capability `Unknown` and must never cache a negative; only a confirmed 404 on the bookmark route calls `markUnsupported`.
- Do not edit generated files: `src/network/html/*.generated.h`, `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`, icon headers, hyphenation tries.
- Add a `CHANGELOG.md` entry for user-facing changes, grouped under Added/Changed/Fixed.
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally: `pio run -e x4-pro` and `pio run -e default`.

### Consumed from P0

```cpp
// lib/BookOrbit/BookOrbitError.h
enum class bookorbit::Status { Ok, Unauthorized, NotFound, ClientError, ServerError, Transport, BodyTooLarge, InvalidJson };
struct bookorbit::Error { Status status; int httpStatus; };
bool bookorbit::isAuthError(const Error&);
bool bookorbit::isTransient(const Error&);

// lib/BookOrbit/BookOrbitClient.h
class bookorbit::BookOrbitClient {
  Error postJson(std::string_view path, std::string_view json, std::string& outBody);
  static std::string withDevice(std::string_view bodyJson, const DeviceIdentity&, std::string_view deviceTime);
};

// lib/BookOrbit/BookOrbitSyncState.h
struct bookorbit::BookSyncState { char annSignature[48]; char bmSignature[48]; uint32_t annExchangedAt; uint32_t bmExchangedAt; /* … */ };
class bookorbit::SyncStateStore { BookSyncState* find(std::string_view md5); BookSyncState& findOrCreate(std::string_view md5); bool flush(); };

// lib/BookOrbit/BookOrbitCapabilities.h
enum class bookorbit::Capability { Unknown, Supported, Unsupported };
class bookorbit::CapabilityCache { Capability get(std::string_view) const; void markUnsupported(std::string_view); void rememberFailure(const Error&); };

// lib/BookOrbit/BookOrbitMatch.h
std::string bookorbit::jsonEscape(std::string_view value);
```

### Consumed from P2

```cpp
// lib/BookOrbit/XPointer.h
struct bookorbit::XPointerStep { std::string name; int index; };
struct bookorbit::XPointer { bool valid; int docFragment; std::vector<XPointerStep> steps; int charOffset; };
bookorbit::XPointer bookorbit::parseXPointer(std::string_view raw);
std::string bookorbit::emitXPointer(const XPointer& pointer);
// Parse + emit in one call. Returns "" when raw is not a crengine xpointer.
// This is the only P2 entry point P4 uses.
std::string bookorbit::normalizeXPointer(std::string_view raw);
```

### Design note: why the whole set is uploaded, not a datetime delta

The Lua annotation client keeps an `annWatermark` and uploads only entries newer than it. P0's `BookSyncState` has no such field — it carries `annSignature`/`annExchangedAt` only — and adding one is a P0 format change, not a P4 change. P4 therefore uploads the **full normalized set in `UPLOAD_CHUNK` slices**, exactly as `bookorbit_bookmarks.lua` already does for dogears, and relies on the signature skip to keep routine syncs free. This is not a fidelity loss: the server dedupes by `md5(datetime|pos0)`, so re-sending an unchanged entry is a no-op. It also fixes the same defect the Lua bookmark client documents — KOReader does not stamp `datetime_updated` on a note edit, so a watermark silently swallows renames. The cost is bounded: a book at the 5000-key cap re-uploads in 100 requests only when its signature actually changed.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/BookOrbitMd5.{h,cpp}` | Host-portable RFC 1321 MD5. `MD5Builder` is Arduino-only; the annotation key must be computable under CTest. |
| `lib/BookOrbit/BookOrbitAnnotationModel.{h,cpp}` | `Annotation`, normalization, truncation, change signature, key building. |
| `lib/BookOrbit/BookOrbitBookmarkModel.{h,cpp}` | `Bookmark`, normalization, change signature, key building. |
| `lib/BookOrbit/BookOrbitExchangePolicy.{h,cpp}` | Signature comparison and the 6-hour staleness bound that decide whether a book may skip its exchange. |
| `lib/BookOrbit/BookOrbitExchangeRequest.{h,cpp}` | `exchange` request encoding for both routes, including the key cap. |
| `lib/BookOrbit/BookOrbitExchangeResponse.{h,cpp}` | Streaming decode of `{unmatched, results:[{hash, toApply:{add,delete}, more}]}`. |
| `lib/BookOrbit/BookOrbitExchangeAck.{h,cpp}` | `exchange-ack` request encoding for both routes. |
| `lib/BookOrbit/BookOrbitAnnotationSync.{h,cpp}` | `IAnnotationApplier`; the annotation exchange state machine (chunking, pull rounds, crash-safe ack). |
| `lib/BookOrbit/BookOrbitBookmarkSync.{h,cpp}` | The bookmark exchange state machine plus `bookmarkSync` tri-state gating. |
| `src/bookorbit/CrossInkAnnotationSource.{h,cpp}` | `BookmarkStore`/`ClippingStore` ↔ wire shapes, and the device `IAnnotationApplier`. Device-only. |
| `test/bookorbit_*/` | One GoogleTest target per unit, registered in `test/CMakeLists.txt`. |

---

### Task 1: Host-portable MD5 and the annotation identity key

`KOReaderDocumentId.cpp:24` uses Arduino's `MD5Builder`, which does not exist under CTest. The identity key `md5(datetime + "|" + pos0)` is the hinge of the entire protocol, so it needs a host-testable MD5.

**Files:**
- Create: `lib/BookOrbit/BookOrbitMd5.h`, `lib/BookOrbit/BookOrbitMd5.cpp`
- Create: `test/bookorbit_md5/CMakeLists.txt`, `test/bookorbit_md5/BookOrbitMd5Test.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(bookorbit_md5)` beside the existing entries)

**Interfaces:**
- Consumes: nothing.
- Produces: `std::string bookorbit::md5Hex(std::string_view data)` — 32 lowercase hex characters.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_md5/BookOrbitMd5Test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::md5Hex;

TEST(BookOrbitMd5, RfcTestVectors) {
  EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
  EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
  EXPECT_EQ(md5Hex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
}

// Crosses the 56-byte padding boundary, so the two-block tail path runs.
TEST(BookOrbitMd5, SpansTwoPaddingBlocks) {
  EXPECT_EQ(md5Hex("The quick brown fox jumps over the lazy dog"), "9e107d9d372bb6826bd81d3542a419d6");
}

TEST(BookOrbitMd5, HandlesManyBlocks) {
  EXPECT_EQ(md5Hex(std::string(1000, 'x')), "398533d48111e9f664b1f64cb10c4b63");
}

TEST(BookOrbitMd5, OutputIsAlwaysThirtyTwoLowercaseHexChars) {
  const std::string digest = md5Hex("anything");
  ASSERT_EQ(digest.size(), 32u);
  for (const char ch : digest) {
    EXPECT_TRUE((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) << "non-hex char: " << ch;
  }
}

TEST(BookOrbitMd5, HandlesEmbeddedNulBytes) {
  const std::string withNul("a\0b", 3);
  EXPECT_NE(md5Hex(withNul), md5Hex("ab"));
}

// These are the real BookOrbit annotation keys for canonical xpointers:
// md5(datetime .. "|" .. pos0), matching BookOrbitAnnotations.buildKey().
TEST(BookOrbitMd5, MatchesBookOrbitAnnotationKeys) {
  EXPECT_EQ(md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17"),
            "08759494897afa79aec0d37d83498d30");
  EXPECT_EQ(md5Hex("2026-08-21 09:15:42|/body[1]/DocFragment[2]/body[1]/div[1]/p[7]/text()[1].0"),
            "256f02a78307c97781079db2c35e4397");
}

// A one-character difference in the char offset is a different annotation.
TEST(BookOrbitMd5, CharOffsetChangesTheKey) {
  EXPECT_NE(md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17"),
            md5Hex("2026-09-11 14:03:00|/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].99"));
}
```

Create `test/bookorbit_md5/CMakeLists.txt`:

```cmake
add_executable(BookOrbitMd5Test
  BookOrbitMd5Test.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
)

target_include_directories(BookOrbitMd5Test PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitMd5Test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitMd5Test)
```

Add to `test/CMakeLists.txt`, after the last existing `add_subdirectory(...)` line:

```cmake
add_subdirectory(bookorbit_md5)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/BookOrbitMd5.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitMd5.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

namespace bookorbit {

// RFC 1321 MD5, returned as 32 lowercase hex characters.
//
// Arduino's MD5Builder (used by KOReaderDocumentId.cpp:24) is unavailable on
// the host, and the BookOrbit annotation identity key md5(datetime|pos0) must
// be computable under the native test suite. Stack use is 64 bytes of state
// plus a 128-byte tail buffer — well inside the 256-byte guideline.
std::string md5Hex(std::string_view data);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitMd5.cpp`:

```cpp
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

constexpr uint8_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

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
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitMd5 --output-on-failure
```

Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitMd5.h lib/BookOrbit/BookOrbitMd5.cpp test/bookorbit_md5 test/CMakeLists.txt
git commit -m "feat: add host-portable MD5 for BookOrbit annotation keys"
```

---

### Task 2: Annotation normalization, truncation, and change signature

**Files:**
- Create: `lib/BookOrbit/BookOrbitAnnotationModel.h`, `lib/BookOrbit/BookOrbitAnnotationModel.cpp`
- Create: `test/bookorbit_annotation_model/CMakeLists.txt`, `test/bookorbit_annotation_model/BookOrbitAnnotationModelTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::md5Hex` (Task 1); `bookorbit::normalizeXPointer` (P2).
- Produces:
  `struct bookorbit::Annotation { std::string datetime, datetimeUpdated, drawer, color, text, note, chapter, posFormat, pos0, pos1; int32_t pageno; }`;
  `struct bookorbit::AnnotationKey { std::string k, dt; }`;
  `struct bookorbit::NormalizedAnnotations { std::vector<Annotation> entries; std::string maxDatetime; std::string signature; }`;
  `NormalizedAnnotations bookorbit::normalizeAnnotations(const std::vector<Annotation>& raw)`;
  `std::string bookorbit::buildAnnotationKey(std::string_view datetime, std::string_view pos0)`;
  `std::vector<AnnotationKey> bookorbit::collectAnnotationKeys(const std::vector<Annotation>& normalized)`;
  `bool bookorbit::isDeviceDatetime(std::string_view value)`.

Truncation limits, verbatim from the spec: `text` ≤ 10000, `note` ≤ 5000, `chapter` ≤ 500, `pos0`/`pos1` ≤ 4000, `color` ≤ 30.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_annotation_model/BookOrbitAnnotationModelTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitAnnotationModel.h"
#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::Annotation;
using bookorbit::buildAnnotationKey;
using bookorbit::collectAnnotationKeys;
using bookorbit::normalizeAnnotations;

namespace {

constexpr char kPosA[] = "/body[1]/DocFragment[2]/body[1]/div[1]/p[7]/text()[1].0";
constexpr char kPosB[] = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17";

Annotation highlightA() {
  Annotation entry;
  entry.datetime = "2026-08-21 09:15:42";
  entry.drawer = "lighten";
  entry.text = "the sea, the sea";
  entry.chapter = "Chapter Two";
  entry.pageno = 31;
  entry.pos0 = kPosA;
  entry.pos1 = kPosA;
  return entry;
}

Annotation highlightB() {
  Annotation entry;
  entry.datetime = "2026-09-11 14:03:00";
  entry.datetimeUpdated = "2026-09-11 15:00:00";
  entry.drawer = "underscore";
  entry.color = "yellow";
  entry.text = "a second highlight";
  entry.note = "look this up";
  entry.pos0 = kPosB;
  entry.pos1 = kPosB;
  return entry;
}

}  // namespace

TEST(BookOrbitAnnotationModel, KeepsTheSpecifiedFields) {
  const auto normalized = normalizeAnnotations({highlightB()});
  ASSERT_EQ(normalized.entries.size(), 1u);
  const auto& entry = normalized.entries[0];
  EXPECT_EQ(entry.datetime, "2026-09-11 14:03:00");
  EXPECT_EQ(entry.datetimeUpdated, "2026-09-11 15:00:00");
  EXPECT_EQ(entry.drawer, "underscore");
  EXPECT_EQ(entry.color, "yellow");
  EXPECT_EQ(entry.text, "a second highlight");
  EXPECT_EQ(entry.note, "look this up");
  EXPECT_EQ(entry.posFormat, "xpointer");
  EXPECT_EQ(entry.pos0, kPosB);
  EXPECT_EQ(entry.pos1, kPosB);
}

TEST(BookOrbitAnnotationModel, AcceptsEveryAllowedDrawer) {
  for (const char* drawer : {"lighten", "underscore", "strikeout", "invert"}) {
    Annotation entry = highlightA();
    entry.drawer = drawer;
    EXPECT_EQ(normalizeAnnotations({entry}).entries.size(), 1u) << drawer;
  }
}

// drawer == "" marks a position-only bookmark; it belongs to the bookmark
// route, not the annotation route.
TEST(BookOrbitAnnotationModel, DropsEntriesWithNoDrawer) {
  Annotation entry = highlightA();
  entry.drawer.clear();
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsUnknownDrawers) {
  Annotation entry = highlightA();
  entry.drawer = "sparkle";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsEntriesWithMalformedDatetime) {
  Annotation entry = highlightA();
  entry.datetime = "2026-08-21T09:15:42Z";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsMalformedDatetimeUpdatedButKeepsTheEntry) {
  Annotation entry = highlightA();
  entry.datetimeUpdated = "yesterday";
  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_TRUE(normalized.entries[0].datetimeUpdated.empty());
}

// An unresolvable position cannot be keyed, so it must never be uploaded.
TEST(BookOrbitAnnotationModel, DropsEntriesWhosePositionIsNotAnXPointer) {
  Annotation entry = highlightA();
  entry.pos0 = "page 42";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

// P2 canonicalizes; an indexed and an unindexed form of the same position
// must produce the same key, or the server sees two annotations.
TEST(BookOrbitAnnotationModel, CanonicalizesPositionsThroughXPointer) {
  Annotation indexed = highlightA();
  Annotation unindexed = highlightA();
  unindexed.pos0 = "/body/DocFragment[2]/body/div/p[7]/text().0";
  unindexed.pos1 = unindexed.pos0;

  const auto left = normalizeAnnotations({indexed});
  const auto right = normalizeAnnotations({unindexed});
  ASSERT_EQ(left.entries.size(), 1u);
  ASSERT_EQ(right.entries.size(), 1u);
  EXPECT_EQ(left.entries[0].pos0, right.entries[0].pos0);
  EXPECT_EQ(left.signature, right.signature);
}

TEST(BookOrbitAnnotationModel, TruncatesToTheSpecLimits) {
  Annotation entry = highlightA();
  entry.text = std::string(10050, 'a');
  entry.note = std::string(5050, 'b');
  entry.chapter = std::string(550, 'c');
  entry.color = std::string(40, 'd');

  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].text.size(), 10000u);
  EXPECT_EQ(normalized.entries[0].note.size(), 5000u);
  EXPECT_EQ(normalized.entries[0].chapter.size(), 500u);
  EXPECT_EQ(normalized.entries[0].color.size(), 30u);
}

// Truncation must not split a multi-byte character, or the JSON body carries
// an invalid UTF-8 sequence.
TEST(BookOrbitAnnotationModel, TruncationDoesNotSplitUtf8) {
  Annotation entry = highlightA();
  entry.chapter.clear();
  for (int i = 0; i < 200; i++) entry.chapter += "\xE2\x80\x94";  // em dash, 3 bytes
  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  const std::string& chapter = normalized.entries[0].chapter;
  EXPECT_LE(chapter.size(), 500u);
  EXPECT_EQ(chapter.size() % 3, 0u);
}

TEST(BookOrbitAnnotationModel, MaxDatetimePrefersDatetimeUpdated) {
  const auto normalized = normalizeAnnotations({highlightA(), highlightB()});
  EXPECT_EQ(normalized.maxDatetime, "2026-09-11 15:00:00");
}

// The signature is "count:maxDatetime:hash1:hash2", the exact string the Lua
// plugin writes, so a book synced by KOReader and by CrossInk agrees.
TEST(BookOrbitAnnotationModel, SignatureMatchesTheLuaFormat) {
  EXPECT_EQ(normalizeAnnotations({highlightA(), highlightB()}).signature,
            "2:2026-09-11 15:00:00:3677518418:4274252292");
  EXPECT_EQ(normalizeAnnotations({highlightA()}).signature, "1:2026-08-21 09:15:42:1143470404:3130326164");
  EXPECT_EQ(normalizeAnnotations({}).signature, "0::0:0");
}

// Order-independent: the signature is a sum and a mix, not a running digest.
TEST(BookOrbitAnnotationModel, SignatureIgnoresEntryOrder) {
  EXPECT_EQ(normalizeAnnotations({highlightA(), highlightB()}).signature,
            normalizeAnnotations({highlightB(), highlightA()}).signature);
}

// Count plus max datetime alone cannot see a delete-plus-add that keeps both
// unchanged; the per-entry hashes are what catch it.
TEST(BookOrbitAnnotationModel, SignatureChangesWhenAnEntryIsReplaced) {
  Annotation replacement = highlightA();
  replacement.pos0 = kPosB;
  replacement.pos1 = kPosB;
  EXPECT_NE(normalizeAnnotations({highlightA()}).signature, normalizeAnnotations({replacement}).signature);
}

TEST(BookOrbitAnnotationModel, SignatureFitsTheBookSyncStateField) {
  // BookSyncState::annSignature is char[48]; a 5-digit count plus a 19-char
  // datetime plus two 10-digit hashes is 47 characters at worst.
  EXPECT_LE(normalizeAnnotations({highlightA(), highlightB()}).signature.size(), 47u);
}

TEST(BookOrbitAnnotationModel, KeyIsMd5OfDatetimePipePos0) {
  EXPECT_EQ(buildAnnotationKey("2026-09-11 14:03:00", kPosB),
            bookorbit::md5Hex(std::string("2026-09-11 14:03:00|") + kPosB));
  EXPECT_EQ(buildAnnotationKey("2026-09-11 14:03:00", kPosB), "08759494897afa79aec0d37d83498d30");
}

TEST(BookOrbitAnnotationModel, CollectKeysCarriesKeyAndDatetime) {
  const auto normalized = normalizeAnnotations({highlightA(), highlightB()});
  const auto keys = collectAnnotationKeys(normalized.entries);
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_EQ(keys[0].k, buildAnnotationKey("2026-08-21 09:15:42", kPosA));
  EXPECT_EQ(keys[0].dt, "2026-08-21 09:15:42");
  EXPECT_EQ(keys[1].dt, "2026-09-11 14:03:00");
}

// The key hashes datetime, never datetimeUpdated: an edited highlight keeps
// its identity.
TEST(BookOrbitAnnotationModel, EditingAnEntryDoesNotChangeItsKey) {
  Annotation edited = highlightB();
  edited.note = "a different note";
  edited.datetimeUpdated = "2026-09-12 08:00:00";
  const auto before = collectAnnotationKeys(normalizeAnnotations({highlightB()}).entries);
  const auto after = collectAnnotationKeys(normalizeAnnotations({edited}).entries);
  ASSERT_EQ(before.size(), 1u);
  ASSERT_EQ(after.size(), 1u);
  EXPECT_EQ(before[0].k, after[0].k);
}

TEST(BookOrbitAnnotationModel, AbsentPagenoStaysAbsent) {
  const auto normalized = normalizeAnnotations({highlightB()});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].pageno, -1);
}
```

Create `test/bookorbit_annotation_model/CMakeLists.txt`:

```cmake
add_executable(BookOrbitAnnotationModelTest
  BookOrbitAnnotationModelTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitAnnotationModelTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitAnnotationModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitAnnotationModelTest)
```

Add `add_subdirectory(bookorbit_annotation_model)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitAnnotationModel.h: No such file or directory`. If it instead fails on `XPointer.cpp: No such file`, P2 has not landed; stop and finish P2 first.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitAnnotationModel.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// Truncation limits, verbatim from the spec's P4 field mapping.
inline constexpr size_t kMaxAnnotationTextBytes = 10000;
inline constexpr size_t kMaxAnnotationNoteBytes = 5000;
inline constexpr size_t kMaxChapterBytes = 500;
inline constexpr size_t kMaxPosBytes = 4000;
inline constexpr size_t kMaxColorBytes = 30;

// One highlight in BookOrbit's wire shape. pageno == -1 means "absent"; the
// encoder omits the field rather than sending a sentinel.
struct Annotation {
  std::string datetime;         // "YYYY-MM-DD HH:MM:SS", local device time
  std::string datetimeUpdated;  // same format, empty when never edited
  std::string drawer;           // lighten | underscore | strikeout | invert
  std::string color;
  std::string text;
  std::string note;
  std::string chapter;
  int32_t pageno = -1;
  std::string posFormat;  // always "xpointer" after normalization
  std::string pos0;
  std::string pos1;
};

// The server's deletion-detection set: k is md5(datetime|pos0), dt the datetime.
struct AnnotationKey {
  std::string k;
  std::string dt;
};

struct NormalizedAnnotations {
  std::vector<Annotation> entries;
  std::string maxDatetime;  // max of (datetimeUpdated ? datetimeUpdated : datetime)
  std::string signature;    // "count:maxDatetime:hash1:hash2"
};

// True for exactly "YYYY-MM-DD HH:MM:SS". BookOrbit keys on this string, so a
// differently-shaped timestamp is not merely ugly — it is a different key.
bool isDeviceDatetime(std::string_view value);

// Drops entries that cannot be synced (no/unknown drawer, malformed datetime,
// position that is not a crengine xpointer), canonicalizes every position
// through P2's XPointer, truncates to the spec limits, and computes the
// order-independent change signature.
NormalizedAnnotations normalizeAnnotations(const std::vector<Annotation>& raw);

// md5(datetime + "|" + pos0) — BookOrbitAnnotations.buildKey() in Lua.
// pos0 must already be canonical, or the key will not match the server's.
std::string buildAnnotationKey(std::string_view datetime, std::string_view pos0);

std::vector<AnnotationKey> collectAnnotationKeys(const std::vector<Annotation>& normalized);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitAnnotationModel.cpp`:

```cpp
#include "BookOrbitAnnotationModel.h"

#include <cstdio>

#include "BookOrbitMd5.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

bool isDigits(const std::string_view value, const size_t from, const size_t count) {
  for (size_t i = from; i < from + count; i++) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

bool isAllowedDrawer(const std::string_view drawer) {
  return drawer == "lighten" || drawer == "underscore" || drawer == "strikeout" || drawer == "invert";
}

// Byte truncation, used for positions only: they are ASCII, and the server
// hashed exactly these bytes.
std::string truncateBytes(const std::string& value, const size_t limit) {
  if (value.size() <= limit) return value;
  return value.substr(0, limit);
}

// Truncation for human text. Backs off to the start of the last complete
// UTF-8 sequence so the JSON body never carries a split character.
std::string truncateUtf8(const std::string& value, const size_t limit) {
  if (value.size() <= limit) return value;
  size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
    cut--;
  }
  return value.substr(0, cut);
}

std::string head(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(0, count);
}

std::string tail(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(value.size() - count);
}

// djb2 over the entry's identity fields, matching bookorbit_sidecar.lua's
// entryHash(). Positions can be kilobytes long, so only their length and both
// ends are sampled: this signal decides whether a book may skip an exchange,
// never what gets uploaded.
uint32_t entryHash(const Annotation& entry) {
  char lengthBuf[16];
  snprintf(lengthBuf, sizeof(lengthBuf), "%zu", entry.pos0.size());

  std::string key;
  key.reserve(entry.datetime.size() + entry.pos0.size() + 80);
  key += entry.datetime;
  key += '|';
  key += lengthBuf;
  key += '|';
  key += head(entry.pos0, 24);
  key += '|';
  key += tail(entry.pos0, 24);
  key += '|';
  key += entry.datetimeUpdated;

  uint32_t hash = 5381;
  for (const char ch : key) {
    hash = hash * 33u + static_cast<unsigned char>(ch);  // wraps at 2^32, as Lua's % 4294967296 does
  }
  return hash;
}

}  // namespace

bool isDeviceDatetime(const std::string_view value) {
  if (value.size() != 19) return false;
  if (value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' || value[16] != ':') return false;
  return isDigits(value, 0, 4) && isDigits(value, 5, 2) && isDigits(value, 8, 2) && isDigits(value, 11, 2) &&
         isDigits(value, 14, 2) && isDigits(value, 17, 2);
}

NormalizedAnnotations normalizeAnnotations(const std::vector<Annotation>& raw) {
  NormalizedAnnotations normalized;
  normalized.entries.reserve(raw.size());

  uint32_t sum = 0;
  uint32_t mix = 0;

  for (const auto& candidate : raw) {
    if (!isAllowedDrawer(candidate.drawer)) continue;
    if (!isDeviceDatetime(candidate.datetime)) continue;

    const std::string canonicalPos0 = normalizeXPointer(candidate.pos0);
    if (canonicalPos0.empty()) continue;

    Annotation entry;
    entry.datetime = candidate.datetime;
    entry.datetimeUpdated = isDeviceDatetime(candidate.datetimeUpdated) ? candidate.datetimeUpdated : std::string();
    entry.drawer = candidate.drawer;
    entry.color = truncateUtf8(candidate.color, kMaxColorBytes);
    entry.text = truncateUtf8(candidate.text, kMaxAnnotationTextBytes);
    entry.note = truncateUtf8(candidate.note, kMaxAnnotationNoteBytes);
    entry.chapter = truncateUtf8(candidate.chapter, kMaxChapterBytes);
    entry.pageno = candidate.pageno;
    entry.posFormat = "xpointer";
    entry.pos0 = truncateBytes(canonicalPos0, kMaxPosBytes);
    entry.pos1 = truncateBytes(normalizeXPointer(candidate.pos1), kMaxPosBytes);

    const std::string& effective = entry.datetimeUpdated.empty() ? entry.datetime : entry.datetimeUpdated;
    if (effective > normalized.maxDatetime) {
      normalized.maxDatetime = effective;
    }

    const uint32_t hash = entryHash(entry);
    sum += hash;
    mix += static_cast<uint32_t>(static_cast<uint64_t>(hash) * ((hash % 8191u) + 1u));

    normalized.entries.push_back(std::move(entry));
  }

  char signature[64];
  snprintf(signature, sizeof(signature), "%zu:%s:%u:%u", normalized.entries.size(),
           normalized.maxDatetime.c_str(), sum, mix);
  normalized.signature = signature;
  return normalized;
}

std::string buildAnnotationKey(const std::string_view datetime, const std::string_view pos0) {
  std::string material;
  material.reserve(datetime.size() + pos0.size() + 1);
  material.append(datetime);
  material += '|';
  material.append(pos0);
  return md5Hex(material);
}

std::vector<AnnotationKey> collectAnnotationKeys(const std::vector<Annotation>& normalized) {
  std::vector<AnnotationKey> keys;
  keys.reserve(normalized.size());
  for (const auto& entry : normalized) {
    keys.push_back({buildAnnotationKey(entry.datetime, entry.pos0), entry.datetime});
  }
  return keys;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitAnnotationModel --output-on-failure
```

Expected: 18 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitAnnotationModel.h lib/BookOrbit/BookOrbitAnnotationModel.cpp test/bookorbit_annotation_model test/CMakeLists.txt
git commit -m "feat: add BookOrbit annotation normalization and change signature"
```

---

### Task 3: Bookmark normalization and change signature

Dogears have no highlighted text, so their signature must hash the note and the chapter too: KOReader does not stamp `datetime_updated` when a user renames a dogear, and without those fields in the hash a rename is invisible (`bookorbit_sidecar.lua:118-131`).

**Files:**
- Create: `lib/BookOrbit/BookOrbitBookmarkModel.h`, `lib/BookOrbit/BookOrbitBookmarkModel.cpp`
- Create: `test/bookorbit_bookmark_model/CMakeLists.txt`, `test/bookorbit_bookmark_model/BookOrbitBookmarkModelTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::md5Hex` (Task 1); `bookorbit::isDeviceDatetime` (Task 2); `bookorbit::normalizeXPointer` (P2).
- Produces:
  `struct bookorbit::Bookmark { std::string datetime, datetimeUpdated, pos, chapter, note; int32_t pageno; }`;
  `struct bookorbit::BookmarkKey { std::string k, dt; }`;
  `struct bookorbit::NormalizedBookmarks { std::vector<Bookmark> entries; std::string maxDatetime; std::string signature; }`;
  `NormalizedBookmarks bookorbit::normalizeBookmarks(const std::vector<Bookmark>& raw)`;
  `std::string bookorbit::buildBookmarkKey(std::string_view datetime, std::string_view pos)`;
  `std::vector<BookmarkKey> bookorbit::collectBookmarkKeys(const std::vector<Bookmark>& normalized)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_bookmark_model/BookOrbitBookmarkModelTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBookmarkModel.h"
#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::Bookmark;
using bookorbit::buildBookmarkKey;
using bookorbit::collectBookmarkKeys;
using bookorbit::normalizeBookmarks;

namespace {

constexpr char kPos[] = "/body[1]/DocFragment[5]/body[1]/p[3]/text()[1].0";

Bookmark dogear() {
  Bookmark entry;
  entry.datetime = "2026-09-01 08:00:00";
  entry.pos = kPos;
  entry.chapter = "Chapter 5";
  return entry;
}

}  // namespace

TEST(BookOrbitBookmarkModel, KeepsTheSpecifiedFields) {
  Bookmark entry = dogear();
  entry.datetimeUpdated = "2026-09-02 09:00:00";
  entry.note = "start here";
  entry.pageno = 77;

  const auto normalized = normalizeBookmarks({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].datetime, "2026-09-01 08:00:00");
  EXPECT_EQ(normalized.entries[0].datetimeUpdated, "2026-09-02 09:00:00");
  EXPECT_EQ(normalized.entries[0].pos, kPos);
  EXPECT_EQ(normalized.entries[0].chapter, "Chapter 5");
  EXPECT_EQ(normalized.entries[0].note, "start here");
  EXPECT_EQ(normalized.entries[0].pageno, 77);
}

TEST(BookOrbitBookmarkModel, DropsEntriesWithMalformedDatetime) {
  Bookmark entry = dogear();
  entry.datetime = "2026-09-01";
  EXPECT_TRUE(normalizeBookmarks({entry}).entries.empty());
}

// A paging document stores a page number, not an xpointer. Those are out of
// scope for v1 and must be dropped rather than uploaded unkeyable.
TEST(BookOrbitBookmarkModel, DropsNonXPointerPositions) {
  Bookmark entry = dogear();
  entry.pos = "412";
  EXPECT_TRUE(normalizeBookmarks({entry}).entries.empty());
}

TEST(BookOrbitBookmarkModel, TruncatesChapterAndNote) {
  Bookmark entry = dogear();
  entry.chapter = std::string(550, 'c');
  entry.note = std::string(550, 'n');
  const auto normalized = normalizeBookmarks({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].chapter.size(), 500u);
  EXPECT_EQ(normalized.entries[0].note.size(), 500u);
}

TEST(BookOrbitBookmarkModel, SignatureMatchesTheLuaFormat) {
  EXPECT_EQ(normalizeBookmarks({dogear()}).signature, "1:2026-09-01 08:00:00:3677112071:3604523879");
  EXPECT_EQ(normalizeBookmarks({}).signature, "0::0:0");
}

// The whole point of hashing the note: a rename leaves count, datetime and
// position identical, and must still force an exchange.
TEST(BookOrbitBookmarkModel, RenamingADogearChangesTheSignature) {
  Bookmark renamed = dogear();
  renamed.note = "Read again";
  EXPECT_EQ(normalizeBookmarks({renamed}).signature, "1:2026-09-01 08:00:00:3770681859:2913110196");
  EXPECT_NE(normalizeBookmarks({dogear()}).signature, normalizeBookmarks({renamed}).signature);
}

TEST(BookOrbitBookmarkModel, KeyIsMd5OfDatetimePipePos) {
  EXPECT_EQ(buildBookmarkKey("2026-09-01 08:00:00", kPos),
            bookorbit::md5Hex(std::string("2026-09-01 08:00:00|") + kPos));
}

TEST(BookOrbitBookmarkModel, CollectKeysCarriesKeyAndDatetime) {
  const auto keys = collectBookmarkKeys(normalizeBookmarks({dogear()}).entries);
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0].k, buildBookmarkKey("2026-09-01 08:00:00", kPos));
  EXPECT_EQ(keys[0].dt, "2026-09-01 08:00:00");
}

TEST(BookOrbitBookmarkModel, CanonicalizesPositionsThroughXPointer) {
  Bookmark unindexed = dogear();
  unindexed.pos = "/body/DocFragment[5]/body/p[3]/text().0";
  const auto left = normalizeBookmarks({dogear()});
  const auto right = normalizeBookmarks({unindexed});
  ASSERT_EQ(right.entries.size(), 1u);
  EXPECT_EQ(left.entries[0].pos, right.entries[0].pos);
}

TEST(BookOrbitBookmarkModel, MaxDatetimePrefersDatetimeUpdated) {
  Bookmark edited = dogear();
  edited.datetimeUpdated = "2026-09-05 10:11:12";
  EXPECT_EQ(normalizeBookmarks({edited}).maxDatetime, "2026-09-05 10:11:12");
}
```

Create `test/bookorbit_bookmark_model/CMakeLists.txt`:

```cmake
add_executable(BookOrbitBookmarkModelTest
  BookOrbitBookmarkModelTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitBookmarkModelTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitBookmarkModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBookmarkModelTest)
```

Add `add_subdirectory(bookorbit_bookmark_model)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitBookmarkModel.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitBookmarkModel.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// A dogear's note is a short label, not prose; the Lua client caps it at 500,
// and the cap must match or the two disagree on what was uploaded.
inline constexpr size_t kMaxBookmarkNoteBytes = 500;

// One position-only bookmark in BookOrbit's wire shape.
struct Bookmark {
  std::string datetime;
  std::string datetimeUpdated;
  std::string pos;  // crengine xpointer; page-number positions are dropped
  std::string chapter;
  std::string note;
  int32_t pageno = -1;
};

struct BookmarkKey {
  std::string k;
  std::string dt;
};

struct NormalizedBookmarks {
  std::vector<Bookmark> entries;
  std::string maxDatetime;
  std::string signature;  // "count:maxDatetime:hash1:hash2"
};

NormalizedBookmarks normalizeBookmarks(const std::vector<Bookmark>& raw);

// md5(datetime + "|" + pos) — BookOrbitBookmarks.buildKey() in Lua.
std::string buildBookmarkKey(std::string_view datetime, std::string_view pos);

std::vector<BookmarkKey> collectBookmarkKeys(const std::vector<Bookmark>& normalized);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitBookmarkModel.cpp`:

```cpp
#include "BookOrbitBookmarkModel.h"

#include <cstdio>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitMd5.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

std::string truncateUtf8(const std::string& value, const size_t limit) {
  if (value.size() <= limit) return value;
  size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
    cut--;
  }
  return value.substr(0, cut);
}

std::string head(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(0, count);
}

std::string tail(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(value.size() - count);
}

// Matches bookorbit_sidecar.lua's bookmarkHash(): the note and the chapter are
// part of the identity because KOReader does not stamp datetime_updated when a
// dogear is renamed.
uint32_t bookmarkHash(const Bookmark& entry) {
  char lengthBuf[16];
  snprintf(lengthBuf, sizeof(lengthBuf), "%zu", entry.pos.size());

  std::string key;
  key.reserve(entry.datetime.size() + entry.pos.size() + entry.note.size() + entry.chapter.size() + 80);
  key += entry.datetime;
  key += '|';
  key += lengthBuf;
  key += '|';
  key += head(entry.pos, 24);
  key += '|';
  key += tail(entry.pos, 24);
  key += '|';
  key += entry.datetimeUpdated;
  key += '|';
  key += entry.note;
  key += '|';
  key += entry.chapter;

  uint32_t hash = 5381;
  for (const char ch : key) {
    hash = hash * 33u + static_cast<unsigned char>(ch);
  }
  return hash;
}

}  // namespace

NormalizedBookmarks normalizeBookmarks(const std::vector<Bookmark>& raw) {
  NormalizedBookmarks normalized;
  normalized.entries.reserve(raw.size());

  uint32_t sum = 0;
  uint32_t mix = 0;

  for (const auto& candidate : raw) {
    if (!isDeviceDatetime(candidate.datetime)) continue;

    const std::string canonicalPos = normalizeXPointer(candidate.pos);
    if (canonicalPos.empty()) continue;

    Bookmark entry;
    entry.datetime = candidate.datetime;
    entry.datetimeUpdated = isDeviceDatetime(candidate.datetimeUpdated) ? candidate.datetimeUpdated : std::string();
    entry.pos = canonicalPos.size() > kMaxPosBytes ? canonicalPos.substr(0, kMaxPosBytes) : canonicalPos;
    entry.chapter = truncateUtf8(candidate.chapter, kMaxChapterBytes);
    entry.note = truncateUtf8(candidate.note, kMaxBookmarkNoteBytes);
    entry.pageno = candidate.pageno;

    const std::string& effective = entry.datetimeUpdated.empty() ? entry.datetime : entry.datetimeUpdated;
    if (effective > normalized.maxDatetime) {
      normalized.maxDatetime = effective;
    }

    const uint32_t hash = bookmarkHash(entry);
    sum += hash;
    mix += static_cast<uint32_t>(static_cast<uint64_t>(hash) * ((hash % 8191u) + 1u));

    normalized.entries.push_back(std::move(entry));
  }

  char signature[64];
  snprintf(signature, sizeof(signature), "%zu:%s:%u:%u", normalized.entries.size(),
           normalized.maxDatetime.c_str(), sum, mix);
  normalized.signature = signature;
  return normalized;
}

std::string buildBookmarkKey(const std::string_view datetime, const std::string_view pos) {
  std::string material;
  material.reserve(datetime.size() + pos.size() + 1);
  material.append(datetime);
  material += '|';
  material.append(pos);
  return md5Hex(material);
}

std::vector<BookmarkKey> collectBookmarkKeys(const std::vector<Bookmark>& normalized) {
  std::vector<BookmarkKey> keys;
  keys.reserve(normalized.size());
  for (const auto& entry : normalized) {
    keys.push_back({buildBookmarkKey(entry.datetime, entry.pos), entry.datetime});
  }
  return keys;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitBookmarkModel --output-on-failure
```

Expected: 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitBookmarkModel.h lib/BookOrbit/BookOrbitBookmarkModel.cpp test/bookorbit_bookmark_model test/CMakeLists.txt
git commit -m "feat: add BookOrbit bookmark normalization and change signature"
```

---

### Task 4: Exchange skip policy

The signature comparison that keeps routine syncs cheap. It is also the phase's crash-safety latch: the stamp is written only after a complete, fully-acknowledged exchange, so anything the server still owes us keeps the next exchange mandatory.

**Files:**
- Create: `lib/BookOrbit/BookOrbitExchangePolicy.h`, `lib/BookOrbit/BookOrbitExchangePolicy.cpp`
- Create: `test/bookorbit_exchange_policy/CMakeLists.txt`, `test/bookorbit_exchange_policy/BookOrbitExchangePolicyTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces:
  `inline constexpr uint32_t bookorbit::kExchangeMaxAgeSeconds = 6 * 3600;`
  `bool bookorbit::canSkipExchange(const char* storedSignature, uint32_t exchangedAt, std::string_view signature, uint32_t nowUnix)`;
  `bool bookorbit::rememberExchanged(char* storedSignature, size_t capacity, uint32_t& exchangedAt, std::string_view signature, uint32_t nowUnix)`.

These operate on the raw `char[48]` fields of P0's `BookSyncState` (`annSignature`/`annExchangedAt`, `bmSignature`/`bmExchangedAt`) so one implementation serves both routes.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_exchange_policy/BookOrbitExchangePolicyTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "lib/BookOrbit/BookOrbitExchangePolicy.h"

using bookorbit::canSkipExchange;
using bookorbit::kExchangeMaxAgeSeconds;
using bookorbit::rememberExchanged;

namespace {
constexpr char kSignature[] = "2:2026-09-11 15:00:00:3677518418:4274252292";
}

TEST(BookOrbitExchangePolicy, NeverExchangedCannotSkip) {
  char stored[48] = {};
  EXPECT_FALSE(canSkipExchange(stored, 0, kSignature, 2000000000u));
}

TEST(BookOrbitExchangePolicy, MatchingSignatureInsideTheWindowSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  ASSERT_TRUE(rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u));
  EXPECT_TRUE(canSkipExchange(stored, exchangedAt, kSignature, 2000000000u + kExchangeMaxAgeSeconds - 1));
}

// The exchange is the only channel that delivers web-created highlights, so a
// book may not skip forever even when nothing changed locally.
TEST(BookOrbitExchangePolicy, StaleStampForcesAnExchange) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, kSignature, 2000000000u + kExchangeMaxAgeSeconds));
}

TEST(BookOrbitExchangePolicy, ChangedSignatureForcesAnExchange) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, "3:2026-09-12 08:00:00:1:2", 2000000000u + 10u));
}

TEST(BookOrbitExchangePolicy, EmptySignatureNeverSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, "", 2000000000u + 10u));
}

// A clock that jumped backwards must not look like a fresh exchange.
TEST(BookOrbitExchangePolicy, StampInTheFutureNeverSkips) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, kSignature, 1999999000u));
}

// The field is char[48]. A signature that does not fit must not be half-written
// and must not stamp a skip, or the book silently stops exchanging.
TEST(BookOrbitExchangePolicy, OversizedSignatureIsRefusedAndLeavesNoStamp) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  const std::string tooLong(80, 'x');
  EXPECT_FALSE(rememberExchanged(stored, sizeof(stored), exchangedAt, tooLong, 2000000000u));
  EXPECT_EQ(stored[0], '\0');
  EXPECT_EQ(exchangedAt, 0u);
  EXPECT_FALSE(canSkipExchange(stored, exchangedAt, tooLong, 2000000000u));
}

TEST(BookOrbitExchangePolicy, RememberIsNulTerminatedAndExact) {
  char stored[48] = {};
  uint32_t exchangedAt = 0;
  rememberExchanged(stored, sizeof(stored), exchangedAt, kSignature, 2000000000u);
  EXPECT_STREQ(stored, kSignature);
  EXPECT_EQ(exchangedAt, 2000000000u);
}
```

Create `test/bookorbit_exchange_policy/CMakeLists.txt`:

```cmake
add_executable(BookOrbitExchangePolicyTest
  BookOrbitExchangePolicyTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangePolicy.cpp
)

target_include_directories(BookOrbitExchangePolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitExchangePolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangePolicyTest)
```

Add `add_subdirectory(bookorbit_exchange_policy)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitExchangePolicy.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitExchangePolicy.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// Six hours, matching BookOrbitAnnotations.EXCHANGE_MAX_AGE. The exchange is
// the only channel that delivers server-created annotations, so an unchanged
// book may skip cheaply but never indefinitely.
inline constexpr uint32_t kExchangeMaxAgeSeconds = 6 * 3600;

// True when this book's local set is byte-identical to the set last fully
// exchanged, and that exchange is recent enough to trust.
bool canSkipExchange(const char* storedSignature, uint32_t exchangedAt, std::string_view signature,
                     uint32_t nowUnix);

// Records a complete exchange. Returns false — writing nothing — when the
// signature does not fit the destination field, because a truncated signature
// would compare equal to a different set and park the book forever.
bool rememberExchanged(char* storedSignature, size_t capacity, uint32_t& exchangedAt, std::string_view signature,
                       uint32_t nowUnix);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitExchangePolicy.cpp`:

```cpp
#include "BookOrbitExchangePolicy.h"

#include <cstring>

namespace bookorbit {

bool canSkipExchange(const char* storedSignature, const uint32_t exchangedAt, const std::string_view signature,
                     const uint32_t nowUnix) {
  if (storedSignature == nullptr || signature.empty() || exchangedAt == 0) return false;
  if (std::strlen(storedSignature) != signature.size()) return false;
  if (std::memcmp(storedSignature, signature.data(), signature.size()) != 0) return false;
  if (exchangedAt > nowUnix) return false;  // clock went backwards
  return (nowUnix - exchangedAt) <= kExchangeMaxAgeSeconds;
}

bool rememberExchanged(char* storedSignature, const size_t capacity, uint32_t& exchangedAt,
                       const std::string_view signature, const uint32_t nowUnix) {
  if (storedSignature == nullptr || signature.empty()) return false;
  if (signature.size() + 1 > capacity) return false;
  std::memcpy(storedSignature, signature.data(), signature.size());
  storedSignature[signature.size()] = '\0';
  exchangedAt = nowUnix;
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitExchangePolicy --output-on-failure
```

Expected: 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitExchangePolicy.h lib/BookOrbit/BookOrbitExchangePolicy.cpp test/bookorbit_exchange_policy test/CMakeLists.txt
git commit -m "feat: add BookOrbit exchange skip policy"
```

---

### Task 5: Exchange request encoding

Leg 1 of the three-legged exchange: `POST /koreader/plugin/annotations/exchange` and `POST /koreader/plugin/bookmarks/exchange` with `{books:[{hash, keys, keysComplete, changes}]}`.

**Files:**
- Create: `lib/BookOrbit/BookOrbitExchangeRequest.h`, `lib/BookOrbit/BookOrbitExchangeRequest.cpp`
- Create: `test/bookorbit_exchange_request/CMakeLists.txt`, `test/bookorbit_exchange_request/BookOrbitExchangeRequestTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::jsonEscape` (P0, `BookOrbitMatch.h`); `Annotation`, `AnnotationKey` (Task 2); `Bookmark`, `BookmarkKey` (Task 3).
- Produces:
  `inline constexpr size_t bookorbit::kUploadChunk = 50;`
  `inline constexpr size_t bookorbit::kMaxPullRounds = 10;`
  `inline constexpr size_t bookorbit::kMaxAnnotationKeysPerBook = 5000;`
  `inline constexpr size_t bookorbit::kMaxBookmarkKeysPerBook = 500;`
  `inline constexpr char bookorbit::kAnnotationExchangePath[]`, `kAnnotationAckPath[]`, `kBookmarkExchangePath[]`, `kBookmarkAckPath[]`;
  `std::string bookorbit::encodeAnnotationExchange(std::string_view hash, const std::vector<AnnotationKey>& keys, bool keysComplete, const std::vector<Annotation>& changes)`;
  `std::string bookorbit::encodeBookmarkExchange(std::string_view hash, const std::vector<BookmarkKey>& keys, bool keysComplete, const std::vector<Bookmark>& changes)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_exchange_request/BookOrbitExchangeRequestTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitExchangeRequest.h"

using bookorbit::Annotation;
using bookorbit::AnnotationKey;
using bookorbit::Bookmark;
using bookorbit::BookmarkKey;
using bookorbit::encodeAnnotationExchange;
using bookorbit::encodeBookmarkExchange;

namespace {

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr char kPos[] = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17";

Annotation highlight() {
  Annotation entry;
  entry.datetime = "2026-09-11 14:03:00";
  entry.datetimeUpdated = "2026-09-11 15:00:00";
  entry.drawer = "lighten";
  entry.color = "yellow";
  entry.text = "a highlight";
  entry.note = "a note";
  entry.chapter = "Chapter Three";
  entry.pageno = 42;
  entry.posFormat = "xpointer";
  entry.pos0 = kPos;
  entry.pos1 = kPos;
  return entry;
}

Bookmark dogear() {
  Bookmark entry;
  entry.datetime = "2026-09-01 08:00:00";
  entry.pos = kPos;
  entry.chapter = "Chapter Three";
  entry.note = "start here";
  entry.pageno = 7;
  return entry;
}

}  // namespace

TEST(BookOrbitExchangeRequest, WrapsOneBookInABooksArray) {
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {});
  EXPECT_EQ(json.rfind(R"({"books":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f")", 0), 0u);
  EXPECT_EQ(json.substr(json.size() - 3), "}]}");
}

TEST(BookOrbitExchangeRequest, EncodesKeysAsKAndDt) {
  const std::vector<AnnotationKey> keys = {{"08759494897afa79aec0d37d83498d30", "2026-09-11 14:03:00"}};
  const std::string json = encodeAnnotationExchange(kHash, keys, true, {});
  EXPECT_NE(json.find(R"("keys":[{"k":"08759494897afa79aec0d37d83498d30","dt":"2026-09-11 14:03:00"}])"),
            std::string::npos);
  EXPECT_NE(json.find(R"("keysComplete":true)"), std::string::npos);
}

// keysComplete false means "this is not my entire key set". Sending a partial
// set with it would invite the server to delete what it cannot see.
TEST(BookOrbitExchangeRequest, IncompleteKeySetOmitsKeysEntirely) {
  const std::vector<AnnotationKey> keys = {{"abc", "2026-09-11 14:03:00"}};
  const std::string json = encodeAnnotationExchange(kHash, keys, false, {});
  EXPECT_NE(json.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(json.find(R"("k":"abc")"), std::string::npos);
  EXPECT_NE(json.find(R"("keys":[])"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesEveryAnnotationField) {
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {highlight()});
  EXPECT_NE(json.find(R"("datetime":"2026-09-11 14:03:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("datetimeUpdated":"2026-09-11 15:00:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("drawer":"lighten")"), std::string::npos);
  EXPECT_NE(json.find(R"("color":"yellow")"), std::string::npos);
  EXPECT_NE(json.find(R"("text":"a highlight")"), std::string::npos);
  EXPECT_NE(json.find(R"("note":"a note")"), std::string::npos);
  EXPECT_NE(json.find(R"("chapter":"Chapter Three")"), std::string::npos);
  EXPECT_NE(json.find(R"("pageno":42)"), std::string::npos);
  EXPECT_NE(json.find(R"("posFormat":"xpointer")"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos0":")") + kPos + '"'), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos1":")") + kPos + '"'), std::string::npos);
}

TEST(BookOrbitExchangeRequest, OmitsEmptyOptionalFields) {
  Annotation bare = highlight();
  bare.datetimeUpdated.clear();
  bare.color.clear();
  bare.note.clear();
  bare.chapter.clear();
  bare.pageno = -1;

  const std::string json = encodeAnnotationExchange(kHash, {}, true, {bare});
  EXPECT_EQ(json.find(R"("datetimeUpdated")"), std::string::npos);
  EXPECT_EQ(json.find(R"("color")"), std::string::npos);
  EXPECT_EQ(json.find(R"("note")"), std::string::npos);
  EXPECT_EQ(json.find(R"("chapter")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pageno")"), std::string::npos);
  // The required identity fields survive.
  EXPECT_NE(json.find(R"("datetime":"2026-09-11 14:03:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("drawer":"lighten")"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EscapesQuotesAndNewlinesInText) {
  Annotation quoted = highlight();
  quoted.text = "he said \"no\"\nand left";
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {quoted});
  EXPECT_NE(json.find(R"(he said \"no\"\nand left)"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesSeveralChangesInOrder) {
  Annotation second = highlight();
  second.datetime = "2026-09-12 10:00:00";
  const std::string json = encodeAnnotationExchange(kHash, {}, true, {highlight(), second});
  const size_t first = json.find(R"("datetime":"2026-09-11 14:03:00")");
  const size_t next = json.find(R"("datetime":"2026-09-12 10:00:00")");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(next, std::string::npos);
  EXPECT_LT(first, next);
}

TEST(BookOrbitExchangeRequest, EmptyChangesEncodeAsAnEmptyArray) {
  EXPECT_NE(encodeAnnotationExchange(kHash, {}, true, {}).find(R"("changes":[])"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, EncodesBookmarkFields) {
  const std::vector<BookmarkKey> keys = {{"deadbeef", "2026-09-01 08:00:00"}};
  const std::string json = encodeBookmarkExchange(kHash, keys, true, {dogear()});
  EXPECT_NE(json.find(R"("datetime":"2026-09-01 08:00:00")"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("pos":")") + kPos + '"'), std::string::npos);
  EXPECT_NE(json.find(R"("pageno":7)"), std::string::npos);
  EXPECT_NE(json.find(R"("chapter":"Chapter Three")"), std::string::npos);
  EXPECT_NE(json.find(R"("note":"start here")"), std::string::npos);
  EXPECT_NE(json.find(R"("k":"deadbeef")"), std::string::npos);
}

// A bookmark has no drawer, colour, text or pos1. Sending them would make the
// server treat a dogear as a highlight.
TEST(BookOrbitExchangeRequest, BookmarkEncodingCarriesNoHighlightFields) {
  const std::string json = encodeBookmarkExchange(kHash, {}, true, {dogear()});
  EXPECT_EQ(json.find(R"("drawer")"), std::string::npos);
  EXPECT_EQ(json.find(R"("color")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pos0")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pos1")"), std::string::npos);
  EXPECT_EQ(json.find(R"("posFormat")"), std::string::npos);
}

TEST(BookOrbitExchangeRequest, CapsAreTheSpecValues) {
  EXPECT_EQ(bookorbit::kUploadChunk, 50u);
  EXPECT_EQ(bookorbit::kMaxPullRounds, 10u);
  EXPECT_EQ(bookorbit::kMaxAnnotationKeysPerBook, 5000u);
  EXPECT_EQ(bookorbit::kMaxBookmarkKeysPerBook, 500u);
}

TEST(BookOrbitExchangeRequest, PathsAreTheSpecRoutes) {
  EXPECT_STREQ(bookorbit::kAnnotationExchangePath, "/koreader/plugin/annotations/exchange");
  EXPECT_STREQ(bookorbit::kAnnotationAckPath, "/koreader/plugin/annotations/exchange-ack");
  EXPECT_STREQ(bookorbit::kBookmarkExchangePath, "/koreader/plugin/bookmarks/exchange");
  EXPECT_STREQ(bookorbit::kBookmarkAckPath, "/koreader/plugin/bookmarks/exchange-ack");
}
```

Create `test/bookorbit_exchange_request/CMakeLists.txt`:

```cmake
add_executable(BookOrbitExchangeRequestTest
  BookOrbitExchangeRequestTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeRequest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitExchangeRequestTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitExchangeRequestTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangeRequestTest)
```

Add `add_subdirectory(bookorbit_exchange_request)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitExchangeRequest.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitExchangeRequest.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitBookmarkModel.h"

namespace bookorbit {

// Limits, verbatim from the spec's P4 section.
inline constexpr size_t kUploadChunk = 50;
inline constexpr size_t kMaxPullRounds = 10;
inline constexpr size_t kMaxAnnotationKeysPerBook = 5000;
inline constexpr size_t kMaxBookmarkKeysPerBook = 500;

inline constexpr char kAnnotationExchangePath[] = "/koreader/plugin/annotations/exchange";
inline constexpr char kAnnotationAckPath[] = "/koreader/plugin/annotations/exchange-ack";
inline constexpr char kBookmarkExchangePath[] = "/koreader/plugin/bookmarks/exchange";
inline constexpr char kBookmarkAckPath[] = "/koreader/plugin/bookmarks/exchange-ack";

// Leg 1 of the exchange. keysComplete asserts "these are ALL my local keys",
// which is what lets the server detect entries deleted on-device. When the key
// set is over the cap, pass keysComplete=false: the keys array is then emitted
// empty and the server skips deletion detection for this book rather than
// deleting what it cannot see.
std::string encodeAnnotationExchange(std::string_view hash, const std::vector<AnnotationKey>& keys,
                                     bool keysComplete, const std::vector<Annotation>& changes);

std::string encodeBookmarkExchange(std::string_view hash, const std::vector<BookmarkKey>& keys, bool keysComplete,
                                   const std::vector<Bookmark>& changes);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitExchangeRequest.cpp`:

```cpp
#include "BookOrbitExchangeRequest.h"

#include <string>

#include "BookOrbitMatch.h"  // jsonEscape

namespace bookorbit {
namespace {

void appendString(std::string& json, const char* name, const std::string_view value, const bool comma) {
  if (comma) json += ',';
  json += '"';
  json += name;
  json += "\":\"";
  json += jsonEscape(value);
  json += '"';
}

// Optional fields are omitted rather than sent empty: the server treats an
// absent note differently from a cleared one.
void appendOptionalString(std::string& json, const char* name, const std::string& value) {
  if (value.empty()) return;
  appendString(json, name, value, true);
}

void appendOptionalPageno(std::string& json, const int32_t pageno) {
  if (pageno < 0) return;
  json += ",\"pageno\":";
  json += std::to_string(pageno);
}

template <typename KeyT>
void appendKeys(std::string& json, const std::vector<KeyT>& keys, const bool keysComplete) {
  json += ",\"keys\":[";
  if (keysComplete) {
    for (size_t i = 0; i < keys.size(); i++) {
      if (i > 0) json += ',';
      json += R"({"k":")";
      json += jsonEscape(keys[i].k);
      json += R"(","dt":")";
      json += jsonEscape(keys[i].dt);
      json += "\"}";
    }
  }
  json += "],\"keysComplete\":";
  json += keysComplete ? "true" : "false";
}

std::string openBook(const std::string_view hash) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += '"';
  return json;
}

}  // namespace

std::string encodeAnnotationExchange(const std::string_view hash, const std::vector<AnnotationKey>& keys,
                                     const bool keysComplete, const std::vector<Annotation>& changes) {
  std::string json = openBook(hash);
  appendKeys(json, keys, keysComplete);
  json += ",\"changes\":[";
  for (size_t i = 0; i < changes.size(); i++) {
    const auto& entry = changes[i];
    if (i > 0) json += ',';
    json += '{';
    appendString(json, "datetime", entry.datetime, false);
    appendOptionalString(json, "datetimeUpdated", entry.datetimeUpdated);
    appendString(json, "drawer", entry.drawer, true);
    appendOptionalString(json, "color", entry.color);
    appendOptionalString(json, "text", entry.text);
    appendOptionalString(json, "note", entry.note);
    appendOptionalString(json, "chapter", entry.chapter);
    appendOptionalPageno(json, entry.pageno);
    appendString(json, "posFormat", entry.posFormat.empty() ? std::string_view("xpointer") : entry.posFormat, true);
    appendString(json, "pos0", entry.pos0, true);
    appendOptionalString(json, "pos1", entry.pos1);
    json += '}';
  }
  json += "]}]}";
  return json;
}

std::string encodeBookmarkExchange(const std::string_view hash, const std::vector<BookmarkKey>& keys,
                                   const bool keysComplete, const std::vector<Bookmark>& changes) {
  std::string json = openBook(hash);
  appendKeys(json, keys, keysComplete);
  json += ",\"changes\":[";
  for (size_t i = 0; i < changes.size(); i++) {
    const auto& entry = changes[i];
    if (i > 0) json += ',';
    json += '{';
    appendString(json, "datetime", entry.datetime, false);
    appendOptionalString(json, "datetimeUpdated", entry.datetimeUpdated);
    appendString(json, "pos", entry.pos, true);
    appendOptionalPageno(json, entry.pageno);
    appendOptionalString(json, "chapter", entry.chapter);
    appendOptionalString(json, "note", entry.note);
    json += '}';
  }
  json += "]}]}";
  return json;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitExchangeRequest --output-on-failure
```

Expected: 12 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitExchangeRequest.h lib/BookOrbit/BookOrbitExchangeRequest.cpp test/bookorbit_exchange_request test/CMakeLists.txt
git commit -m "feat: add BookOrbit exchange request encoding"
```

---

### Task 6: Exchange response decoding

Leg 2: `{unmatched:[hash], results:[{hash, toApply:{add:[…], delete:[…]}, more:bool}]}`. Decoded through `StreamingJsonParser` (C callbacks over a `void* ctx`, 512-byte token buffer, 32 nesting levels) so a book with thousands of server-side changes never buffers a DOM.

**Files:**
- Create: `lib/BookOrbit/BookOrbitExchangeResponse.h`, `lib/BookOrbit/BookOrbitExchangeResponse.cpp`
- Create: `test/bookorbit_exchange_response/CMakeLists.txt`, `test/bookorbit_exchange_response/BookOrbitExchangeResponseTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamingJsonParser` (`lib/JsonParser/StreamingJsonParser.h`).
- Produces:
  `struct bookorbit::RemoteEntry { std::string serverId, key, datetime, datetimeUpdated, drawer, color, text, note, chapter, title, posFormat, pos0, pos1; int32_t pageno; }`;
  `struct bookorbit::ExchangeBookResult { std::string hash; std::vector<RemoteEntry> add, remove; bool more; }`;
  `struct bookorbit::ExchangeResponse { std::vector<std::string> unmatched; std::vector<ExchangeBookResult> results; }`;
  `bool bookorbit::decodeExchangeResponse(std::string_view json, ExchangeResponse& out)`.

`delete` is a C++ keyword, so the vector is named `remove`. A bookmark entry's `pos` decodes into `RemoteEntry::pos0` — one struct serves both routes.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_exchange_response/BookOrbitExchangeResponseTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitExchangeResponse.h"

using bookorbit::decodeExchangeResponse;
using bookorbit::ExchangeResponse;

TEST(BookOrbitExchangeResponse, DecodesAnEmptyResult) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"unmatched":[],"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false}]})", response));
  EXPECT_TRUE(response.unmatched.empty());
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_EQ(response.results[0].hash, "abc");
  EXPECT_TRUE(response.results[0].add.empty());
  EXPECT_TRUE(response.results[0].remove.empty());
  EXPECT_FALSE(response.results[0].more);
}

TEST(BookOrbitExchangeResponse, DecodesUnmatchedHashes) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":["abc","def"],"results":[]})", response));
  ASSERT_EQ(response.unmatched.size(), 2u);
  EXPECT_EQ(response.unmatched[0], "abc");
  EXPECT_EQ(response.unmatched[1], "def");
}

TEST(BookOrbitExchangeResponse, DecodesAnAnnotationToAdd) {
  const std::string body = R"({
    "unmatched": [],
    "results": [{
      "hash": "abc",
      "toApply": {
        "add": [{
          "serverId": 4711,
          "datetime": "2026-09-11 14:03:00",
          "datetimeUpdated": "2026-09-11 15:00:00",
          "drawer": "underscore",
          "color": "yellow",
          "text": "from the web",
          "note": "a note",
          "chapter": "Chapter Three",
          "pageno": 42,
          "posFormat": "xpointer",
          "pos0": "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0",
          "pos1": "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].31"
        }],
        "delete": []
      },
      "more": false
    }]
  })";

  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(body, response));
  ASSERT_EQ(response.results.size(), 1u);
  ASSERT_EQ(response.results[0].add.size(), 1u);
  const auto& entry = response.results[0].add[0];
  EXPECT_EQ(entry.serverId, "4711");
  EXPECT_EQ(entry.datetime, "2026-09-11 14:03:00");
  EXPECT_EQ(entry.datetimeUpdated, "2026-09-11 15:00:00");
  EXPECT_EQ(entry.drawer, "underscore");
  EXPECT_EQ(entry.color, "yellow");
  EXPECT_EQ(entry.text, "from the web");
  EXPECT_EQ(entry.note, "a note");
  EXPECT_EQ(entry.chapter, "Chapter Three");
  EXPECT_EQ(entry.pageno, 42);
  EXPECT_EQ(entry.posFormat, "xpointer");
  EXPECT_EQ(entry.pos0, "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0");
  EXPECT_EQ(entry.pos1, "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].31");
}

TEST(BookOrbitExchangeResponse, DecodesDeletesByServerIdKeyAndDatetime) {
  const std::string body = R"({
    "unmatched": [],
    "results": [{
      "hash": "abc",
      "toApply": {
        "add": [],
        "delete": [{"serverId": "9", "key": "08759494897afa79aec0d37d83498d30", "datetime": "2026-09-11 14:03:00"}]
      },
      "more": true
    }]
  })";

  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(body, response));
  ASSERT_EQ(response.results[0].remove.size(), 1u);
  EXPECT_EQ(response.results[0].remove[0].serverId, "9");
  EXPECT_EQ(response.results[0].remove[0].key, "08759494897afa79aec0d37d83498d30");
  EXPECT_EQ(response.results[0].remove[0].datetime, "2026-09-11 14:03:00");
  EXPECT_TRUE(response.results[0].more);
}

// The server may send serverId as a JSON number or a string; both must land in
// the same field, because the ack has to echo it back verbatim.
TEST(BookOrbitExchangeResponse, AcceptsNumericAndStringServerIds) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[{"serverId":12,"pos0":"/body[1]"},)"
      R"({"serverId":"13","pos0":"/body[1]"}],"delete":[]},"more":false}]})",
      response));
  ASSERT_EQ(response.results[0].add.size(), 2u);
  EXPECT_EQ(response.results[0].add[0].serverId, "12");
  EXPECT_EQ(response.results[0].add[1].serverId, "13");
}

// A bookmark's position arrives as "pos"; one struct serves both routes.
TEST(BookOrbitExchangeResponse, BookmarkPosLandsInPos0) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[{"serverId":1,"pos":"/body[1]/DocFragment[5]",)"
      R"("title":"Chapter 5"}],"delete":[]},"more":false}]})",
      response));
  ASSERT_EQ(response.results[0].add.size(), 1u);
  EXPECT_EQ(response.results[0].add[0].pos0, "/body[1]/DocFragment[5]");
  EXPECT_EQ(response.results[0].add[0].title, "Chapter 5");
}

TEST(BookOrbitExchangeResponse, DecodesSeveralBooks) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false},)"
      R"({"hash":"def","toApply":{"add":[],"delete":[]},"more":true}]})",
      response));
  ASSERT_EQ(response.results.size(), 2u);
  EXPECT_EQ(response.results[0].hash, "abc");
  EXPECT_FALSE(response.results[0].more);
  EXPECT_EQ(response.results[1].hash, "def");
  EXPECT_TRUE(response.results[1].more);
}

TEST(BookOrbitExchangeResponse, MissingSectionsDecodeAsEmpty) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"results":[{"hash":"abc","more":false}]})", response));
  ASSERT_EQ(response.results.size(), 1u);
  EXPECT_TRUE(response.results[0].add.empty());
  EXPECT_TRUE(response.results[0].remove.empty());
}

TEST(BookOrbitExchangeResponse, RejectsMalformedJson) {
  ExchangeResponse response;
  EXPECT_FALSE(decodeExchangeResponse("{not json", response));
}

TEST(BookOrbitExchangeResponse, ClearsPreviousContentBeforeDecoding) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":["stale"],"results":[]})", response));
  ASSERT_TRUE(decodeExchangeResponse(R"({"unmatched":[],"results":[]})", response));
  EXPECT_TRUE(response.unmatched.empty());
}

// An entry with no serverId cannot be acked, so it must not enter the apply
// list — an unackable entry would be re-sent forever.
TEST(BookOrbitExchangeResponse, DropsEntriesWithoutAServerId) {
  ExchangeResponse response;
  ASSERT_TRUE(decodeExchangeResponse(
      R"({"results":[{"hash":"abc","toApply":{"add":[{"pos0":"/body[1]"}],"delete":[]},"more":false}]})", response));
  EXPECT_TRUE(response.results[0].add.empty());
}
```

Create `test/bookorbit_exchange_response/CMakeLists.txt`:

```cmake
add_executable(BookOrbitExchangeResponseTest
  BookOrbitExchangeResponseTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeResponse.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitExchangeResponseTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitExchangeResponseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangeResponseTest)
```

Add `add_subdirectory(bookorbit_exchange_response)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitExchangeResponse.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitExchangeResponse.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One server-side change. The same struct carries an annotation and a bookmark:
// a bookmark's "pos" decodes into pos0, and its label into title.
struct RemoteEntry {
  std::string serverId;  // echoed verbatim in the ack; number or string on the wire
  std::string key;       // md5(datetime|pos) identity, present on deletes
  std::string datetime;
  std::string datetimeUpdated;
  std::string drawer;
  std::string color;
  std::string text;
  std::string note;
  std::string chapter;
  std::string title;
  std::string posFormat;
  std::string pos0;
  std::string pos1;
  int32_t pageno = -1;
};

struct ExchangeBookResult {
  std::string hash;
  std::vector<RemoteEntry> add;
  std::vector<RemoteEntry> remove;  // "delete" on the wire; a C++ keyword here
  bool more = false;                // the server has further changes queued
};

struct ExchangeResponse {
  std::vector<std::string> unmatched;
  std::vector<ExchangeBookResult> results;
};

// Returns false only on malformed JSON. A well-formed response with nothing to
// apply is a success with empty vectors.
bool decodeExchangeResponse(std::string_view json, ExchangeResponse& out);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitExchangeResponse.cpp`:

```cpp
#include "BookOrbitExchangeResponse.h"

#include <cstdlib>
#include <cstring>

#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

enum class Section : uint8_t { None, Unmatched, Add, Delete };

// StreamingJsonParser is a C-callback tokenizer over a void* ctx, so the
// decoder is a small depth-tracking state machine rather than a DOM walk.
struct DecodeCtx {
  ExchangeResponse* out = nullptr;
  std::string key;
  Section section = Section::None;
  bool inResults = false;
  int arrayDepth = 0;
  int sectionArrayDepth = 0;
  int resultsArrayDepth = 0;
  int objectDepth = 0;
  int resultObjectDepth = 0;
  ExchangeBookResult result;
  RemoteEntry entry;
};

void assignEntryString(RemoteEntry& entry, const std::string& key, const char* value, const size_t len) {
  const std::string text(value, len);
  if (key == "serverId") entry.serverId = text;
  else if (key == "key") entry.key = text;
  else if (key == "datetime") entry.datetime = text;
  else if (key == "datetimeUpdated") entry.datetimeUpdated = text;
  else if (key == "drawer") entry.drawer = text;
  else if (key == "color") entry.color = text;
  else if (key == "text") entry.text = text;
  else if (key == "note") entry.note = text;
  else if (key == "chapter") entry.chapter = text;
  else if (key == "title") entry.title = text;
  else if (key == "posFormat") entry.posFormat = text;
  else if (key == "pos0" || key == "pos") entry.pos0 = text;
  else if (key == "pos1") entry.pos1 = text;
}

void onKey(void* raw, const char* key, const size_t len) {
  static_cast<DecodeCtx*>(raw)->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section == Section::Unmatched) {
    ctx->out->unmatched.emplace_back(value, len);
    return;
  }
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    assignEntryString(ctx->entry, ctx->key, value, len);
    return;
  }
  if (ctx->inResults && ctx->key == "hash") {
    ctx->result.hash.assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section != Section::Add && ctx->section != Section::Delete) return;
  const std::string text(value, len);
  if (ctx->key == "serverId") {
    ctx->entry.serverId = text;  // kept as text; the ack echoes it verbatim
  } else if (ctx->key == "pageno") {
    ctx->entry.pageno = static_cast<int32_t>(strtol(text.c_str(), nullptr, 10));
  }
}

void onBool(void* raw, const bool value) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->inResults && ctx->section == Section::None && ctx->key == "more") {
    ctx->result.more = value;
  }
}

void onNull(void*) {}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->arrayDepth++;
  if (ctx->key == "unmatched" && ctx->section == Section::None) {
    ctx->section = Section::Unmatched;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  } else if (ctx->key == "results" && !ctx->inResults) {
    ctx->inResults = true;
    ctx->resultsArrayDepth = ctx->arrayDepth;
  } else if (ctx->inResults && ctx->key == "add") {
    ctx->section = Section::Add;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  } else if (ctx->inResults && ctx->key == "delete") {
    ctx->section = Section::Delete;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  }
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section != Section::None && ctx->arrayDepth == ctx->sectionArrayDepth) {
    ctx->section = Section::None;
    ctx->sectionArrayDepth = 0;
  }
  if (ctx->inResults && ctx->arrayDepth == ctx->resultsArrayDepth) {
    ctx->inResults = false;
    ctx->resultsArrayDepth = 0;
  }
  ctx->arrayDepth--;
  ctx->key.clear();
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->objectDepth++;
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    ctx->entry = RemoteEntry{};
  } else if (ctx->inResults && ctx->resultObjectDepth == 0) {
    ctx->resultObjectDepth = ctx->objectDepth;
    ctx->result = ExchangeBookResult{};
  }
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    // An entry with no serverId cannot be acknowledged, so applying it would
    // guarantee the server re-sends it forever. Drop it here.
    if (!ctx->entry.serverId.empty()) {
      if (ctx->section == Section::Add) {
        ctx->result.add.push_back(ctx->entry);
      } else {
        ctx->result.remove.push_back(ctx->entry);
      }
    }
  } else if (ctx->resultObjectDepth != 0 && ctx->objectDepth == ctx->resultObjectDepth) {
    ctx->out->results.push_back(ctx->result);
    ctx->resultObjectDepth = 0;
  }
  ctx->objectDepth--;
  ctx->key.clear();
}

}  // namespace

bool decodeExchangeResponse(const std::string_view json, ExchangeResponse& out) {
  out.unmatched.clear();
  out.results.clear();

  DecodeCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  // json.data() is not NUL-terminated, but feed() takes an explicit length, so
  // this is safe. Never pass it to a C string API.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.unmatched.clear();
    out.results.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitExchangeResponse --output-on-failure
```

Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitExchangeResponse.h lib/BookOrbit/BookOrbitExchangeResponse.cpp test/bookorbit_exchange_response test/CMakeLists.txt
git commit -m "feat: add BookOrbit exchange response decoding"
```

---

### Task 7: Exchange-ack encoding

Leg 3: `POST …/exchange-ack` with `{books:[{hash, applied:[{serverId,status,key,datetime,pos0}], deleted:[{serverId,status}]}]}`, `status ∈ {"applied","failed"}`. The server marks an entry delivered only when this lands, which is what makes the whole exchange crash-safe.

**Files:**
- Create: `lib/BookOrbit/BookOrbitExchangeAck.h`, `lib/BookOrbit/BookOrbitExchangeAck.cpp`
- Create: `test/bookorbit_exchange_ack/CMakeLists.txt`, `test/bookorbit_exchange_ack/BookOrbitExchangeAckTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::jsonEscape` (P0).
- Produces:
  `struct bookorbit::AppliedAck { std::string serverId, key, datetime, pos0; bool failed; }`;
  `struct bookorbit::DeletedAck { std::string serverId; bool failed; }`;
  `std::string bookorbit::encodeExchangeAck(std::string_view hash, const std::vector<AppliedAck>& applied, const std::vector<DeletedAck>& deleted)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_exchange_ack/BookOrbitExchangeAckTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitExchangeAck.h"

using bookorbit::AppliedAck;
using bookorbit::DeletedAck;
using bookorbit::encodeExchangeAck;

namespace {
constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
}

TEST(BookOrbitExchangeAck, WrapsOneBookInABooksArray) {
  const std::string json = encodeExchangeAck(kHash, {}, {});
  EXPECT_EQ(json, R"({"books":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f","applied":[],"deleted":[]}]})");
}

TEST(BookOrbitExchangeAck, EncodesAnAppliedEntryWithItsLocalIdentity) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.key = "08759494897afa79aec0d37d83498d30";
  ack.datetime = "2026-09-11 14:03:00";
  ack.pos0 = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0";

  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"("serverId":"4711")"), std::string::npos);
  EXPECT_NE(json.find(R"("status":"applied")"), std::string::npos);
  EXPECT_NE(json.find(R"("key":"08759494897afa79aec0d37d83498d30")"), std::string::npos);
  EXPECT_NE(json.find(R"("datetime":"2026-09-11 14:03:00")"), std::string::npos);
  EXPECT_NE(json.find(R"("pos0":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0")"), std::string::npos);
}

// A failed apply must be reported, not silently dropped: the server keeps the
// entry pending so a later sync can retry it.
TEST(BookOrbitExchangeAck, EncodesAFailedEntry) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.failed = true;
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"("status":"failed")"), std::string::npos);
}

// A failed apply has no local identity to report; those fields are omitted
// rather than sent empty.
TEST(BookOrbitExchangeAck, OmitsIdentityFieldsWhenTheyAreUnknown) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.failed = true;
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_EQ(json.find(R"("key")"), std::string::npos);
  EXPECT_EQ(json.find(R"("datetime")"), std::string::npos);
  EXPECT_EQ(json.find(R"("pos0")"), std::string::npos);
}

TEST(BookOrbitExchangeAck, EncodesDeletes) {
  const std::string json = encodeExchangeAck(kHash, {}, {{"9", false}, {"10", true}});
  EXPECT_NE(json.find(R"("deleted":[{"serverId":"9","status":"applied"},{"serverId":"10","status":"failed"}])"),
            std::string::npos);
}

TEST(BookOrbitExchangeAck, EncodesSeveralAppliedEntriesInOrder) {
  AppliedAck first;
  first.serverId = "1";
  AppliedAck second;
  second.serverId = "2";
  const std::string json = encodeExchangeAck(kHash, {first, second}, {});
  EXPECT_LT(json.find(R"("serverId":"1")"), json.find(R"("serverId":"2")"));
}

TEST(BookOrbitExchangeAck, EscapesTheIdentityFields) {
  AppliedAck ack;
  ack.serverId = "4711";
  ack.pos0 = R"(/body[1]/"quoted")";
  const std::string json = encodeExchangeAck(kHash, {ack}, {});
  EXPECT_NE(json.find(R"(/body[1]/\"quoted\")"), std::string::npos);
}
```

Create `test/bookorbit_exchange_ack/CMakeLists.txt`:

```cmake
add_executable(BookOrbitExchangeAckTest
  BookOrbitExchangeAckTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeAck.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitExchangeAckTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitExchangeAckTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitExchangeAckTest)
```

Add `add_subdirectory(bookorbit_exchange_ack)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitExchangeAck.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitExchangeAck.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One acknowledged add/edit. key/datetime/pos0 report the LOCAL identity the
// entry ended up with, so the server can address it in a later delete.
struct AppliedAck {
  std::string serverId;
  std::string key;
  std::string datetime;
  std::string pos0;
  bool failed = false;
};

struct DeletedAck {
  std::string serverId;
  bool failed = false;
};

// Leg 3 of the exchange. The server marks entries delivered only when this
// request succeeds, so an exchange whose ack never lands is re-delivered.
std::string encodeExchangeAck(std::string_view hash, const std::vector<AppliedAck>& applied,
                              const std::vector<DeletedAck>& deleted);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitExchangeAck.cpp`:

```cpp
#include "BookOrbitExchangeAck.h"

#include "BookOrbitMatch.h"  // jsonEscape

namespace bookorbit {
namespace {

void appendOptionalString(std::string& json, const char* name, const std::string& value) {
  if (value.empty()) return;
  json += ",\"";
  json += name;
  json += "\":\"";
  json += jsonEscape(value);
  json += '"';
}

}  // namespace

std::string encodeExchangeAck(const std::string_view hash, const std::vector<AppliedAck>& applied,
                              const std::vector<DeletedAck>& deleted) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += R"(","applied":[)";
  for (size_t i = 0; i < applied.size(); i++) {
    const auto& ack = applied[i];
    if (i > 0) json += ',';
    json += R"({"serverId":")";
    json += jsonEscape(ack.serverId);
    json += R"(","status":")";
    json += ack.failed ? "failed" : "applied";
    json += '"';
    appendOptionalString(json, "key", ack.key);
    appendOptionalString(json, "datetime", ack.datetime);
    appendOptionalString(json, "pos0", ack.pos0);
    json += '}';
  }
  json += R"(],"deleted":[)";
  for (size_t i = 0; i < deleted.size(); i++) {
    if (i > 0) json += ',';
    json += R"({"serverId":")";
    json += jsonEscape(deleted[i].serverId);
    json += R"(","status":")";
    json += deleted[i].failed ? "failed" : "applied";
    json += "\"}";
  }
  json += "]}]}";
  return json;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitExchangeAck --output-on-failure
```

Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitExchangeAck.h lib/BookOrbit/BookOrbitExchangeAck.cpp test/bookorbit_exchange_ack test/CMakeLists.txt
git commit -m "feat: add BookOrbit exchange-ack encoding"
```

---

### Task 8: Annotation exchange state machine

The three legs joined up: skip check → chunked upload → apply → ack → follow-up pull. **Crash safety is the property under test:** the skip stamp is written only after the ack for every round succeeded, so an exchange interrupted between apply and ack re-exchanges on the next trigger and converges — losing nothing and duplicating nothing.

**Files:**
- Create: `lib/BookOrbit/BookOrbitAnnotationSync.h`, `lib/BookOrbit/BookOrbitAnnotationSync.cpp`
- Create: `test/bookorbit_annotation_sync/CMakeLists.txt`, `test/bookorbit_annotation_sync/BookOrbitAnnotationSyncTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `BookOrbitClient`, `Error`, `Status`, `isAuthError` (P0); `BookSyncState` (P0); `canSkipExchange`, `rememberExchanged` (Task 4); `encodeAnnotationExchange`, path constants, caps (Task 5); `decodeExchangeResponse`, `RemoteEntry` (Task 6); `encodeExchangeAck`, `AppliedAck`, `DeletedAck` (Task 7); `collectAnnotationKeys` (Task 2).
- Produces:
  `struct bookorbit::ExchangeOutcome { size_t uploaded, applied, deleted, failed; bool hadErrors, skipped, unmatched; }`;
  `class bookorbit::IAnnotationApplier` with
  `virtual size_t applyAdds(const std::vector<RemoteEntry>& adds, std::vector<AppliedAck>& acks) = 0` and
  `virtual size_t applyDeletes(const std::vector<RemoteEntry>& deletes, std::vector<DeletedAck>& acks) = 0`;
  `Error bookorbit::exchangeAnnotations(BookOrbitClient& client, BookSyncState& book, std::string_view hash, const NormalizedAnnotations& local, IAnnotationApplier& applier, uint32_t nowUnix, ExchangeOutcome& outcome)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_annotation_sync/BookOrbitAnnotationSyncTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitAnnotationSync.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/BookOrbitSyncState.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr uint32_t kNow = 2000000000u;

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  std::vector<bookorbit::HttpResponse> queued;
  bool repeatLast = false;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (cursor < queued.size()) return queued[cursor++];
    if (repeatLast && !queued.empty()) return queued.back();
    return {200, false, R"({"unmatched":[],"results":[{"hash":"abc","toApply":{"add":[],"delete":[]},"more":false}]})"};
  }

  size_t countTo(const std::string& pathSuffix) const {
    size_t count = 0;
    for (const auto& request : sent) {
      if (request.url.size() >= pathSuffix.size() &&
          request.url.compare(request.url.size() - pathSuffix.size(), pathSuffix.size(), pathSuffix) == 0) {
        count++;
      }
    }
    return count;
  }

 private:
  size_t cursor = 0;
};

// Applies by local identity, exactly as the device store does: re-delivering
// an entry that is already present is a no-op, not a duplicate.
class RecordingApplier : public bookorbit::IAnnotationApplier {
 public:
  std::vector<std::string> storedKeys;
  size_t redelivered = 0;
  bool failEverything = false;

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds,
                   std::vector<bookorbit::AppliedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : adds) {
      bookorbit::AppliedAck ack;
      ack.serverId = entry.serverId;
      if (failEverything) {
        ack.failed = true;
        acks.push_back(ack);
        continue;
      }
      const std::string key = bookorbit::buildAnnotationKey(entry.datetime, entry.pos0);
      if (std::find(storedKeys.begin(), storedKeys.end(), key) == storedKeys.end()) {
        storedKeys.push_back(key);
        touched++;
      } else {
        redelivered++;
      }
      ack.key = key;
      ack.datetime = entry.datetime;
      ack.pos0 = entry.pos0;
      acks.push_back(ack);
    }
    return touched;
  }

  size_t applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                      std::vector<bookorbit::DeletedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : deletes) {
      const auto it = std::find(storedKeys.begin(), storedKeys.end(), entry.key);
      if (it != storedKeys.end()) {
        storedKeys.erase(it);
        touched++;
      }
      acks.push_back({entry.serverId, false});
    }
    return touched;
  }
};

bookorbit::BookOrbitClient makeClient(ScriptedTransport& transport) {
  return bookorbit::BookOrbitClient(transport, "https://books.example.com/api/v1", "u", "k",
                                    {"crossink-abc123", "Xteink X4 Pro", "0.1.0"});
}

bookorbit::Annotation highlightAt(const int minute) {
  bookorbit::Annotation entry;
  char datetime[24];
  snprintf(datetime, sizeof(datetime), "2026-09-11 14:%02d:00", minute % 60);
  entry.datetime = datetime;
  entry.drawer = "lighten";
  entry.text = "a highlight";
  char pos[96];
  snprintf(pos, sizeof(pos), "/body[1]/DocFragment[3]/body[1]/p[%d]/text()[1].0", minute + 1);
  entry.pos0 = pos;
  entry.pos1 = pos;
  return entry;
}

std::vector<bookorbit::Annotation> highlights(const int count) {
  std::vector<bookorbit::Annotation> entries;
  entries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) entries.push_back(highlightAt(i));
  return entries;
}

std::string oneAddResponse(const bool more) {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[{"serverId":4711,"datetime":"2026-09-11 14:03:00","drawer":"lighten",)"
         R"("text":"from the web","posFormat":"xpointer",)"
         R"("pos0":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].0",)"
         R"("pos1":"/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].9"}],"delete":[]},"more":)" +
         (more ? "true" : "false") + "}]}";
}

std::string emptyResponse() {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[],"delete":[]},"more":false}]})";
}

}  // namespace

using bookorbit::ExchangeOutcome;
using bookorbit::exchangeAnnotations;
using bookorbit::normalizeAnnotations;
using bookorbit::Status;

TEST(BookOrbitAnnotationSync, UnchangedBookSkipsTheExchangeEntirely) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(3));
  snprintf(book.annSignature, sizeof(book.annSignature), "%s", local.signature.c_str());
  book.annExchangedAt = kNow - 60u;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitAnnotationSync, ChangedBookUploadsInChunksOfFifty) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(120));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 3u);
  EXPECT_EQ(outcome.uploaded, 120u);
}

// The key set is the deletion-detection ground truth; it belongs on the first
// request only, and later chunks must not claim to be complete.
TEST(BookOrbitAnnotationSync, OnlyTheFirstRequestCarriesTheKeySet) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(60));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_EQ(transport.sent.size(), 2u);
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":true)"), std::string::npos);
  EXPECT_NE(transport.sent[0].body.find(R"("k":")"), std::string::npos);
  EXPECT_NE(transport.sent[1].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(transport.sent[1].body.find(R"("k":")"), std::string::npos);
}

// Over the 5000 cap, deletion detection degrades gracefully instead of letting
// the server delete entries it cannot see.
TEST(BookOrbitAnnotationSync, OverTheKeyCapSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(5001));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(transport.sent[0].body.find(R"("k":")"), std::string::npos);
  // An incomplete key set means the exchange was not authoritative, so the
  // book must not be stamped as skippable.
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, AppliesServerAddsAndAcknowledgesThem) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(outcome.applied, 1u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  ASSERT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 1u);
  EXPECT_NE(transport.sent[1].body.find(R"("serverId":"4711")"), std::string::npos);
  EXPECT_NE(transport.sent[1].body.find(R"("status":"applied")"), std::string::npos);
}

TEST(BookOrbitAnnotationSync, CleanExchangeStampsTheSkipSignature) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(2));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_STREQ(book.annSignature, local.signature.c_str());
  EXPECT_EQ(book.annExchangedAt, kNow);
}

// THE CRASH-SAFETY CASE. The entries were applied locally but the ack never
// landed, so the server still owes them. Nothing may be stamped, and the next
// exchange must re-apply without duplicating.
TEST(BookOrbitAnnotationSync, AckFailureLeavesTheBookUnstamped) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {0, true, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.annSignature[0], '\0');
  EXPECT_EQ(book.annExchangedAt, 0u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
}

TEST(BookOrbitAnnotationSync, ReExchangeAfterALostAckDoesNotDuplicate) {
  ScriptedTransport first;
  first.queued = {{200, false, oneAddResponse(false)}, {0, true, ""}};
  auto firstClient = makeClient(first);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome lost;
  exchangeAnnotations(firstClient, book, kHash, local, applier, kNow, lost);
  ASSERT_EQ(applier.storedKeys.size(), 1u);

  // The server re-sends the same entry because it was never acknowledged.
  ScriptedTransport second;
  second.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto secondClient = makeClient(second);

  ExchangeOutcome retry;
  ASSERT_EQ(exchangeAnnotations(secondClient, book, kHash, local, applier, kNow + 5u, retry).status, Status::Ok);
  EXPECT_EQ(applier.storedKeys.size(), 1u) << "re-delivery created a duplicate annotation";
  EXPECT_EQ(applier.redelivered, 1u);
  EXPECT_STREQ(book.annSignature, local.signature.c_str());
}

TEST(BookOrbitAnnotationSync, KeepsPullingWhileTheServerReportsMore) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(true)}, {200, false, "{}"},
                      {200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 2u);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 2u);
}

// A server stuck on more:true must not pin the reader in a loop.
TEST(BookOrbitAnnotationSync, StopsAfterTenPullRounds) {
  ScriptedTransport transport;
  transport.repeatLast = true;
  transport.queued = {{200, false, oneAddResponse(true)}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations({});

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange-ack"), 10u);
  // The pull did not complete, so the book stays mandatory next time.
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, UnmatchedBookAbortsWithoutStamping) {
  ScriptedTransport transport;
  transport.queued = {{200, false, std::string(R"({"unmatched":[")") + kHash + R"("],"results":[]})"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Ok);
  EXPECT_TRUE(outcome.unmatched);
  EXPECT_EQ(book.annSignature[0], '\0');
  EXPECT_EQ(transport.sent.size(), 1u);
}

TEST(BookOrbitAnnotationSync, AuthErrorAbortsImmediately) {
  ScriptedTransport transport;
  transport.queued = {{401, false, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::Unauthorized);
  EXPECT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, ServerErrorLeavesTheWatermarkUnadvanced) {
  ScriptedTransport transport;
  transport.queued = {{503, false, ""}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::ServerError);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.annSignature[0], '\0');
}

// A failed apply means the server still owes us the entry; stamping would hide
// it until the 6-hour bound expired.
TEST(BookOrbitAnnotationSync, FailedApplyPreventsStamping) {
  ScriptedTransport transport;
  transport.queued = {{200, false, oneAddResponse(false)}, {200, false, "{}"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  applier.failEverything = true;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome);
  EXPECT_EQ(outcome.failed, 1u);
  EXPECT_NE(transport.sent[1].body.find(R"("status":"failed")"), std::string::npos);
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitAnnotationSync, MalformedResponseIsAnError) {
  ScriptedTransport transport;
  transport.queued = {{200, false, "{not json"}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeAnnotations(highlights(1));

  ExchangeOutcome outcome;
  EXPECT_EQ(exchangeAnnotations(client, book, kHash, local, applier, kNow, outcome).status, Status::InvalidJson);
  EXPECT_EQ(book.annSignature[0], '\0');
}

// An empty local set still exchanges once: it is the only way server-created
// highlights reach the device.
TEST(BookOrbitAnnotationSync, EmptyLocalSetStillExchangesOnce) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(exchangeAnnotations(client, book, kHash, normalizeAnnotations({}), applier, kNow, outcome).status,
            Status::Ok);
  EXPECT_EQ(transport.countTo("/koreader/plugin/annotations/exchange"), 1u);
}
```

Create `test/bookorbit_annotation_sync/CMakeLists.txt`:

```cmake
add_executable(BookOrbitAnnotationSyncTest
  BookOrbitAnnotationSyncTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationSync.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangePolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeRequest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeResponse.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeAck.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitAnnotationSyncTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitAnnotationSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitAnnotationSyncTest)
```

Add `add_subdirectory(bookorbit_annotation_sync)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitAnnotationSync.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitAnnotationSync.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitClient.h"
#include "BookOrbitExchangeAck.h"
#include "BookOrbitExchangeResponse.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

struct ExchangeOutcome {
  size_t uploaded = 0;
  size_t applied = 0;
  size_t deleted = 0;
  size_t failed = 0;
  bool hadErrors = false;
  bool skipped = false;    // signature unchanged; nothing was sent
  bool unmatched = false;  // the server does not know this book
};

// Applies server-side changes to whatever holds the local set — the open
// reader, or the on-disk stores when the book is closed. Injected so the
// exchange state machine is host-testable; a std::function would cost more
// than a vtable here and library code avoids it.
class IAnnotationApplier {
 public:
  virtual ~IAnnotationApplier() = default;

  // Returns how many local entries were actually touched. Every entry must
  // produce exactly one ack, failed or not.
  virtual size_t applyAdds(const std::vector<RemoteEntry>& adds, std::vector<AppliedAck>& acks) = 0;
  virtual size_t applyDeletes(const std::vector<RemoteEntry>& deletes, std::vector<DeletedAck>& acks) = 0;
};

// Runs the full three-legged exchange for one book.
//
// Returns the transport-level Error. Status::Ok covers "skipped", "unmatched"
// and "applied everything" — read `outcome` for which. Status::Unauthorized
// aborts the whole sync at the caller's level; every other error leaves the
// book's skip signature unstamped so the next trigger retries. There are no
// retry loops here, deliberately: on a battery device a failed phase waits for
// the next sync trigger.
Error exchangeAnnotations(BookOrbitClient& client, BookSyncState& book, std::string_view hash,
                          const NormalizedAnnotations& local, IAnnotationApplier& applier, uint32_t nowUnix,
                          ExchangeOutcome& outcome);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitAnnotationSync.cpp`:

```cpp
#include "BookOrbitAnnotationSync.h"

#include <Logging.h>

#include <string>

#include "BookOrbitExchangePolicy.h"
#include "BookOrbitExchangeRequest.h"

namespace bookorbit {
namespace {

const ExchangeBookResult* findResult(const ExchangeResponse& response, const std::string_view hash) {
  for (const auto& result : response.results) {
    if (result.hash == hash) return &result;
  }
  // Single-book requests: some servers echo no hash at all.
  return response.results.empty() ? nullptr : &response.results.front();
}

bool mentionsHash(const std::vector<std::string>& hashes, const std::string_view hash) {
  for (const auto& candidate : hashes) {
    if (candidate == hash) return true;
  }
  return false;
}

bool hasPending(const ExchangeBookResult& result) { return !result.add.empty() || !result.remove.empty(); }

}  // namespace

Error exchangeAnnotations(BookOrbitClient& client, BookSyncState& book, const std::string_view hash,
                          const NormalizedAnnotations& local, IAnnotationApplier& applier, const uint32_t nowUnix,
                          ExchangeOutcome& outcome) {
  outcome = ExchangeOutcome{};

  if (canSkipExchange(book.annSignature, book.annExchangedAt, local.signature, nowUnix)) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  const std::vector<AnnotationKey> keys = collectAnnotationKeys(local.entries);
  const bool keysComplete = keys.size() <= kMaxAnnotationKeysPerBook;

  ExchangeResponse response;
  ExchangeBookResult pending;

  bool firstRequest = true;
  size_t cursor = 0;

  // One pass even with nothing to upload: the exchange is the only channel
  // that delivers server-created highlights.
  do {
    std::vector<Annotation> chunk;
    chunk.reserve(kUploadChunk);
    while (cursor < local.entries.size() && chunk.size() < kUploadChunk) {
      chunk.push_back(local.entries[cursor]);
      cursor++;
    }

    const std::string body =
        encodeAnnotationExchange(hash, firstRequest ? keys : std::vector<AnnotationKey>{},
                                 firstRequest && keysComplete, chunk);

    std::string responseBody;
    const Error error = client.postJson(kAnnotationExchangePath, body, responseBody);
    if (error.status != Status::Ok) {
      LOG_ERR("BookOrbit: annotation exchange failed (%d)", error.httpStatus);
      outcome.hadErrors = true;
      // Auth aborts the whole sync at the caller's level; every other error
      // just leaves this book unstamped for the next trigger.
      return error;
    }

    if (!decodeExchangeResponse(responseBody, response)) {
      LOG_ERR("BookOrbit: annotation exchange response was not valid JSON");
      outcome.hadErrors = true;
      return {Status::InvalidJson, 0};
    }

    if (mentionsHash(response.unmatched, hash)) {
      outcome.unmatched = true;
      return {Status::Ok, 0};
    }

    const ExchangeBookResult* result = findResult(response, hash);
    if (result != nullptr) pending = *result;
    outcome.uploaded += chunk.size();
    firstRequest = false;
  } while (cursor < local.entries.size());

  size_t rounds = 0;
  bool pullComplete = true;

  while (hasPending(pending)) {
    if (rounds >= kMaxPullRounds) {
      // A server stuck on more:true must not pin the reader in a loop; the
      // rest arrives on the next sync trigger.
      pullComplete = false;
      break;
    }
    rounds++;

    std::vector<AppliedAck> appliedAcks;
    std::vector<DeletedAck> deletedAcks;
    const size_t addedTouched = applier.applyAdds(pending.add, appliedAcks);
    const size_t deletedTouched = applier.applyDeletes(pending.remove, deletedAcks);
    outcome.applied += addedTouched;
    outcome.deleted += deletedTouched;
    for (const auto& ack : appliedAcks) {
      if (ack.failed) outcome.failed++;
    }

    std::string ackResponse;
    const Error ackError =
        client.postJson(kAnnotationAckPath, encodeExchangeAck(hash, appliedAcks, deletedAcks), ackResponse);
    if (ackError.status != Status::Ok) {
      // The changes are on disk but the server never heard so. Leaving the
      // book unstamped makes it re-exchange; the applier dedupes by identity,
      // so the re-delivery is a no-op rather than a duplicate.
      LOG_ERR("BookOrbit: annotation exchange ack failed (%d)", ackError.httpStatus);
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }

    if (!pending.more) break;

    std::string followUpBody;
    const Error followUp = client.postJson(
        kAnnotationExchangePath, encodeAnnotationExchange(hash, {}, false, {}), followUpBody);
    if (followUp.status != Status::Ok) {
      LOG_ERR("BookOrbit: annotation exchange follow-up failed (%d)", followUp.httpStatus);
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    if (!decodeExchangeResponse(followUpBody, response)) {
      LOG_ERR("BookOrbit: annotation follow-up response was not valid JSON");
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    const ExchangeBookResult* followUpResult = findResult(response, hash);
    pending = (followUpResult != nullptr) ? *followUpResult : ExchangeBookResult{};
  }

  // Stamped only when the complete key set went out and nothing was left
  // undelivered. Anything the server will re-send keeps the next exchange
  // mandatory.
  if (pullComplete && !outcome.hadErrors && outcome.failed == 0 && keysComplete) {
    rememberExchanged(book.annSignature, sizeof(book.annSignature), book.annExchangedAt, local.signature, nowUnix);
  }
  return {Status::Ok, 0};
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitAnnotationSync --output-on-failure
```

Expected: 16 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitAnnotationSync.h lib/BookOrbit/BookOrbitAnnotationSync.cpp test/bookorbit_annotation_sync test/CMakeLists.txt
git commit -m "feat: add BookOrbit annotation exchange state machine"
```

---

### Task 9: Bookmark exchange state machine and `bookmarkSync` gating

The same three legs against `/koreader/plugin/bookmarks/exchange`, plus the capability gate. **This is where the tri-state rule earns its keep:** a 5xx or a dropped connection must leave `bookmarkSync` `Unknown` so the next sync tries again; only a confirmed 404 on the bookmark route itself caches the negative.

**Files:**
- Create: `lib/BookOrbit/BookOrbitBookmarkSync.h`, `lib/BookOrbit/BookOrbitBookmarkSync.cpp`
- Create: `test/bookorbit_bookmark_sync/CMakeLists.txt`, `test/bookorbit_bookmark_sync/BookOrbitBookmarkSyncTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `CapabilityCache`, `Capability` (P0); `BookOrbitClient`, `BookSyncState` (P0); `ExchangeOutcome`, `IAnnotationApplier` (Task 8); `encodeBookmarkExchange`, caps and paths (Task 5); `decodeExchangeResponse` (Task 6); `encodeExchangeAck` (Task 7); `canSkipExchange`, `rememberExchanged` (Task 4); `collectBookmarkKeys` (Task 3).
- Produces:
  `inline constexpr char bookorbit::kBookmarkCapability[] = "bookmarkSync";`
  `Error bookorbit::exchangeBookmarks(BookOrbitClient& client, CapabilityCache& capabilities, BookSyncState& book, std::string_view hash, const NormalizedBookmarks& local, IAnnotationApplier& applier, uint32_t nowUnix, ExchangeOutcome& outcome)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_bookmark_sync/BookOrbitBookmarkSyncTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBookmarkSync.h"
#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/BookOrbitSyncState.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr uint32_t kNow = 2000000000u;

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  std::vector<bookorbit::HttpResponse> queued;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    if (cursor < queued.size()) return queued[cursor++];
    return {200, false, R"({"unmatched":[],"results":[]})"};
  }

 private:
  size_t cursor = 0;
};

class RecordingApplier : public bookorbit::IAnnotationApplier {
 public:
  std::vector<std::string> storedKeys;

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds,
                   std::vector<bookorbit::AppliedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : adds) {
      const std::string key = bookorbit::buildBookmarkKey(entry.datetime, entry.pos0);
      if (std::find(storedKeys.begin(), storedKeys.end(), key) == storedKeys.end()) {
        storedKeys.push_back(key);
        touched++;
      }
      bookorbit::AppliedAck ack;
      ack.serverId = entry.serverId;
      ack.key = key;
      ack.datetime = entry.datetime;
      ack.pos0 = entry.pos0;
      acks.push_back(ack);
    }
    return touched;
  }

  size_t applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                      std::vector<bookorbit::DeletedAck>& acks) override {
    size_t touched = 0;
    for (const auto& entry : deletes) {
      const auto it = std::find(storedKeys.begin(), storedKeys.end(), entry.key);
      if (it != storedKeys.end()) {
        storedKeys.erase(it);
        touched++;
      }
      acks.push_back({entry.serverId, false});
    }
    return touched;
  }
};

bookorbit::BookOrbitClient makeClient(ScriptedTransport& transport) {
  return bookorbit::BookOrbitClient(transport, "https://books.example.com/api/v1", "u", "k",
                                    {"crossink-abc123", "Xteink X4 Pro", "0.1.0"});
}

std::vector<bookorbit::Bookmark> dogears(const int count) {
  std::vector<bookorbit::Bookmark> entries;
  entries.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; i++) {
    bookorbit::Bookmark entry;
    char datetime[24];
    snprintf(datetime, sizeof(datetime), "2026-09-01 08:%02d:00", i % 60);
    entry.datetime = datetime;
    char pos[96];
    snprintf(pos, sizeof(pos), "/body[1]/DocFragment[5]/body[1]/p[%d]/text()[1].0", i + 1);
    entry.pos = pos;
    entries.push_back(entry);
  }
  return entries;
}

std::string emptyResponse() {
  return std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
         R"(","toApply":{"add":[],"delete":[]},"more":false}]})";
}

}  // namespace

using bookorbit::Capability;
using bookorbit::CapabilityCache;
using bookorbit::ExchangeOutcome;
using bookorbit::exchangeBookmarks;
using bookorbit::kBookmarkCapability;
using bookorbit::normalizeBookmarks;
using bookorbit::Status;

TEST(BookOrbitBookmarkSync, ConfirmedUnsupportedServerIsNeverCalled) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  capabilities.markUnsupported(kBookmarkCapability);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(2)), applier, kNow, outcome)
          .status,
      Status::Ok);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

// Unknown is not a negative. A server we have not asked yet still gets tried.
TEST(BookOrbitBookmarkSync, UnknownCapabilityStillAttemptsTheExchange) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  ASSERT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome);
  EXPECT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/bookmarks/exchange");
}

TEST(BookOrbitBookmarkSync, ConfirmedNotFoundDowngradesTheCapability) {
  ScriptedTransport transport;
  transport.queued = {{404, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
          .status,
      Status::NotFound);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unsupported);
}

// THE TRI-STATE INVARIANT. One 500 must never permanently disable bookmark
// sync.
TEST(BookOrbitBookmarkSync, ServerErrorLeavesTheCapabilityUnknown) {
  ScriptedTransport transport;
  transport.queued = {{500, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
          .status,
      Status::ServerError);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, TransportFailureLeavesTheCapabilityUnknown) {
  ScriptedTransport transport;
  transport.queued = {{0, true, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
          .status,
      Status::Transport);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, UnchangedBookSkipsTheExchange) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeBookmarks(dogears(2));
  snprintf(book.bmSignature, sizeof(book.bmSignature), "%s", local.signature.c_str());
  book.bmExchangedAt = kNow - 60u;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, local, applier, kNow, outcome);
  EXPECT_TRUE(outcome.skipped);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitBookmarkSync, UploadsInChunksOfFifty) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}, {200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(60)), applier, kNow, outcome);
  EXPECT_EQ(transport.sent.size(), 2u);
  EXPECT_EQ(outcome.uploaded, 60u);
}

// The bookmark key cap is 500, not the annotation route's 5000.
TEST(BookOrbitBookmarkSync, OverFiveHundredKeysSendsKeysCompleteFalse) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(501)), applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":false)"), std::string::npos);
  EXPECT_EQ(book.bmSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, FiveHundredKeysStillSendsTheCompleteSet) {
  ScriptedTransport transport;
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(500)), applier, kNow, outcome);
  ASSERT_FALSE(transport.sent.empty());
  EXPECT_NE(transport.sent[0].body.find(R"("keysComplete":true)"), std::string::npos);
}

TEST(BookOrbitBookmarkSync, AppliesAddsAndAcknowledgesOnTheBookmarkRoute) {
  ScriptedTransport transport;
  transport.queued = {{200, false,
                       std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
                           R"(","toApply":{"add":[{"serverId":5,"datetime":"2026-09-02 10:00:00",)"
                           R"("pos":"/body[1]/DocFragment[7]/body[1]/p[1]/text()[1].0","title":"Chapter 7"}],)"
                           R"("delete":[]},"more":false}]})"},
                      {200, false, "{}"}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  ASSERT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks({}), applier, kNow, outcome).status,
      Status::Ok);
  EXPECT_EQ(outcome.applied, 1u);
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  EXPECT_EQ(transport.sent[1].url, "https://books.example.com/api/v1/koreader/plugin/bookmarks/exchange-ack");
}

TEST(BookOrbitBookmarkSync, CleanExchangeStampsTheBookmarkSignature) {
  ScriptedTransport transport;
  transport.queued = {{200, false, emptyResponse()}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;
  const auto local = normalizeBookmarks(dogears(2));

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, local, applier, kNow, outcome);
  EXPECT_STREQ(book.bmSignature, local.signature.c_str());
  EXPECT_EQ(book.bmExchangedAt, kNow);
  // The annotation stamp is a separate field and must be untouched.
  EXPECT_EQ(book.annSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, LostAckLeavesTheBookmarkStampUnwritten) {
  ScriptedTransport transport;
  transport.queued = {{200, false,
                       std::string(R"({"unmatched":[],"results":[{"hash":")") + kHash +
                           R"(","toApply":{"add":[{"serverId":5,"datetime":"2026-09-02 10:00:00",)"
                           R"("pos":"/body[1]/DocFragment[7]/body[1]/p[1]/text()[1].0"}],"delete":[]},)"
                           R"("more":false}]})"},
                      {0, true, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks({}), applier, kNow, outcome);
  EXPECT_TRUE(outcome.hadErrors);
  EXPECT_EQ(book.bmSignature[0], '\0');
  EXPECT_EQ(applier.storedKeys.size(), 1u);
  // A lost ack is a transport failure, not a statement about support.
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}

TEST(BookOrbitBookmarkSync, UnmatchedBookAborts) {
  ScriptedTransport transport;
  transport.queued = {{200, false, std::string(R"({"unmatched":[")") + kHash + R"("],"results":[]})"}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome);
  EXPECT_TRUE(outcome.unmatched);
  EXPECT_EQ(book.bmSignature[0], '\0');
}

TEST(BookOrbitBookmarkSync, AuthErrorAbortsAndDoesNotTouchTheCapability) {
  ScriptedTransport transport;
  transport.queued = {{403, false, ""}};
  auto client = makeClient(transport);
  CapabilityCache capabilities;
  RecordingApplier applier;
  bookorbit::BookSyncState book;

  ExchangeOutcome outcome;
  EXPECT_EQ(
      exchangeBookmarks(client, capabilities, book, kHash, normalizeBookmarks(dogears(1)), applier, kNow, outcome)
          .status,
      Status::Unauthorized);
  EXPECT_EQ(capabilities.get(kBookmarkCapability), Capability::Unknown);
}
```

Create `test/bookorbit_bookmark_sync/CMakeLists.txt`:

```cmake
add_executable(BookOrbitBookmarkSyncTest
  BookOrbitBookmarkSyncTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkSync.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationSync.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitAnnotationModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookmarkModel.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangePolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeRequest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeResponse.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitExchangeAck.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMd5.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitBookmarkSyncTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitBookmarkSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBookmarkSyncTest)
```

Add `add_subdirectory(bookorbit_bookmark_sync)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitBookmarkSync.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitBookmarkSync.h`:

```cpp
#pragma once

#include <cstdint>
#include <string_view>

#include "BookOrbitAnnotationSync.h"
#include "BookOrbitBookmarkModel.h"
#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

// The capability name the server advertises from /koreader/plugin/version.
inline constexpr char kBookmarkCapability[] = "bookmarkSync";

// Position-only bookmarks (dogears) over the same three-legged exchange.
//
// Capability handling is tri-state and deliberately asymmetric:
//   Unsupported -> skipped without a request (outcome.skipped)
//   Unknown     -> tried; a new plugin on an old server learns from the 404
//   Supported   -> tried
// Only Status::NotFound on this route calls markUnsupported. A 5xx or a
// transport failure leaves the capability Unknown, because it says nothing
// about whether the server supports bookmarks.
Error exchangeBookmarks(BookOrbitClient& client, CapabilityCache& capabilities, BookSyncState& book,
                        std::string_view hash, const NormalizedBookmarks& local, IAnnotationApplier& applier,
                        uint32_t nowUnix, ExchangeOutcome& outcome);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitBookmarkSync.cpp`:

```cpp
#include "BookOrbitBookmarkSync.h"

#include <Logging.h>

#include <string>
#include <vector>

#include "BookOrbitExchangeAck.h"
#include "BookOrbitExchangePolicy.h"
#include "BookOrbitExchangeRequest.h"
#include "BookOrbitExchangeResponse.h"

namespace bookorbit {
namespace {

const ExchangeBookResult* findResult(const ExchangeResponse& response, const std::string_view hash) {
  for (const auto& result : response.results) {
    if (result.hash == hash) return &result;
  }
  return response.results.empty() ? nullptr : &response.results.front();
}

bool mentionsHash(const std::vector<std::string>& hashes, const std::string_view hash) {
  for (const auto& candidate : hashes) {
    if (candidate == hash) return true;
  }
  return false;
}

bool hasPending(const ExchangeBookResult& result) { return !result.add.empty() || !result.remove.empty(); }

// One place decides what an error means for the capability, so the tri-state
// rule cannot be half-applied.
void noteRouteError(CapabilityCache& capabilities, const Error& error) {
  if (error.status == Status::NotFound) {
    // Definitive: this server has no bookmark route.
    capabilities.markUnsupported(kBookmarkCapability);
    return;
  }
  // Everything else — 5xx, transport, auth — says nothing about support.
  // Caching a negative here is exactly the bug the tri-state design exists to
  // prevent.
}

}  // namespace

Error exchangeBookmarks(BookOrbitClient& client, CapabilityCache& capabilities, BookSyncState& book,
                        const std::string_view hash, const NormalizedBookmarks& local,
                        IAnnotationApplier& applier, const uint32_t nowUnix, ExchangeOutcome& outcome) {
  outcome = ExchangeOutcome{};

  if (capabilities.get(kBookmarkCapability) == Capability::Unsupported) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  if (canSkipExchange(book.bmSignature, book.bmExchangedAt, local.signature, nowUnix)) {
    outcome.skipped = true;
    return {Status::Ok, 0};
  }

  const std::vector<BookmarkKey> keys = collectBookmarkKeys(local.entries);
  const bool keysComplete = keys.size() <= kMaxBookmarkKeysPerBook;

  ExchangeResponse response;
  ExchangeBookResult pending;
  bool firstRequest = true;
  size_t cursor = 0;

  do {
    std::vector<Bookmark> chunk;
    chunk.reserve(kUploadChunk);
    while (cursor < local.entries.size() && chunk.size() < kUploadChunk) {
      chunk.push_back(local.entries[cursor]);
      cursor++;
    }

    const std::string body = encodeBookmarkExchange(
        hash, firstRequest ? keys : std::vector<BookmarkKey>{}, firstRequest && keysComplete, chunk);

    std::string responseBody;
    const Error error = client.postJson(kBookmarkExchangePath, body, responseBody);
    if (error.status != Status::Ok) {
      LOG_ERR("BookOrbit: bookmark exchange failed (%d)", error.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, error);
      return error;
    }

    if (!decodeExchangeResponse(responseBody, response)) {
      LOG_ERR("BookOrbit: bookmark exchange response was not valid JSON");
      outcome.hadErrors = true;
      return {Status::InvalidJson, 0};
    }

    if (mentionsHash(response.unmatched, hash)) {
      outcome.unmatched = true;
      return {Status::Ok, 0};
    }

    const ExchangeBookResult* result = findResult(response, hash);
    if (result != nullptr) pending = *result;
    outcome.uploaded += chunk.size();
    firstRequest = false;
  } while (cursor < local.entries.size());

  size_t rounds = 0;
  bool pullComplete = true;

  while (hasPending(pending)) {
    if (rounds >= kMaxPullRounds) {
      pullComplete = false;
      break;
    }
    rounds++;

    std::vector<AppliedAck> appliedAcks;
    std::vector<DeletedAck> deletedAcks;
    outcome.applied += applier.applyAdds(pending.add, appliedAcks);
    outcome.deleted += applier.applyDeletes(pending.remove, deletedAcks);
    for (const auto& ack : appliedAcks) {
      if (ack.failed) outcome.failed++;
    }

    std::string ackResponse;
    const Error ackError =
        client.postJson(kBookmarkAckPath, encodeExchangeAck(hash, appliedAcks, deletedAcks), ackResponse);
    if (ackError.status != Status::Ok) {
      LOG_ERR("BookOrbit: bookmark exchange ack failed (%d)", ackError.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, ackError);
      pullComplete = false;
      break;
    }

    if (!pending.more) break;

    std::string followUpBody;
    const Error followUp =
        client.postJson(kBookmarkExchangePath, encodeBookmarkExchange(hash, {}, false, {}), followUpBody);
    if (followUp.status != Status::Ok) {
      LOG_ERR("BookOrbit: bookmark exchange follow-up failed (%d)", followUp.httpStatus);
      outcome.hadErrors = true;
      noteRouteError(capabilities, followUp);
      pullComplete = false;
      break;
    }
    if (!decodeExchangeResponse(followUpBody, response)) {
      outcome.hadErrors = true;
      pullComplete = false;
      break;
    }
    const ExchangeBookResult* result = findResult(response, hash);
    pending = (result != nullptr) ? *result : ExchangeBookResult{};
  }

  if (pullComplete && !outcome.hadErrors && outcome.failed == 0 && keysComplete) {
    rememberExchanged(book.bmSignature, sizeof(book.bmSignature), book.bmExchangedAt, local.signature, nowUnix);
  }
  return {Status::Ok, 0};
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitBookmarkSync --output-on-failure
```

Expected: 14 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitBookmarkSync.h lib/BookOrbit/BookOrbitBookmarkSync.cpp test/bookorbit_bookmark_sync test/CMakeLists.txt
git commit -m "feat: add BookOrbit bookmark exchange with capability gating"
```

---

### Task 10: Device adapter over `BookmarkStore` and `ClippingStore`

Maps CrossInk's local records onto the wire shapes and back. Device-only: it touches the store singletons, the open `Epub`, and `ProgressMapper`, none of which build on the host — the protocol logic it feeds is already covered by Tasks 2–9.

**Files:**
- Create: `src/bookorbit/CrossInkAnnotationSource.h`, `src/bookorbit/CrossInkAnnotationSource.cpp`
- Reference: `src/ClippingStore.h` (`Clipping`: `spineIndex`, `startPage`, `endPage`, `pageCount`, `paragraphIndex`, `timestamp`, `chapterTitle`, text read through `readClippingText`)
- Reference: `src/BookmarkStore.h` (`Bookmark`: `spineIndex`, `progress`, `timestamp`, `chapterTitle`, `paragraphIndex`, `snippet`)
- Reference: `lib/KOReaderSync/ProgressMapper.h:60,76` (`toKOReader`, `toCrossPoint`), `lib/BookOrbit/XPointer.h` (P2)

**Interfaces:**
- Consumes: `bookorbit::Annotation`, `normalizeAnnotations` (Task 2); `bookorbit::Bookmark`, `normalizeBookmarks` (Task 3); `bookorbit::IAnnotationApplier`, `RemoteEntry`, `AppliedAck`, `DeletedAck` (Tasks 6–8); `ProgressMapper`; `BOOKMARKS`; `CLIPPINGS`.
- Produces:
  `std::string crossinkFormatDeviceDatetime(uint32_t unixTime)`;
  `bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out)`;
  `bool collectBookOrbitBookmarks(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Bookmark>& out)`;
  `class CrossInkAnnotationApplier : public bookorbit::IAnnotationApplier` with
  `explicit CrossInkAnnotationApplier(std::shared_ptr<Epub> epub, bool bookmarksRoute)`.

**Mapping decisions, stated once so the implementation does not have to guess:**

| BookOrbit field | CrossInk source |
|---|---|
| `datetime` | `crossinkFormatDeviceDatetime(record.timestamp)` — local time, `%Y-%m-%d %H:%M:%S` |
| `datetimeUpdated` | empty; CrossInk does not track highlight edit time |
| `drawer` | always `"lighten"` — CrossInk renders one highlight style |
| `text` | `CLIPPINGS.readClippingText(index, text)` |
| `chapter` | `clipping.chapterTitle` / `bookmark.chapterTitle` |
| `pageno` | `clipping.startPage + 1` / `bookmark` page derived from `progress` |
| `pos0` / `pos1` | `ProgressMapper::toKOReader(...).xpath`, canonicalized by `normalizeXPointer` |
| `pos` (bookmark) | same, from `(spineIndex, progress)` |
| `note` (bookmark) | empty: `Bookmark::snippet` is a rendered preview, not a user label |

- [ ] **Step 1: Write the collectors**

Create `src/bookorbit/CrossInkAnnotationSource.h`:

```cpp
#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitAnnotationSync.h"
#include "BookOrbitBookmarkModel.h"

// Local device time as "YYYY-MM-DD HH:MM:SS" — the exact shape BookOrbit keys
// on. A wrong shape is not a cosmetic problem: it is a different identity key.
std::string crossinkFormatDeviceDatetime(uint32_t unixTime);

// Reads the currently loaded ClippingStore / BookmarkStore for the open book
// and produces wire-shaped records. Both return false when no book is loaded.
bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out);
bool collectBookOrbitBookmarks(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Bookmark>& out);

// Applies server-side changes to the local stores. One instance serves one
// route: bookmarksRoute selects BookmarkStore over ClippingStore.
class CrossInkAnnotationApplier : public bookorbit::IAnnotationApplier {
 public:
  CrossInkAnnotationApplier(std::shared_ptr<Epub> epub, bool bookmarksRoute);

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds,
                   std::vector<bookorbit::AppliedAck>& acks) override;
  size_t applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                      std::vector<bookorbit::DeletedAck>& acks) override;

 private:
  std::shared_ptr<Epub> epub;
  bool bookmarksRoute;
};
```

In `src/bookorbit/CrossInkAnnotationSource.cpp`, the datetime helper and the
clipping collector:

```cpp
std::string crossinkFormatDeviceDatetime(const uint32_t unixTime) {
  const time_t seconds = static_cast<time_t>(unixTime);
  struct tm parts = {};
  localtime_r(&seconds, &parts);
  char buffer[20];  // "YYYY-MM-DD HH:MM:SS" + NUL
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return buffer;
}

bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out) {
  if (!epub) {
    LOG_ERR("BookOrbit: no book loaded, cannot collect highlights");
    return false;
  }

  const auto& clippings = CLIPPINGS.getClippings();
  out.clear();
  out.reserve(clippings.size());

  std::string text;
  for (size_t index = 0; index < clippings.size(); index++) {
    const Clipping& clipping = clippings[index];

    CrossPointPosition start;
    start.spineIndex = clipping.spineIndex;
    start.pageNumber = clipping.startPage;
    start.totalPages = clipping.pageCount;
    start.paragraphIndex = clipping.paragraphIndex;
    start.hasParagraphIndex = clipping.paragraphIndex != UINT16_MAX;

    CrossPointPosition end = start;
    end.pageNumber = clipping.endPage;

    bookorbit::Annotation entry;
    entry.datetime = crossinkFormatDeviceDatetime(clipping.timestamp);
    entry.drawer = "lighten";  // CrossInk renders exactly one highlight style
    entry.chapter = clipping.chapterTitle;
    entry.pageno = static_cast<int32_t>(clipping.startPage) + 1;
    entry.posFormat = "xpointer";
    entry.pos0 = ProgressMapper::toKOReader(epub, start).xpath;
    entry.pos1 = ProgressMapper::toKOReader(epub, end).xpath;

    text.clear();
    if (CLIPPINGS.readClippingText(index, text)) {
      entry.text = text;
    }

    // normalizeAnnotations() drops anything whose position is not a real
    // xpointer, so an unmappable clipping is skipped rather than uploaded
    // under a key the server cannot match.
    out.push_back(std::move(entry));
  }
  return true;
}
```

The bookmark collector mirrors it, deriving a page from `progress`:

```cpp
bool collectBookOrbitBookmarks(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Bookmark>& out) {
  if (!epub) {
    LOG_ERR("BookOrbit: no book loaded, cannot collect bookmarks");
    return false;
  }

  const auto& bookmarks = BOOKMARKS.getBookmarks();
  out.clear();
  out.reserve(bookmarks.size());

  for (const Bookmark& bookmark : bookmarks) {
    CrossPointPosition position;
    position.spineIndex = bookmark.spineIndex;
    // BookmarkStore keeps intra-spine progress, not a page. Pages move with
    // font size; the fraction does not, so rebuild the page from it.
    position.totalPages = 1000;
    position.pageNumber = static_cast<int>(bookmark.progress * 1000.0f);
    position.paragraphIndex = bookmark.paragraphIndex;
    position.hasParagraphIndex = bookmark.paragraphIndex != UINT16_MAX;

    bookorbit::Bookmark entry;
    entry.datetime = crossinkFormatDeviceDatetime(bookmark.timestamp);
    entry.chapter = bookmark.chapterTitle;
    entry.pos = ProgressMapper::toKOReader(epub, position).xpath;
    out.push_back(std::move(entry));
  }
  return true;
}
```

- [ ] **Step 2: Write the applier**

`applyAdds` resolves each remote position back to a CrossInk position and
writes it into the matching store. Every entry produces exactly one ack —
silence would make the server re-send it forever:

```cpp
size_t CrossInkAnnotationApplier::applyAdds(const std::vector<bookorbit::RemoteEntry>& adds,
                                            std::vector<bookorbit::AppliedAck>& acks) {
  size_t touched = 0;
  acks.reserve(acks.size() + adds.size());

  for (const auto& remote : adds) {
    bookorbit::AppliedAck ack;
    ack.serverId = remote.serverId;

    const std::string canonical = bookorbit::normalizeXPointer(remote.pos0);
    if (canonical.empty() || !epub) {
      LOG_ERR("BookOrbit: cannot resolve incoming position for server entry %s", remote.serverId.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    KOReaderPosition incoming;
    incoming.xpath = canonical;
    incoming.percentage = 0.0f;
    const CrossPointPosition local = ProgressMapper::toCrossPoint(epub, incoming);
    if (!local.valid) {
      LOG_ERR("BookOrbit: incoming position did not resolve: %s", canonical.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    const std::string datetime = remote.datetime.empty()
                                     ? crossinkFormatDeviceDatetime(static_cast<uint32_t>(time(nullptr)))
                                     : remote.datetime;

    bool stored = false;
    if (bookmarksRoute) {
      const float progress = local.totalPages > 0
                                 ? static_cast<float>(local.pageNumber) / static_cast<float>(local.totalPages)
                                 : 0.0f;
      stored = BOOKMARKS.addBookmark(static_cast<uint16_t>(local.spineIndex), progress, local.totalPages,
                                     remote.chapter.c_str(), local.paragraphIndex, nullptr) ==
               BookmarkStore::AddResult::Added;
    } else {
      stored = CLIPPINGS.addClipping(static_cast<uint16_t>(local.spineIndex),
                                     static_cast<uint16_t>(local.pageNumber),
                                     static_cast<uint16_t>(local.pageNumber),
                                     static_cast<uint16_t>(local.totalPages), 0, 0, 0, remote.chapter.c_str(),
                                     local.paragraphIndex, remote.text, UINT16_MAX, 0) ==
               ClippingStore::AddResult::Added;
    }

    if (!stored) {
      LOG_ERR("BookOrbit: local store refused entry %s", remote.serverId.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    touched++;
    // Report the identity the entry actually got locally, so a later
    // server-side delete can address it.
    ack.datetime = datetime;
    ack.pos0 = canonical;
    ack.key = bookmarksRoute ? bookorbit::buildBookmarkKey(datetime, canonical)
                             : bookorbit::buildAnnotationKey(datetime, canonical);
    acks.push_back(ack);
  }

  if (touched > 0) {
    if (bookmarksRoute) {
      BOOKMARKS.saveToFile();
    } else {
      CLIPPINGS.saveToFile();
    }
  }
  return touched;
}
```

`applyDeletes` removes by index and acks every entry, present or not — a
missing entry was already deleted locally, which is the outcome the server
asked for:

```cpp
size_t CrossInkAnnotationApplier::applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                                               std::vector<bookorbit::DeletedAck>& acks) {
  size_t touched = 0;
  acks.reserve(acks.size() + deletes.size());

  for (const auto& remote : deletes) {
    if (bookmarksRoute) {
      const auto& bookmarks = BOOKMARKS.getBookmarks();
      for (size_t index = 0; index < bookmarks.size(); index++) {
        const std::string datetime = crossinkFormatDeviceDatetime(bookmarks[index].timestamp);
        CrossPointPosition position;
        position.spineIndex = bookmarks[index].spineIndex;
        position.totalPages = 1000;
        position.pageNumber = static_cast<int>(bookmarks[index].progress * 1000.0f);
        const std::string pos = bookorbit::normalizeXPointer(ProgressMapper::toKOReader(epub, position).xpath);
        if (!pos.empty() && bookorbit::buildBookmarkKey(datetime, pos) == remote.key) {
          BOOKMARKS.removeBookmarkAt(index);
          touched++;
          break;
        }
      }
    } else {
      const auto& clippings = CLIPPINGS.getClippings();
      for (size_t index = 0; index < clippings.size(); index++) {
        const std::string datetime = crossinkFormatDeviceDatetime(clippings[index].timestamp);
        CrossPointPosition position;
        position.spineIndex = clippings[index].spineIndex;
        position.pageNumber = clippings[index].startPage;
        position.totalPages = clippings[index].pageCount;
        const std::string pos = bookorbit::normalizeXPointer(ProgressMapper::toKOReader(epub, position).xpath);
        if (!pos.empty() && bookorbit::buildAnnotationKey(datetime, pos) == remote.key) {
          CLIPPINGS.removeClippingAt(index);
          touched++;
          break;
        }
      }
    }
    // Ack either way: a missing entry is already in the state the server wants.
    acks.push_back({remote.serverId, false});
  }

  if (touched > 0) {
    if (bookmarksRoute) {
      BOOKMARKS.saveToFile();
    } else {
      CLIPPINGS.saveToFile();
    }
  }
  return touched;
}
```

- [ ] **Step 3: Verify it builds for every target**

```bash
pio run -e x4-pro && pio run -e default && pio run -e sticky && pio run -e simulator
```

Expected: all four link, and `scripts/check_firmware_size.py` passes.

- [ ] **Step 4: Static analysis and formatting**

```bash
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

- [ ] **Step 5: Commit**

```bash
git add src/bookorbit/CrossInkAnnotationSource.h src/bookorbit/CrossInkAnnotationSource.cpp
git commit -m "feat: map CrossInk bookmarks and clippings onto BookOrbit shapes"
```

---

### Task 11: Sync entry point, translated strings, and changelog

**Files:**
- Modify: `lib/I18n/translations/en.yaml` (add the `STR_BOOKORBIT_ANN_*` keys, then regenerate)
- Modify: `src/activities/bookorbit/BookOrbitSettingsActivity.cpp` (P0 Task 10) — add the "Sync highlights now" row
- Modify: `CHANGELOG.md`
- Reference: `src/activities/settings/KOReaderSettingsActivity.cpp` for the row pattern

**Interfaces:**
- Consumes: `exchangeAnnotations` (Task 8), `exchangeBookmarks` (Task 9), `collectBookOrbitAnnotations`, `collectBookOrbitBookmarks`, `CrossInkAnnotationApplier` (Task 10), `SyncStateStore`, `CapabilityCache`, `BookOrbitClient` (P0).
- Produces: a manual sync action; no API consumed by later phases.

- [ ] **Step 1: Add translation keys**

Add to `lib/I18n/translations/en.yaml`:

```yaml
STR_BOOKORBIT_ANN_SYNC_NOW: "Sync highlights now"
STR_BOOKORBIT_ANN_SYNCING: "Syncing highlights…"
STR_BOOKORBIT_ANN_UP_TO_DATE: "Highlights already up to date"
STR_BOOKORBIT_ANN_SYNCED: "Highlights synced"
STR_BOOKORBIT_ANN_PARTIAL: "Some highlights could not be applied"
STR_BOOKORBIT_ANN_UNMATCHED: "This book is not in your BookOrbit library"
STR_BOOKORBIT_ANN_FAILED: "Could not sync highlights"
STR_BOOKORBIT_BM_UNSUPPORTED: "This server does not sync bookmarks"
```

Regenerate:

```bash
python3 scripts/gen_i18n.py
```

Do not hand-edit `lib/I18n/I18nKeys.h` or `I18nStrings.{h,cpp}` — they are generated.

- [ ] **Step 2: Wire the sync action**

Add a row to `BookOrbitSettingsActivity` that runs, for the currently open book:

1. `collectBookOrbitAnnotations(epub, raw)` → `normalizeAnnotations(raw)`.
2. `exchangeAnnotations(client, state.findOrCreate(hash), hash, normalized, applier, now, outcome)`.
3. `collectBookOrbitBookmarks(epub, rawBookmarks)` → `normalizeBookmarks(...)` → `exchangeBookmarks(...)`.
4. `syncState.flush()` — the signature stamps are only durable once this returns.

Map the result to one message, all through `tr(STR_*)`:

| Condition | Message |
|---|---|
| `outcome.skipped` on both routes | `STR_BOOKORBIT_ANN_UP_TO_DATE` |
| `Status::Ok`, `failed == 0` | `STR_BOOKORBIT_ANN_SYNCED` |
| `Status::Ok`, `failed > 0` | `STR_BOOKORBIT_ANN_PARTIAL` |
| `outcome.unmatched` | `STR_BOOKORBIT_ANN_UNMATCHED` |
| bookmark route `Status::NotFound` | `STR_BOOKORBIT_BM_UNSUPPORTED` |
| any other error | `STR_BOOKORBIT_ANN_FAILED` |

Run the exchange off the UI cadence, as the spec's phase chain requires: allocate
the client and buffers in `onEnter()`, release them in `onExit()`, and never
block the render task. A network task stack of 4096 bytes is the repo norm.

- [ ] **Step 3: Verify it builds and runs**

```bash
pio run -e x4-pro && pio run -e default
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
ctest --test-dir /tmp/crossink-tests --output-on-failure
```

Expected: builds link, the smoke test passes, and every `BookOrbit*` test target is still green.

- [ ] **Step 4: Add the changelog entry**

Under an `### Added` heading in `CHANGELOG.md`:

```markdown
- BookOrbit highlight and bookmark sync: highlights and bookmarks now sync two-way with a BookOrbit server, including highlights created on the web and deletions made on another device.
```

- [ ] **Step 5: Commit**

```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add lib/I18n/translations/en.yaml lib/I18n src/activities/bookorbit CHANGELOG.md
git commit -m "feat: add BookOrbit highlight and bookmark sync action"
```

---

## Hardware Verification

After Task 11, on an X4 Pro with an SD card and a reachable BookOrbit server:

1. Open a book the server already knows (its partial MD5 matched in P0). Highlight two passages and add a dogear.
2. Settings → BookOrbit Sync → "Sync highlights now". Expect `Highlights synced`.
3. On the BookOrbit web UI, confirm both highlights appear with the right text and chapter, and the dogear appears at the right chapter.
4. Tap "Sync highlights now" again without changing anything. Expect `Highlights already up to date`, and **no** `/annotations/exchange` request in the serial log — this is the signature skip working.
5. Create a highlight on the web, then sync on-device. Expect it to appear in the book's clipping list at the right position.
6. Delete a highlight on the web, then sync. Expect it gone from the device list.
7. **Crash safety:** start a sync that has server-side changes to apply, and pull power (or drop WiFi) immediately after the reader redraws with the new highlight but before the sync completes. On reboot, sync again: the highlight must still be there exactly once, not twice, and not missing.
8. Point the device at a BookOrbit server that predates the bookmark route. Expect `This server does not sync bookmarks`, highlights still syncing, and one 404 in the log — not one per sync.
9. Stop the server mid-sync (forcing a 5xx or a dropped connection) and sync again once it is back. Bookmark sync must resume; a permanently disabled bookmark route here is the tri-state bug.
10. Confirm the state file on the SD card carries `annSignature` and `bmSignature` for the book, and that no clipping or bookmark file was rewritten on a skipped sync.

## Self-Review Notes

- **Spec coverage:** the three-legged exchange (Tasks 5–9), `keys`/`keysComplete` with the 5000/500 caps (Tasks 5, 8, 9), `UPLOAD_CHUNK = 50` and `MAX_PULL_ROUNDS = 10` (Tasks 8, 9), the `"count:maxDatetime:hash1:hash2"` signature stored in `annSignature`/`bmSignature` (Tasks 2, 3, 4), the full annotation and bookmark field mappings with every truncation limit (Tasks 2, 3), and `bookmarkSync` tri-state gating (Task 9). `BookmarkStore`/`ClippingStore` mapping is Task 10.
- **P2 dependency is load-bearing, not decorative.** Every position that reaches a key, a signature, or the wire passes through `normalizeXPointer` first. If P2's canonical form changes after P4 ships, every stored signature changes with it and every book re-exchanges once — correct, but noisy. Freeze the canonical form with P2, not later.
- **Deviation from the Lua client, deliberate:** no `annWatermark` datetime delta. P0's `BookSyncState` has no such field, and adding one is a P0 format change. The full set uploads in 50-entry chunks and the signature skip keeps that off the wire for unchanged books. See *Design note* above.
- **Ack-before-stamp is the only crash-safety mechanism here,** and it is asymmetric on purpose: applying locally then failing to ack costs one redundant re-delivery, while stamping before the ack would silently lose server-side changes for six hours. The applier must therefore be identity-keyed — Task 10 keys on `md5(datetime|pos)` exactly as the wire does, and the Task 8 test `ReExchangeAfterALostAckDoesNotDuplicate` is what proves it.
- **`failed > 0` blocks the stamp** on purpose. An entry that could not be applied is still owed by the server; stamping would hide it until the six-hour bound expired.
- **Type consistency:** `RemoteEntry` (Task 6) is the single decoded shape for both routes — a bookmark's `pos` lands in `pos0` — so `IAnnotationApplier` (Task 8) serves annotations and bookmarks without a second interface. `AppliedAck`/`DeletedAck` (Task 7) are produced by the applier and consumed by both state machines unchanged.
- **Not in this phase:** editing highlights from the server (`toApply.edit` in the Lua client). CrossInk has no highlight-edit UI, so an `edit` entry has no local target; the decoder ignores the section rather than half-applying it. Add it when the edit UI exists.
