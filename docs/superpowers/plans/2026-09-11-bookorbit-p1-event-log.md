# BookOrbit P1 — Reading Event Log and Page-Stats Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Record a per-page-turn reading event log on-device with KOReader's exact clamping semantics, derive KOReader's statistics from it, and upload it to BookOrbit incrementally.

**Architecture:** A 16-byte append-only binary record per page turn, mirroring KOReader's `page_stat_data` row, written alongside each book's existing `stats_v5.bin`. Page numbers come from CrossInk's existing layout-independent reference pages. Everything except the reader hook is pure logic behind injected interfaces, so it runs under the native GoogleTest suite.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), `sqlite3` CLI (ground-truth verification only, not linked into firmware).

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md`

**Depends on:** P0 (`docs/superpowers/plans/2026-09-11-bookorbit-p0-client-and-state.md`) — consumes `BookOrbitClient`, `Error`/`Status`, `SyncStateStore` (field `statsWatermark`), `IBlobStore`, `jsonEscape`.

## Global Constraints

- Repo guide is `AGENTS.md` (mirrored to `CLAUDE.md`). Its rules bind every task.
- Branch prefix `feat/`; commit messages `<type>: <short summary>`.
- All user-facing strings via `tr(STR_*)`. Logs may be hardcoded.
- No exceptions, no `abort()`. `LOG_ERR(...)` then `return false` on recoverable failure.
- `new` is not nothrow on ESP32. Use `new (std::nothrow)` or `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h`.
- Local stack allocations over 256 bytes must be justified in a comment.
- `string_view::data()` is **not** null-terminated — never pass it to a C API.
- File I/O uses `FsFile`, never Arduino `File`. Always close explicitly.
- **Debounce persistent writes. Do not write progress on every page turn** (`AGENTS.md` resource rule 8) — the event log buffers in RAM and flushes every `kFlushEveryNEvents = 50` turns, matching KOReader's `MAX_PAGETURNS_BEFORE_FLUSH = 50`.
- Shared code must stay within ESP32-C3 limits (~380 KB internal RAM, no PSRAM).
- **KOReader clamping, exact:** `min_sec` default **5** — dwell below this is discarded. `max_sec` default **120** — dwell above this is **clamped to 120, not discarded**. Both user-configurable.
- Page-stats batch: **500** events per POST. Body cap **900 KiB**.
- Upload watermark back-off on a full batch: `statsWatermark = lastEvent.startTime - 1`.
- Capped statistics group by **page**, not by event: `min(sum(duration), max_sec)` per distinct page.
- Do not edit generated files (`lib/I18n/I18nKeys.h`, `I18nStrings.*`, icon headers, `*.generated.h`).
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally `pio run -e x4-pro` and `pio run -e default`.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/ReadingEvent.h` | The 16-byte record and its little-endian encoding. |
| `lib/BookOrbit/ClampPolicy.{h,cpp}` | KOReader's min/max dwell rules. Pure function, no state. |
| `lib/BookOrbit/ReadingEventLog.{h,cpp}` | Append-only per-book log: buffer, flush, read-after-watermark. |
| `lib/BookOrbit/PageResolver.{h,cpp}` | Stable global page + total from reference pages, with the byte-based fallback. |
| `lib/BookOrbit/PageStatsCodec.{h,cpp}` | page-stats encode / decode, including the watermark back-off rule. |
| `lib/BookOrbit/ReadingStatsQuery.{h,cpp}` | KOReader's statistics formulas over the log. |
| `lib/BookOrbit/BookOrbitOutbox.{h,cpp}` | Phase-ack state machine (deferred here from P0, now that it has phases to sequence). |
| `src/activities/reader/` | The page-turn hook, modified in place. |
| `test/bookorbit_*/` | One GoogleTest target per unit. |

---

### Task 1: The reading event record

**Files:**
- Create: `lib/BookOrbit/ReadingEvent.h`, `lib/BookOrbit/ReadingEvent.cpp`
- Create: `test/bookorbit_reading_event/CMakeLists.txt`, `test/bookorbit_reading_event/ReadingEventTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct bookorbit::ReadingEvent { uint32_t page; uint32_t startTime; uint16_t durationSeconds; uint16_t totalPages; uint32_t reserved; }`;
  `void bookorbit::encodeEvent(const ReadingEvent&, uint8_t out[16])`;
  `bool bookorbit::decodeEvent(const uint8_t in[16], ReadingEvent& out)`;
  `inline constexpr size_t bookorbit::kEventBytes = 16`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_reading_event/ReadingEventTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdint>

#include "lib/BookOrbit/ReadingEvent.h"

using bookorbit::decodeEvent;
using bookorbit::encodeEvent;
using bookorbit::kEventBytes;
using bookorbit::ReadingEvent;

TEST(ReadingEvent, RecordIsSixteenBytes) {
  EXPECT_EQ(kEventBytes, 16u);
  EXPECT_EQ(sizeof(ReadingEvent), 16u);
}

TEST(ReadingEvent, RoundTripsAllFields) {
  ReadingEvent in;
  in.page = 42;
  in.startTime = 1787561453u;
  in.durationSeconds = 37;
  in.totalPages = 310;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);

  ReadingEvent out;
  ASSERT_TRUE(decodeEvent(buf, out));
  EXPECT_EQ(out.page, 42u);
  EXPECT_EQ(out.startTime, 1787561453u);
  EXPECT_EQ(out.durationSeconds, 37);
  EXPECT_EQ(out.totalPages, 310);
}

// Little-endian is explicit so a log written on one target reads on another.
TEST(ReadingEvent, EncodesLittleEndian) {
  ReadingEvent in;
  in.page = 0x04030201u;
  in.startTime = 0u;
  in.durationSeconds = 0;
  in.totalPages = 0;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[1], 0x02);
  EXPECT_EQ(buf[2], 0x03);
  EXPECT_EQ(buf[3], 0x04);
}

TEST(ReadingEvent, HandlesMaximumValues) {
  ReadingEvent in;
  in.page = 0xFFFFFFFFu;
  in.startTime = 0xFFFFFFFFu;
  in.durationSeconds = 0xFFFF;
  in.totalPages = 0xFFFF;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);

  ReadingEvent out;
  ASSERT_TRUE(decodeEvent(buf, out));
  EXPECT_EQ(out.page, 0xFFFFFFFFu);
  EXPECT_EQ(out.startTime, 0xFFFFFFFFu);
  EXPECT_EQ(out.durationSeconds, 0xFFFF);
  EXPECT_EQ(out.totalPages, 0xFFFF);
}
```

Create `test/bookorbit_reading_event/CMakeLists.txt`:

```cmake
add_executable(ReadingEventTest
  ReadingEventTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingEventTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingEventTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingEventTest)
```

Add `add_subdirectory(bookorbit_reading_event)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `ReadingEvent.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/ReadingEvent.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

namespace bookorbit {

inline constexpr size_t kEventBytes = 16;

// One page-turn event, mirroring a row of KOReader's page_stat_data table so
// the upload payload needs no translation layer.
//
// `page` and `totalPages` are layout-independent reference pages, so they stay
// comparable across font and margin changes. `totalPages` is recorded on every
// event because the server rescales against it exactly as KOReader's page_stat
// view does.
struct ReadingEvent {
  uint32_t page = 0;
  uint32_t startTime = 0;  // unix epoch seconds; requires a valid RTC
  uint16_t durationSeconds = 0;
  uint16_t totalPages = 0;
  uint32_t reserved = 0;
};

static_assert(sizeof(ReadingEvent) == kEventBytes, "ReadingEvent must stay 16 bytes");

void encodeEvent(const ReadingEvent& event, uint8_t out[kEventBytes]);
bool decodeEvent(const uint8_t in[kEventBytes], ReadingEvent& out);

}  // namespace bookorbit
```

Create `lib/BookOrbit/ReadingEvent.cpp`:

```cpp
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
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
         (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
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
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R ReadingEvent --output-on-failure
```

Expected: 4 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/ReadingEvent.h lib/BookOrbit/ReadingEvent.cpp test/bookorbit_reading_event test/CMakeLists.txt
git commit -m "feat: add reading event record for BookOrbit page stats"
```

---

### Task 2: KOReader clamping policy

**Files:**
- Create: `lib/BookOrbit/ClampPolicy.h`, `lib/BookOrbit/ClampPolicy.cpp`
- Create: `test/bookorbit_clamp/CMakeLists.txt`, `test/bookorbit_clamp/ClampPolicyTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `struct bookorbit::ClampSettings { uint16_t minSec = 5; uint16_t maxSec = 120; }`;
  `bool bookorbit::clampDwell(uint32_t rawSeconds, const ClampSettings&, uint16_t& outSeconds)` — returns false when the dwell is discarded.

**This is the behavioural change that makes on-device numbers match KOReader's.** CrossInk today uses a 2-second minimum and *discards* anything over a 300-second idle threshold (`EpubReaderActivity.cpp:1460-1479`). KOReader instead **clamps** the long tail. Both differences are deliberate here.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_clamp/ClampPolicyTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/ClampPolicy.h"

using bookorbit::ClampSettings;
using bookorbit::clampDwell;

TEST(ClampPolicy, DefaultsMatchKOReader) {
  const ClampSettings settings;
  EXPECT_EQ(settings.minSec, 5);
  EXPECT_EQ(settings.maxSec, 120);
}

TEST(ClampPolicy, DiscardsBelowMinimum) {
  const ClampSettings settings;
  uint16_t out = 0xFFFF;
  EXPECT_FALSE(clampDwell(4, settings, out));
}

TEST(ClampPolicy, AcceptsExactlyMinimum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(5, settings, out));
  EXPECT_EQ(out, 5);
}

TEST(ClampPolicy, CreditsInFullWithinRange) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(37, settings, out));
  EXPECT_EQ(out, 37);
}

TEST(ClampPolicy, AcceptsExactlyMaximum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(120, settings, out));
  EXPECT_EQ(out, 120);
}

// The critical divergence from CrossInk's current behaviour: a long dwell is
// CLAMPED to max_sec, not discarded. KOReader credits 120s for an idle page.
TEST(ClampPolicy, ClampsAboveMaximumRatherThanDiscarding) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(121, settings, out));
  EXPECT_EQ(out, 120);
}

TEST(ClampPolicy, ClampsVeryLongIdleToMaximum) {
  const ClampSettings settings;
  uint16_t out = 0;
  ASSERT_TRUE(clampDwell(7200, settings, out));
  EXPECT_EQ(out, 120);
}

TEST(ClampPolicy, HonoursCustomSettings) {
  ClampSettings settings;
  settings.minSec = 10;
  settings.maxSec = 60;
  uint16_t out = 0;
  EXPECT_FALSE(clampDwell(9, settings, out));
  ASSERT_TRUE(clampDwell(300, settings, out));
  EXPECT_EQ(out, 60);
}

TEST(ClampPolicy, ZeroDwellIsDiscarded) {
  const ClampSettings settings;
  uint16_t out = 0xFFFF;
  EXPECT_FALSE(clampDwell(0, settings, out));
}
```

Create `test/bookorbit_clamp/CMakeLists.txt`:

```cmake
add_executable(ClampPolicyTest
  ClampPolicyTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
)

target_include_directories(ClampPolicyTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ClampPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClampPolicyTest)
```

Add `add_subdirectory(bookorbit_clamp)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `ClampPolicy.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/ClampPolicy.h`:

```cpp
#pragma once

#include <cstdint>

namespace bookorbit {

// KOReader's DEFAULT_MIN_READ_SEC / DEFAULT_MAX_READ_SEC, from
// plugins/statistics.koplugin/main.lua:29-30. Adopting these exactly is what
// makes on-device statistics comparable to KOReader's for the same reading.
struct ClampSettings {
  uint16_t minSec = 5;
  uint16_t maxSec = 120;
};

// Applies KOReader's rules to one page dwell:
//   below minSec          -> discarded (returns false)
//   minSec..maxSec        -> credited in full
//   above maxSec          -> CLAMPED to maxSec (returns true)
//
// The clamp is the point. CrossInk currently discards long dwells outright,
// which silently loses reading time KOReader would have credited.
bool clampDwell(uint32_t rawSeconds, const ClampSettings& settings, uint16_t& outSeconds);

}  // namespace bookorbit
```

Create `lib/BookOrbit/ClampPolicy.cpp`:

```cpp
#include "ClampPolicy.h"

namespace bookorbit {

bool clampDwell(const uint32_t rawSeconds, const ClampSettings& settings, uint16_t& outSeconds) {
  if (rawSeconds < settings.minSec) {
    return false;
  }
  if (rawSeconds > settings.maxSec) {
    outSeconds = settings.maxSec;
    return true;
  }
  outSeconds = static_cast<uint16_t>(rawSeconds);
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R ClampPolicy --output-on-failure
```

Expected: 9 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/ClampPolicy.h lib/BookOrbit/ClampPolicy.cpp test/bookorbit_clamp test/CMakeLists.txt
git commit -m "feat: adopt KOReader dwell clamping rules"
```

---

### Task 3: Append-only event log

**Files:**
- Create: `lib/BookOrbit/ReadingEventLog.h`, `lib/BookOrbit/ReadingEventLog.cpp`
- Create: `test/bookorbit_event_log/CMakeLists.txt`, `test/bookorbit_event_log/ReadingEventLogTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `ReadingEvent`, `encodeEvent`, `decodeEvent` (Task 1); `IBlobStore` (P0 Task 4).
- Produces: `class bookorbit::ReadingEventLog` with
  `ReadingEventLog(IBlobStore&, std::string path)`,
  `void append(const ReadingEvent&)`,
  `bool flush()`,
  `size_t pendingCount() const`,
  `bool readAfter(uint32_t watermark, size_t limit, std::vector<ReadingEvent>& out) const`,
  `bool maxStartTime(uint32_t& out) const`;
  `inline constexpr size_t bookorbit::kFlushEveryNEvents = 50`.

`readAfter` returns events ordered by `(startTime, page)`, matching the Lua client's `ORDER BY start_time, page`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_event_log/ReadingEventLogTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/IBlobStore.h"
#include "lib/BookOrbit/ReadingEventLog.h"

namespace {

class FakeBlobStore : public bookorbit::IBlobStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;

  bool read(const std::string_view path, std::vector<uint8_t>& out) override {
    const auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool write(const std::string_view path, const uint8_t* data, const size_t len) override {
    files[std::string(path)] = std::vector<uint8_t>(data, data + len);
    return true;
  }
  bool append(const std::string_view path, const uint8_t* data, const size_t len) override {
    auto& file = files[std::string(path)];
    file.insert(file.end(), data, data + len);
    return true;
  }
  bool rename(const std::string_view from, const std::string_view to) override {
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

bookorbit::ReadingEvent makeEvent(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  bookorbit::ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

}  // namespace

using bookorbit::kFlushEveryNEvents;
using bookorbit::ReadingEvent;
using bookorbit::ReadingEventLog;

TEST(ReadingEventLog, AppendBuffersWithoutWriting) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  EXPECT_EQ(log.pendingCount(), 1u);
  EXPECT_FALSE(store.exists("/events.bin"));
}

TEST(ReadingEventLog, FlushPersistsBufferedEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 1010, 20));
  ASSERT_TRUE(log.flush());
  EXPECT_EQ(log.pendingCount(), 0u);
  EXPECT_EQ(store.files["/events.bin"].size(), 2u * bookorbit::kEventBytes);
}

// AGENTS.md resource rule 8: do not write on every page turn.
TEST(ReadingEventLog, AutoFlushesAtFiftyEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  EXPECT_EQ(kFlushEveryNEvents, 50u);
  for (uint32_t i = 0; i < kFlushEveryNEvents; i++) {
    log.append(makeEvent(i + 1, 1000 + i, 10));
  }
  EXPECT_EQ(log.pendingCount(), 0u);
  EXPECT_EQ(store.files["/events.bin"].size(), kFlushEveryNEvents * bookorbit::kEventBytes);
}

TEST(ReadingEventLog, AppendsRatherThanOverwriting) {
  FakeBlobStore store;
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(1, 1000, 10));
    ASSERT_TRUE(log.flush());
  }
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(2, 1010, 20));
    ASSERT_TRUE(log.flush());
  }
  EXPECT_EQ(store.files["/events.bin"].size(), 2u * bookorbit::kEventBytes);
}

TEST(ReadingEventLog, ReadAfterFiltersByWatermark) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 2000, 20));
  log.append(makeEvent(3, 3000, 30));
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(1000, 500, out));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].startTime, 2000u);
  EXPECT_EQ(out[1].startTime, 3000u);
}

TEST(ReadingEventLog, ReadAfterRespectsLimit) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  for (uint32_t i = 0; i < 10; i++) {
    log.append(makeEvent(i + 1, 1000 + i, 10));
  }
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 4, out));
  EXPECT_EQ(out.size(), 4u);
}

// Matches the Lua client's "ORDER BY start_time, page".
TEST(ReadingEventLog, ReadAfterOrdersByStartTimeThenPage) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(5, 2000, 10));
  log.append(makeEvent(2, 2000, 10));
  log.append(makeEvent(9, 1000, 10));
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].startTime, 1000u);
  EXPECT_EQ(out[1].page, 2u);
  EXPECT_EQ(out[2].page, 5u);
}

TEST(ReadingEventLog, ReadAfterIncludesPendingEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  ASSERT_TRUE(log.flush());
  log.append(makeEvent(2, 2000, 20));  // still buffered

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  EXPECT_EQ(out.size(), 2u);
}

TEST(ReadingEventLog, MaxStartTimeReportsLatest) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 5000, 20));
  ASSERT_TRUE(log.flush());

  uint32_t latest = 0;
  ASSERT_TRUE(log.maxStartTime(latest));
  EXPECT_EQ(latest, 5000u);
}

TEST(ReadingEventLog, MaxStartTimeFailsOnEmptyLog) {
  FakeBlobStore store;
  const ReadingEventLog log(store, "/events.bin");
  uint32_t latest = 0;
  EXPECT_FALSE(log.maxStartTime(latest));
}

// A truncated tail (power loss mid-write) must not corrupt the whole log.
TEST(ReadingEventLog, IgnoresTruncatedTrailingRecord) {
  FakeBlobStore store;
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(1, 1000, 10));
    ASSERT_TRUE(log.flush());
  }
  store.files["/events.bin"].resize(bookorbit::kEventBytes + 7);  // half a record

  const ReadingEventLog log(store, "/events.bin");
  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  EXPECT_EQ(out.size(), 1u);
}
```

Create `test/bookorbit_event_log/CMakeLists.txt`:

```cmake
add_executable(ReadingEventLogTest
  ReadingEventLogTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEventLog.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingEventLogTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingEventLogTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingEventLogTest)
```

Add `add_subdirectory(bookorbit_event_log)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `ReadingEventLog.h: No such file or directory`, plus an error that `IBlobStore` has no `append` member.

- [ ] **Step 3: Extend IBlobStore, then write the log**

First add the append primitive to P0's interface. In `lib/BookOrbit/IBlobStore.h`, add:

```cpp
  // Appends to the end of a file, creating it when absent. Separate from
  // write() because the event log must never rewrite what it already wrote.
  virtual bool append(std::string_view path, const uint8_t* data, size_t len) = 0;
```

Then create `lib/BookOrbit/ReadingEventLog.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IBlobStore.h"
#include "ReadingEvent.h"

namespace bookorbit {

// Matches KOReader's MAX_PAGETURNS_BEFORE_FLUSH and satisfies AGENTS.md
// resource rule 8 (do not write persistently on every page turn).
inline constexpr size_t kFlushEveryNEvents = 50;

// Append-only per-book reading event log. Buffers in RAM and flushes in
// batches; a crash loses at most kFlushEveryNEvents events, exactly as
// KOReader's in-memory page_stat buffer does.
class ReadingEventLog {
 public:
  ReadingEventLog(IBlobStore& store, std::string path);

  void append(const ReadingEvent& event);
  bool flush();
  size_t pendingCount() const { return pending.size(); }

  // Events with startTime > watermark, ordered by (startTime, page), capped at
  // limit. Includes still-buffered events so a sync never misses recent reading.
  bool readAfter(uint32_t watermark, size_t limit, std::vector<ReadingEvent>& out) const;

  bool maxStartTime(uint32_t& out) const;

 private:
  bool readAll(std::vector<ReadingEvent>& out) const;

  IBlobStore& blobs;
  std::string path;
  std::vector<ReadingEvent> pending;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/ReadingEventLog.cpp`:

```cpp
#include "ReadingEventLog.h"

#include <algorithm>
#include <utility>

namespace bookorbit {

ReadingEventLog::ReadingEventLog(IBlobStore& store, std::string logPath)
    : blobs(store), path(std::move(logPath)) {}

void ReadingEventLog::append(const ReadingEvent& event) {
  pending.push_back(event);
  if (pending.size() >= kFlushEveryNEvents) {
    flush();
  }
}

bool ReadingEventLog::flush() {
  if (pending.empty()) return true;

  std::vector<uint8_t> raw;
  raw.resize(pending.size() * kEventBytes);
  for (size_t i = 0; i < pending.size(); i++) {
    encodeEvent(pending[i], raw.data() + i * kEventBytes);
  }
  if (!blobs.append(path, raw.data(), raw.size())) {
    return false;  // keep pending so the next flush retries
  }
  pending.clear();
  return true;
}

bool ReadingEventLog::readAll(std::vector<ReadingEvent>& out) const {
  out.clear();
  std::vector<uint8_t> raw;
  if (blobs.read(path, raw)) {
    // A truncated trailing record (power loss mid-write) is ignored rather
    // than invalidating the whole log.
    const size_t whole = raw.size() / kEventBytes;
    out.reserve(whole + pending.size());
    for (size_t i = 0; i < whole; i++) {
      ReadingEvent event;
      if (decodeEvent(raw.data() + i * kEventBytes, event)) {
        out.push_back(event);
      }
    }
  }
  out.insert(out.end(), pending.begin(), pending.end());
  return true;
}

bool ReadingEventLog::readAfter(const uint32_t watermark, const size_t limit,
                                std::vector<ReadingEvent>& out) const {
  std::vector<ReadingEvent> all;
  if (!readAll(all)) return false;

  out.clear();
  for (const auto& event : all) {
    if (event.startTime > watermark) {
      out.push_back(event);
    }
  }
  std::sort(out.begin(), out.end(), [](const ReadingEvent& a, const ReadingEvent& b) {
    if (a.startTime != b.startTime) return a.startTime < b.startTime;
    return a.page < b.page;
  });
  if (out.size() > limit) {
    out.resize(limit);
  }
  return true;
}

bool ReadingEventLog::maxStartTime(uint32_t& out) const {
  std::vector<ReadingEvent> all;
  if (!readAll(all) || all.empty()) return false;
  uint32_t latest = 0;
  for (const auto& event : all) {
    latest = std::max(latest, event.startTime);
  }
  out = latest;
  return true;
}

}  // namespace bookorbit
```

Update the `FakeBlobStore` in P0's `test/bookorbit_atomic_blob/AtomicBlobWriterTest.cpp` and `test/bookorbit_sync_state/BookOrbitSyncStateTest.cpp` to implement the new `append` override, or those targets will no longer compile.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
```

Expected: 11 `ReadingEventLog` tests PASS, and every P0 target still passes.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/ReadingEventLog.h lib/BookOrbit/ReadingEventLog.cpp lib/BookOrbit/IBlobStore.h test/bookorbit_event_log test/bookorbit_atomic_blob test/bookorbit_sync_state test/CMakeLists.txt
git commit -m "feat: add append-only reading event log"
```

---

### Task 4: Stable page resolution

**Files:**
- Create: `lib/BookOrbit/PageResolver.h`, `lib/BookOrbit/PageResolver.cpp`
- Create: `test/bookorbit_page_resolver/CMakeLists.txt`, `test/bookorbit_page_resolver/PageResolverTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `lib/Epub/Epub.h:185-193` — `hasStablePageNumbers()`, `resolveReferencePage()`, `calculateSizeProgress()`, `getBookSize()`

**Interfaces:**
- Consumes: nothing (the Epub dependency is injected as a plain struct so this stays host-testable).
- Produces: `struct bookorbit::PageSource { bool hasStablePages; uint32_t referencePage; uint32_t referencePageCount; float sizeProgress; size_t bookSize; }`;
  `bool bookorbit::resolvePage(const PageSource&, uint32_t& outPage, uint16_t& outTotal)`;
  `inline constexpr size_t bookorbit::kNominalPageBytes = 2048`.

**The fallback divisor must never change once events exist.** `totalPages` is recorded on every event, so a later firmware may pick a different divisor without invalidating history — but changing it mid-book would make one book's events inconsistent with each other.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_page_resolver/PageResolverTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/PageResolver.h"

using bookorbit::kNominalPageBytes;
using bookorbit::PageSource;
using bookorbit::resolvePage;

TEST(PageResolver, PrefersStableReferencePages) {
  PageSource source;
  source.hasStablePages = true;
  source.referencePage = 42;
  source.referencePageCount = 310;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(page, 42u);
  EXPECT_EQ(total, 310);
}

TEST(PageResolver, FallsBackToByteBasedPages) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.5f;
  source.bookSize = 100 * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 100);
  EXPECT_EQ(page, 51u);  // floor(0.5 * 100) + 1
}

TEST(PageResolver, FallbackRoundsPartialPageUp) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.0f;
  source.bookSize = kNominalPageBytes + 1;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 2);  // ceil(2049 / 2048)
  EXPECT_EQ(page, 1u);
}

TEST(PageResolver, FallbackClampsAtFinalPage) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 1.0f;
  source.bookSize = 10 * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 10);
  EXPECT_EQ(page, 10u);  // never total + 1
}

TEST(PageResolver, RejectsEmptyBook) {
  PageSource source;
  source.hasStablePages = false;
  source.bookSize = 0;

  uint32_t page = 0;
  uint16_t total = 0;
  EXPECT_FALSE(resolvePage(source, page, total));
}

TEST(PageResolver, RejectsZeroReferencePageCount) {
  PageSource source;
  source.hasStablePages = true;
  source.referencePage = 1;
  source.referencePageCount = 0;

  uint32_t page = 0;
  uint16_t total = 0;
  EXPECT_FALSE(resolvePage(source, page, total));
}

// totalPages is a uint16_t on the wire; a pathological book must not wrap.
TEST(PageResolver, ClampsTotalToSixteenBitMaximum) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.0f;
  source.bookSize = static_cast<size_t>(70000) * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 0xFFFF);
}
```

Create `test/bookorbit_page_resolver/CMakeLists.txt`:

```cmake
add_executable(PageResolverTest
  PageResolverTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/PageResolver.cpp
)

target_include_directories(PageResolverTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(PageResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageResolverTest)
```

Add `add_subdirectory(bookorbit_page_resolver)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `PageResolver.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/PageResolver.h`:

```cpp
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
  uint32_t referencePage = 0;       // 1-based
  uint32_t referencePageCount = 0;
  float sizeProgress = 0.0f;        // 0..1, used only in the fallback
  size_t bookSize = 0;              // bytes, used only in the fallback
};

// Produces the layout-independent (page, totalPages) pair recorded on every
// event. Returns false when neither source can yield a usable page.
bool resolvePage(const PageSource& source, uint32_t& outPage, uint16_t& outTotal);

}  // namespace bookorbit
```

Create `lib/BookOrbit/PageResolver.cpp`:

```cpp
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
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R PageResolver --output-on-failure
```

Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/PageResolver.h lib/BookOrbit/PageResolver.cpp test/bookorbit_page_resolver test/CMakeLists.txt
git commit -m "feat: add stable page resolution for reading events"
```

---

### Task 5: Page-stats codec and watermark back-off

**Files:**
- Create: `lib/BookOrbit/PageStatsCodec.h`, `lib/BookOrbit/PageStatsCodec.cpp`
- Create: `test/bookorbit_page_stats/CMakeLists.txt`, `test/bookorbit_page_stats/PageStatsCodecTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `ReadingEvent` (Task 1); `jsonEscape` (P0 Task 7); `StreamingJsonParser`.
- Produces:
  `std::string bookorbit::encodePageStats(std::string_view hash, const std::vector<ReadingEvent>&)`;
  `struct bookorbit::PageStatsAck { std::vector<std::string> unmatched; std::vector<std::pair<std::string,uint32_t>> watermarks; }`;
  `bool bookorbit::decodePageStats(std::string_view json, PageStatsAck& out)`;
  `bool bookorbit::nextWatermark(const std::vector<ReadingEvent>& sent, size_t batchSize, uint32_t oldWatermark, uint32_t serverWatermark, uint32_t& outWatermark)` — returns true when more batches remain;
  `inline constexpr size_t bookorbit::kStatsBatchSize = 500`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_page_stats/PageStatsCodecTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/PageStatsCodec.h"

using bookorbit::decodePageStats;
using bookorbit::encodePageStats;
using bookorbit::kStatsBatchSize;
using bookorbit::nextWatermark;
using bookorbit::PageStatsAck;
using bookorbit::ReadingEvent;

namespace {

ReadingEvent makeEvent(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

std::vector<ReadingEvent> makeBatch(const size_t count, const uint32_t sharedStartTime) {
  std::vector<ReadingEvent> events;
  events.reserve(count);
  for (size_t i = 0; i < count; i++) {
    events.push_back(makeEvent(static_cast<uint32_t>(i + 1), sharedStartTime, 10));
  }
  return events;
}

}  // namespace

TEST(PageStatsCodec, BatchSizeMatchesLuaClient) { EXPECT_EQ(kStatsBatchSize, 500u); }

TEST(PageStatsCodec, EncodesBooksWithHashAndEvents) {
  const std::vector<ReadingEvent> events{makeEvent(42, 1787561453u, 37)};
  const std::string json = encodePageStats("0f0a792b00a37cf80baa5e50c078b31f", events);

  EXPECT_NE(json.find(R"("hash":"0f0a792b00a37cf80baa5e50c078b31f")"), std::string::npos);
  EXPECT_NE(json.find(R"("page":42)"), std::string::npos);
  EXPECT_NE(json.find(R"("startTime":1787561453)"), std::string::npos);
  EXPECT_NE(json.find(R"("durationSeconds":37)"), std::string::npos);
  EXPECT_NE(json.find(R"("totalPages":310)"), std::string::npos);
}

TEST(PageStatsCodec, EncodesEmptyEventListAsEmptyArray) {
  const std::string json = encodePageStats("abc", {});
  EXPECT_NE(json.find(R"("events":[])"), std::string::npos);
}

TEST(PageStatsCodec, DecodesWatermarksAndUnmatched) {
  const std::string body = R"({
    "unmatched": ["deadbeef"],
    "results": [{"hash": "abc", "watermark": 1787561453}]
  })";

  PageStatsAck ack;
  ASSERT_TRUE(decodePageStats(body, ack));
  ASSERT_EQ(ack.unmatched.size(), 1u);
  EXPECT_EQ(ack.unmatched[0], "deadbeef");
  ASSERT_EQ(ack.watermarks.size(), 1u);
  EXPECT_EQ(ack.watermarks[0].first, "abc");
  EXPECT_EQ(ack.watermarks[0].second, 1787561453u);
}

TEST(PageStatsCodec, RejectsMalformedJson) {
  PageStatsAck ack;
  EXPECT_FALSE(decodePageStats("{not json", ack));
}

// A short batch means the server saw everything: trust its watermark and stop.
TEST(PageStatsCodec, ShortBatchTrustsServerWatermarkAndStops) {
  const auto events = makeBatch(10, 5000);
  uint32_t watermark = 0;
  EXPECT_FALSE(nextWatermark(events, kStatsBatchSize, 1000u, 5000u, watermark));
  EXPECT_EQ(watermark, 5000u);
}

// THE CRITICAL RULE. A full batch may have been cut inside a group of events
// sharing one startTime. Backing off one second re-sends that boundary group
// on the next round; without this, those events are lost forever. Re-sends are
// idempotent server-side.
TEST(PageStatsCodec, FullBatchBacksOffOneSecondAndContinues) {
  const auto events = makeBatch(kStatsBatchSize, 5000);
  uint32_t watermark = 0;
  EXPECT_TRUE(nextWatermark(events, kStatsBatchSize, 1000u, 5000u, watermark));
  EXPECT_EQ(watermark, 4999u);
}

// The back-off must never move the watermark backwards past where we already
// were, or the sync would loop forever re-sending the same events.
TEST(PageStatsCodec, BackOffNeverRegressesBelowOldWatermark) {
  const auto events = makeBatch(kStatsBatchSize, 1000);
  uint32_t watermark = 0;
  EXPECT_TRUE(nextWatermark(events, kStatsBatchSize, 1000u, 7000u, watermark));
  EXPECT_EQ(watermark, 7000u);  // falls back to the server's value
}

TEST(PageStatsCodec, EmptyBatchStopsWithoutChangingWatermark) {
  uint32_t watermark = 0;
  EXPECT_FALSE(nextWatermark({}, kStatsBatchSize, 1234u, 1234u, watermark));
  EXPECT_EQ(watermark, 1234u);
}
```

Create `test/bookorbit_page_stats/CMakeLists.txt`:

```cmake
add_executable(PageStatsCodecTest
  PageStatsCodecTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/PageStatsCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(PageStatsCodecTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(PageStatsCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PageStatsCodecTest)
```

Add `add_subdirectory(bookorbit_page_stats)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `PageStatsCodec.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/PageStatsCodec.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ReadingEvent.h"

namespace bookorbit {

// POST /koreader/plugin/page-stats accepts at most this many events.
inline constexpr size_t kStatsBatchSize = 500;

std::string encodePageStats(std::string_view hash, const std::vector<ReadingEvent>& events);

struct PageStatsAck {
  std::vector<std::string> unmatched;
  std::vector<std::pair<std::string, uint32_t>> watermarks;
};

bool decodePageStats(std::string_view json, PageStatsAck& out);

// Computes the watermark for the next round. Returns true when more events
// remain to send.
//
// A full batch may have been cut inside a group of events sharing one
// startTime, so the watermark backs off one second to re-fetch that boundary
// group. Re-sent events are idempotent server-side. Skipping this loses every
// event that shares a second with a batch boundary.
bool nextWatermark(const std::vector<ReadingEvent>& sent, size_t batchSize, uint32_t oldWatermark,
                   uint32_t serverWatermark, uint32_t& outWatermark);

}  // namespace bookorbit
```

Create `lib/BookOrbit/PageStatsCodec.cpp`:

```cpp
#include "PageStatsCodec.h"

#include "BookOrbitMatch.h"  // jsonEscape
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

struct AckCtx {
  PageStatsAck* out = nullptr;
  std::string key;
  bool inUnmatched = false;
  bool inResults = false;
  std::string hash;
  uint32_t watermark = 0;
  bool sawWatermark = false;
};

void onKey(void* raw, const char* key, const size_t len) {
  static_cast<AckCtx*>(raw)->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inUnmatched) {
    ctx->out->unmatched.emplace_back(value, len);
  } else if (ctx->inResults && ctx->key == "hash") {
    ctx->hash.assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults && ctx->key == "watermark") {
    ctx->watermark = static_cast<uint32_t>(strtoul(std::string(value, len).c_str(), nullptr, 10));
    ctx->sawWatermark = true;
  }
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->key == "unmatched") ctx->inUnmatched = true;
  else if (ctx->key == "results") ctx->inResults = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  ctx->inUnmatched = false;
  ctx->inResults = false;
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults) {
    ctx->hash.clear();
    ctx->watermark = 0;
    ctx->sawWatermark = false;
  }
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults && !ctx->hash.empty() && ctx->sawWatermark) {
    ctx->out->watermarks.emplace_back(ctx->hash, ctx->watermark);
  }
  ctx->key.clear();
}

void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

std::string encodePageStats(const std::string_view hash, const std::vector<ReadingEvent>& events) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += R"(","events":[)";
  for (size_t i = 0; i < events.size(); i++) {
    const auto& event = events[i];
    if (i > 0) json += ',';
    json += R"({"page":)" + std::to_string(event.page);
    json += R"(,"startTime":)" + std::to_string(event.startTime);
    json += R"(,"durationSeconds":)" + std::to_string(event.durationSeconds);
    json += R"(,"totalPages":)" + std::to_string(event.totalPages);
    json += '}';
  }
  json += "]}]}";
  return json;
}

bool decodePageStats(const std::string_view json, PageStatsAck& out) {
  out.unmatched.clear();
  out.watermarks.clear();

  AckCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.unmatched.clear();
    out.watermarks.clear();
    return false;
  }
  return true;
}

bool nextWatermark(const std::vector<ReadingEvent>& sent, const size_t batchSize, const uint32_t oldWatermark,
                   const uint32_t serverWatermark, uint32_t& outWatermark) {
  if (sent.empty()) {
    outWatermark = oldWatermark;
    return false;
  }

  if (sent.size() < batchSize) {
    outWatermark = serverWatermark;
    return false;
  }

  // Full batch: back off one second so a group sharing the cut timestamp is
  // re-fetched next round.
  const uint32_t lastStart = sent.back().startTime;
  const uint32_t backedOff = lastStart > 0 ? lastStart - 1 : 0;
  outWatermark = (backedOff <= oldWatermark) ? serverWatermark : backedOff;
  return true;
}

}  // namespace bookorbit
```

Add `#include <cstdlib>` for `strtoul`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R PageStatsCodec --output-on-failure
```

Expected: 9 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/PageStatsCodec.h lib/BookOrbit/PageStatsCodec.cpp test/bookorbit_page_stats test/CMakeLists.txt
git commit -m "feat: add page-stats codec with watermark back-off"
```

---

### Task 6: Statistics query — KOReader's formulas

**Files:**
- Create: `lib/BookOrbit/ReadingStatsQuery.h`, `lib/BookOrbit/ReadingStatsQuery.cpp`
- Create: `test/bookorbit_stats_query/CMakeLists.txt`, `test/bookorbit_stats_query/ReadingStatsQueryTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `ReadingEvent` (Task 1), `ClampSettings` (Task 2).
- Produces: `struct bookorbit::BookStats { uint32_t totalTimeUncapped; uint32_t totalTimeCapped; uint32_t pagesRead; uint32_t activeDays; float avgSecondsPerPage; float avgSecondsPerDay; }`;
  `BookStats bookorbit::computeBookStats(const std::vector<ReadingEvent>&, const ClampSettings&, int32_t utcOffsetSeconds)`;
  `uint32_t bookorbit::estimatedSecondsLeft(const BookStats&, uint32_t currentPage, uint32_t totalPages)`;
  `uint32_t bookorbit::currentStreakDays(const std::vector<ReadingEvent>&, uint32_t nowUnix, int32_t utcOffsetSeconds)`;
  `uint32_t bookorbit::longestStreakDays(const std::vector<ReadingEvent>&, int32_t utcOffsetSeconds)`.

**Capped time groups by page, not by event** — `min(sum(duration), maxSec)` per distinct page. This is KOReader's `STATISTICS_SQL_BOOK_CAPPED_TOTALS_QUERY` (`statistics.koplugin/main.lua:40-49`), and getting it wrong is the easiest way to produce numbers that look right but aren't.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_stats_query/ReadingStatsQueryTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <vector>

#include "lib/BookOrbit/ReadingStatsQuery.h"

using bookorbit::ClampSettings;
using bookorbit::computeBookStats;
using bookorbit::currentStreakDays;
using bookorbit::estimatedSecondsLeft;
using bookorbit::longestStreakDays;
using bookorbit::ReadingEvent;

namespace {

ReadingEvent ev(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

constexpr uint32_t kDay = 86400;

}  // namespace

TEST(ReadingStatsQuery, SumsUncappedTime) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 30), ev(2, 1030, 40)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 70u);
}

TEST(ReadingStatsQuery, CountsDistinctPages) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 30), ev(1, 2000, 40), ev(2, 3000, 20)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.pagesRead, 2u);
}

// KOReader caps the SUM per page, not each event. Two 90s visits to one page
// total 180s uncapped but only 120s capped.
TEST(ReadingStatsQuery, CapsPerPageNotPerEvent) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 90), ev(1, 2000, 90)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 180u);
  EXPECT_EQ(stats.totalTimeCapped, 120u);
}

TEST(ReadingStatsQuery, CapsEachPageIndependently) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 90), ev(1, 2000, 90), ev(2, 3000, 30)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeCapped, 150u);  // 120 + 30
}

TEST(ReadingStatsQuery, AverageTimePerPageUsesCappedTotals) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60), ev(2, 2000, 40)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_FLOAT_EQ(stats.avgSecondsPerPage, 50.0f);
}

TEST(ReadingStatsQuery, CountsDistinctActiveDays) {
  const std::vector<ReadingEvent> events{ev(1, 0, 30), ev(2, 100, 30), ev(3, kDay, 30)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(stats.activeDays, 2u);
}

TEST(ReadingStatsQuery, ActiveDaysHonourUtcOffset) {
  // 23:30 UTC is the next local day at +01:00.
  const std::vector<ReadingEvent> events{ev(1, 84600, 30), ev(2, 84700, 30)};
  const auto utc = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(utc.activeDays, 1u);
  const auto shifted = computeBookStats(events, ClampSettings{}, 3600);
  EXPECT_EQ(shifted.activeDays, 1u);
}

TEST(ReadingStatsQuery, EmptyLogYieldsZeroes) {
  const auto stats = computeBookStats({}, ClampSettings{}, 0);
  EXPECT_EQ(stats.totalTimeUncapped, 0u);
  EXPECT_EQ(stats.pagesRead, 0u);
  EXPECT_FLOAT_EQ(stats.avgSecondsPerPage, 0.0f);
}

TEST(ReadingStatsQuery, EstimatesTimeLeftFromAveragePace) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60), ev(2, 2000, 60)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(estimatedSecondsLeft(stats, 10, 20), 600u);  // 10 pages * 60s
}

TEST(ReadingStatsQuery, TimeLeftIsZeroAtEndOfBook) {
  const std::vector<ReadingEvent> events{ev(1, 1000, 60)};
  const auto stats = computeBookStats(events, ClampSettings{}, 0);
  EXPECT_EQ(estimatedSecondsLeft(stats, 20, 20), 0u);
}

TEST(ReadingStatsQuery, CurrentStreakCountsConsecutiveDaysEndingToday) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 2 * kDay, 30), ev(2, today - kDay, 30), ev(3, today, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 3u);
}

TEST(ReadingStatsQuery, CurrentStreakBreaksOnAGap) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 5 * kDay, 30), ev(2, today, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 1u);
}

TEST(ReadingStatsQuery, CurrentStreakIsZeroWhenNotReadRecently) {
  const uint32_t today = 10 * kDay;
  const std::vector<ReadingEvent> events{ev(1, today - 5 * kDay, 30)};
  EXPECT_EQ(currentStreakDays(events, today, 0), 0u);
}

TEST(ReadingStatsQuery, LongestStreakFindsBestRun) {
  const std::vector<ReadingEvent> events{
      ev(1, 1 * kDay, 30), ev(2, 2 * kDay, 30), ev(3, 3 * kDay, 30),  // run of 3
      ev(4, 9 * kDay, 30),                                            // gap, run of 1
  };
  EXPECT_EQ(longestStreakDays(events, 0), 3u);
}
```

Create `test/bookorbit_stats_query/CMakeLists.txt`:

```cmake
add_executable(ReadingStatsQueryTest
  ReadingStatsQueryTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingStatsQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(ReadingStatsQueryTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(ReadingStatsQueryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingStatsQueryTest)
```

Add `add_subdirectory(bookorbit_stats_query)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `ReadingStatsQuery.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/ReadingStatsQuery.h`:

```cpp
#pragma once

#include <cstdint>
#include <vector>

#include "ClampPolicy.h"
#include "ReadingEvent.h"

namespace bookorbit {

struct BookStats {
  uint32_t totalTimeUncapped = 0;
  uint32_t totalTimeCapped = 0;
  uint32_t pagesRead = 0;      // distinct pages
  uint32_t activeDays = 0;     // distinct local calendar days
  float avgSecondsPerPage = 0.0f;
  float avgSecondsPerDay = 0.0f;
};

// KOReader's book statistics, computed over the raw event log.
//
// The capped total groups by PAGE, not by event: min(sum(duration), maxSec)
// per distinct page, matching STATISTICS_SQL_BOOK_CAPPED_TOTALS_QUERY in
// statistics.koplugin/main.lua:40-49. Averages derive from the capped totals.
BookStats computeBookStats(const std::vector<ReadingEvent>& events, const ClampSettings& settings,
                           int32_t utcOffsetSeconds);

// (totalPages - currentPage) * avgSecondsPerPage.
uint32_t estimatedSecondsLeft(const BookStats& stats, uint32_t currentPage, uint32_t totalPages);

// Consecutive local days with reading, ending today. Zero when today has none.
uint32_t currentStreakDays(const std::vector<ReadingEvent>& events, uint32_t nowUnix, int32_t utcOffsetSeconds);

uint32_t longestStreakDays(const std::vector<ReadingEvent>& events, int32_t utcOffsetSeconds);

}  // namespace bookorbit
```

Implement `lib/BookOrbit/ReadingStatsQuery.cpp` as follows. Build a
`std::vector<std::pair<uint32_t,uint32_t>>` of `(page, summedDuration)` and a
sorted unique vector of local day numbers — `std::map` is avoided because heap
churn on the C3 matters and the vectors stay small.

```cpp
#include "ReadingStatsQuery.h"

#include <algorithm>

namespace bookorbit {
namespace {

uint32_t localDay(const uint32_t startTime, const int32_t utcOffsetSeconds) {
  const int64_t shifted = static_cast<int64_t>(startTime) + utcOffsetSeconds;
  if (shifted < 0) return 0;
  return static_cast<uint32_t>(shifted / 86400);
}

std::vector<uint32_t> sortedUniqueDays(const std::vector<ReadingEvent>& events,
                                       const int32_t utcOffsetSeconds) {
  std::vector<uint32_t> days;
  days.reserve(events.size());
  for (const auto& event : events) {
    days.push_back(localDay(event.startTime, utcOffsetSeconds));
  }
  std::sort(days.begin(), days.end());
  days.erase(std::unique(days.begin(), days.end()), days.end());
  return days;
}

}  // namespace

BookStats computeBookStats(const std::vector<ReadingEvent>& events, const ClampSettings& settings,
                           const int32_t utcOffsetSeconds) {
  BookStats stats;
  if (events.empty()) return stats;

  std::vector<std::pair<uint32_t, uint32_t>> perPage;  // (page, summed duration)
  perPage.reserve(events.size());

  for (const auto& event : events) {
    stats.totalTimeUncapped += event.durationSeconds;
    const auto it = std::find_if(perPage.begin(), perPage.end(),
                                 [&](const auto& entry) { return entry.first == event.page; });
    if (it == perPage.end()) {
      perPage.emplace_back(event.page, event.durationSeconds);
    } else {
      it->second += event.durationSeconds;
    }
  }

  stats.pagesRead = static_cast<uint32_t>(perPage.size());
  for (const auto& [page, summed] : perPage) {
    (void)page;
    stats.totalTimeCapped += std::min<uint32_t>(summed, settings.maxSec);
  }

  stats.activeDays = static_cast<uint32_t>(sortedUniqueDays(events, utcOffsetSeconds).size());

  if (stats.pagesRead > 0) {
    stats.avgSecondsPerPage =
        static_cast<float>(stats.totalTimeCapped) / static_cast<float>(stats.pagesRead);
  }
  if (stats.activeDays > 0) {
    stats.avgSecondsPerDay =
        static_cast<float>(stats.totalTimeCapped) / static_cast<float>(stats.activeDays);
  }
  return stats;
}

uint32_t estimatedSecondsLeft(const BookStats& stats, const uint32_t currentPage, const uint32_t totalPages) {
  if (totalPages <= currentPage) return 0;
  const uint32_t remaining = totalPages - currentPage;
  return static_cast<uint32_t>(static_cast<float>(remaining) * stats.avgSecondsPerPage);
}

uint32_t currentStreakDays(const std::vector<ReadingEvent>& events, const uint32_t nowUnix,
                           const int32_t utcOffsetSeconds) {
  const auto days = sortedUniqueDays(events, utcOffsetSeconds);
  if (days.empty()) return 0;

  const uint32_t today = localDay(nowUnix, utcOffsetSeconds);
  if (days.back() != today) return 0;

  uint32_t streak = 1;
  for (size_t i = days.size() - 1; i > 0; i--) {
    if (days[i] - days[i - 1] != 1) break;
    streak++;
  }
  return streak;
}

uint32_t longestStreakDays(const std::vector<ReadingEvent>& events, const int32_t utcOffsetSeconds) {
  const auto days = sortedUniqueDays(events, utcOffsetSeconds);
  if (days.empty()) return 0;

  uint32_t best = 1;
  uint32_t run = 1;
  for (size_t i = 1; i < days.size(); i++) {
    run = (days[i] - days[i - 1] == 1) ? run + 1 : 1;
    best = std::max(best, run);
  }
  return best;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R ReadingStatsQuery --output-on-failure
```

Expected: 14 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/ReadingStatsQuery.h lib/BookOrbit/ReadingStatsQuery.cpp test/bookorbit_stats_query test/CMakeLists.txt
git commit -m "feat: add KOReader-equivalent reading statistics"
```

---

### Task 7: Ground-truth verification against real KOReader data

**Files:**
- Create: `test/bookorbit_stats_ground_truth/CMakeLists.txt`, `test/bookorbit_stats_ground_truth/StatsGroundTruthTest.cpp`
- Create: `test/bookorbit_stats_ground_truth/export_ground_truth.sh`
- Create: `test/bookorbit_stats_ground_truth/fixtures/` (generated, committed)
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `computeBookStats` (Task 6), `ReadingEvent` (Task 1).
- Produces: no API. This task exists to prove the formulas match the real implementation.

**Why this matters:** verifying against a reading of KOReader's SQL is weaker than verifying against KOReader's SQL *executing*. A real `statistics.sqlite3` with **2027 events across 11 books** is available at `/home/monish/repos/koreader/settings/statistics.sqlite3`.

- [ ] **Step 1: Write the export script**

Create `test/bookorbit_stats_ground_truth/export_ground_truth.sh`:

```bash
#!/usr/bin/env bash
# Exports events and KOReader's own computed statistics from a real
# statistics.sqlite3, so the C++ implementation can be checked against numbers
# SQLite produced rather than against our reading of the SQL.
#
# Usage: ./export_ground_truth.sh <path-to-statistics.sqlite3> <out-dir>
set -euo pipefail

DB="${1:?path to statistics.sqlite3 required}"
OUT="${2:?output directory required}"
MAX_SEC=120

mkdir -p "$OUT"

# Raw events, ordered as the uploader orders them.
sqlite3 -noheader -csv "$DB" "
  SELECT id_book, page, start_time, duration, total_pages
  FROM page_stat_data
  WHERE total_pages > 0
  ORDER BY id_book, start_time, page;
" > "$OUT/events.csv"

# KOReader's own answers, straight from its SQL.
# Capped totals group by page, per STATISTICS_SQL_BOOK_CAPPED_TOTALS_QUERY.
sqlite3 -noheader -csv "$DB" "
  SELECT b.id,
         (SELECT count(DISTINCT page) FROM page_stat_data WHERE id_book = b.id AND total_pages > 0),
         (SELECT sum(duration)        FROM page_stat_data WHERE id_book = b.id AND total_pages > 0),
         (SELECT sum(capped) FROM (
             SELECT min(sum(duration), $MAX_SEC) AS capped
             FROM page_stat_data WHERE id_book = b.id AND total_pages > 0
             GROUP BY page)),
         (SELECT count(DISTINCT date(start_time, 'unixepoch'))
            FROM page_stat_data WHERE id_book = b.id AND total_pages > 0)
  FROM book b
  ORDER BY b.id;
" > "$OUT/expected.csv"

echo "wrote $OUT/events.csv ($(wc -l < "$OUT/events.csv") rows)"
echo "wrote $OUT/expected.csv ($(wc -l < "$OUT/expected.csv") rows)"
```

Note the SQL uses `date(start_time,'unixepoch')` in UTC, so the test passes
`utcOffsetSeconds = 0` to match.

- [ ] **Step 2: Generate the fixtures and verify they are non-trivial**

```bash
chmod +x test/bookorbit_stats_ground_truth/export_ground_truth.sh
./test/bookorbit_stats_ground_truth/export_ground_truth.sh \
  /home/monish/repos/koreader/settings/statistics.sqlite3 \
  test/bookorbit_stats_ground_truth/fixtures
wc -l test/bookorbit_stats_ground_truth/fixtures/*.csv
```

Expected: `events.csv` has ~2027 rows, `expected.csv` ~11 rows. **If either is
empty, stop** — the test would pass vacuously and prove nothing.

- [ ] **Step 3: Write the test**

Create `test/bookorbit_stats_ground_truth/StatsGroundTruthTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "lib/BookOrbit/ReadingStatsQuery.h"

using bookorbit::ClampSettings;
using bookorbit::computeBookStats;
using bookorbit::ReadingEvent;

namespace {

struct Expected {
  uint32_t pagesRead = 0;
  uint32_t uncapped = 0;
  uint32_t capped = 0;
  uint32_t activeDays = 0;
};

std::vector<std::string> splitCsv(const std::string& line) {
  std::vector<std::string> fields;
  std::stringstream stream(line);
  std::string field;
  while (std::getline(stream, field, ',')) fields.push_back(field);
  return fields;
}

std::string fixturePath(const char* name) {
  return std::string(BOOKORBIT_GROUND_TRUTH_DIR) + "/" + name;
}

std::map<int, std::vector<ReadingEvent>> loadEvents() {
  std::map<int, std::vector<ReadingEvent>> byBook;
  std::ifstream file(fixturePath("events.csv"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    const auto fields = splitCsv(line);
    if (fields.size() < 5) continue;
    ReadingEvent event;
    event.page = static_cast<uint32_t>(std::stoul(fields[1]));
    event.startTime = static_cast<uint32_t>(std::stoul(fields[2]));
    event.durationSeconds = static_cast<uint16_t>(std::stoul(fields[3]));
    event.totalPages = static_cast<uint16_t>(std::stoul(fields[4]));
    byBook[std::stoi(fields[0])].push_back(event);
  }
  return byBook;
}

std::map<int, Expected> loadExpected() {
  std::map<int, Expected> expected;
  std::ifstream file(fixturePath("expected.csv"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    const auto fields = splitCsv(line);
    if (fields.size() < 5 || fields[1].empty()) continue;
    Expected row;
    row.pagesRead = static_cast<uint32_t>(std::stoul(fields[1]));
    row.uncapped = fields[2].empty() ? 0 : static_cast<uint32_t>(std::stoul(fields[2]));
    row.capped = fields[3].empty() ? 0 : static_cast<uint32_t>(std::stoul(fields[3]));
    row.activeDays = static_cast<uint32_t>(std::stoul(fields[4]));
    expected[std::stoi(fields[0])] = row;
  }
  return expected;
}

}  // namespace

// Guards against a vacuous pass: if the fixtures are missing or empty, every
// other assertion below would trivially hold.
TEST(StatsGroundTruth, FixturesAreLoaded) {
  const auto events = loadEvents();
  const auto expected = loadExpected();
  ASSERT_FALSE(events.empty()) << "events.csv missing — run export_ground_truth.sh";
  ASSERT_FALSE(expected.empty()) << "expected.csv missing — run export_ground_truth.sh";

  size_t total = 0;
  for (const auto& [id, list] : events) {
    (void)id;
    total += list.size();
  }
  EXPECT_GT(total, 1000u) << "expected roughly 2027 real events";
}

// The real check: our C++ must agree with numbers SQLite actually produced.
TEST(StatsGroundTruth, MatchesKOReaderSqlForEveryBook) {
  const auto events = loadEvents();
  const auto expected = loadExpected();

  ClampSettings settings;  // maxSec = 120, matching MAX_SEC in the export script
  size_t compared = 0;

  for (const auto& [bookId, expectedRow] : expected) {
    const auto it = events.find(bookId);
    if (it == events.end()) continue;

    const auto stats = computeBookStats(it->second, settings, /*utcOffsetSeconds=*/0);
    EXPECT_EQ(stats.pagesRead, expectedRow.pagesRead) << "pagesRead mismatch for book " << bookId;
    EXPECT_EQ(stats.totalTimeUncapped, expectedRow.uncapped) << "uncapped mismatch for book " << bookId;
    EXPECT_EQ(stats.totalTimeCapped, expectedRow.capped) << "capped mismatch for book " << bookId;
    EXPECT_EQ(stats.activeDays, expectedRow.activeDays) << "activeDays mismatch for book " << bookId;
    compared++;
  }

  EXPECT_GT(compared, 0u) << "no books compared — fixture book ids do not line up";
}
```

Create `test/bookorbit_stats_ground_truth/CMakeLists.txt`:

```cmake
add_executable(StatsGroundTruthTest
  StatsGroundTruthTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingStatsQuery.cpp
  ${REPO_ROOT}/lib/BookOrbit/ClampPolicy.cpp
  ${REPO_ROOT}/lib/BookOrbit/ReadingEvent.cpp
)

target_include_directories(StatsGroundTruthTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_compile_definitions(StatsGroundTruthTest PRIVATE
  BOOKORBIT_GROUND_TRUTH_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(StatsGroundTruthTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StatsGroundTruthTest)
```

Add `add_subdirectory(bookorbit_stats_ground_truth)` to `test/CMakeLists.txt`.

- [ ] **Step 4: Run and reconcile**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
ctest --test-dir /tmp/crossink-tests -R StatsGroundTruth --output-on-failure
```

Expected: both tests PASS. **A mismatch here means Task 6's formulas are wrong,
not that the fixture is wrong** — fix `ReadingStatsQuery.cpp` and re-run. Do not
adjust the expected CSV to make the test pass.

- [ ] **Step 5: Commit**

```bash
git add test/bookorbit_stats_ground_truth test/CMakeLists.txt
git commit -m "test: verify reading statistics against real KOReader data"
```

---

### Task 8: Phase orchestrator and outbox

**Files:**
- Create: `lib/BookOrbit/BookOrbitOutbox.h`, `lib/BookOrbit/BookOrbitOutbox.cpp`
- Create: `test/bookorbit_outbox/CMakeLists.txt`, `test/bookorbit_outbox/BookOrbitOutboxTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `SyncStateStore` (P0 Task 5), `Error`/`Status` (P0 Task 2).
- Produces: `enum class bookorbit::SyncPhase { Match, Stats, Progress, State, Annotations, Bookmarks, Done }`;
  `class bookorbit::SyncOutbox` with
  `SyncPhase currentPhase() const`,
  `bool advance(const Error& phaseResult)`,
  `bool acknowledge(SyncPhase, SyncStateStore&)`,
  `bool isAborted() const`,
  `SyncPhase nextPhase(SyncPhase)`.

This was deferred from P0 because a phase-ack machine with one phase to sequence would have been speculative. It now has two real phases and lands here.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_outbox/BookOrbitOutboxTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitError.h"
#include "lib/BookOrbit/BookOrbitOutbox.h"

using bookorbit::classify;
using bookorbit::SyncOutbox;
using bookorbit::SyncPhase;

TEST(BookOrbitOutbox, StartsAtMatch) {
  const SyncOutbox outbox;
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Match);
}

TEST(BookOrbitOutbox, PhaseOrderMatchesSpec) {
  SyncOutbox outbox;
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Match), SyncPhase::Stats);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Stats), SyncPhase::Progress);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Progress), SyncPhase::State);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::State), SyncPhase::Annotations);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Annotations), SyncPhase::Bookmarks);
  EXPECT_EQ(outbox.nextPhase(SyncPhase::Bookmarks), SyncPhase::Done);
}

TEST(BookOrbitOutbox, SuccessAdvancesToNextPhase) {
  SyncOutbox outbox;
  ASSERT_TRUE(outbox.advance(classify(200, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Stats);
}

// An auth failure aborts the WHOLE sync, not just this phase.
TEST(BookOrbitOutbox, AuthErrorAbortsEntireSync) {
  SyncOutbox outbox;
  EXPECT_FALSE(outbox.advance(classify(401, false)));
  EXPECT_TRUE(outbox.isAborted());
}

TEST(BookOrbitOutbox, TransportErrorAbortsEntireSync) {
  SyncOutbox outbox;
  EXPECT_FALSE(outbox.advance(classify(0, true)));
  EXPECT_TRUE(outbox.isAborted());
}

// A non-auth HTTP error fails just this phase and moves on. The watermark
// stays unadvanced, so the data retries on the next sync trigger.
TEST(BookOrbitOutbox, ServerErrorSkipsPhaseButContinues) {
  SyncOutbox outbox;
  ASSERT_TRUE(outbox.advance(classify(500, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Stats);
  EXPECT_FALSE(outbox.isAborted());
  EXPECT_TRUE(outbox.hadErrors());
}

TEST(BookOrbitOutbox, RunsToCompletion) {
  SyncOutbox outbox;
  for (int i = 0; i < 6; i++) {
    ASSERT_TRUE(outbox.advance(classify(200, false))) << "stopped at step " << i;
  }
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Done);
  EXPECT_FALSE(outbox.hadErrors());
}

TEST(BookOrbitOutbox, AdvanceAfterDoneIsANoOp) {
  SyncOutbox outbox;
  for (int i = 0; i < 6; i++) outbox.advance(classify(200, false));
  EXPECT_FALSE(outbox.advance(classify(200, false)));
  EXPECT_EQ(outbox.currentPhase(), SyncPhase::Done);
}
```

Create `test/bookorbit_outbox/CMakeLists.txt`:

```cmake
add_executable(BookOrbitOutboxTest
  BookOrbitOutboxTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitOutbox.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitOutboxTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitOutboxTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitOutboxTest)
```

Add `add_subdirectory(bookorbit_outbox)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitOutbox.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitOutbox.h`:

```cpp
#pragma once

#include "BookOrbitError.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

enum class SyncPhase {
  Match,
  Stats,
  Progress,
  State,
  Annotations,
  Bookmarks,
  Done,
};

// Sequences one book's sync phases, persisting each acknowledgement before
// advancing so a crash can never skip a watermark.
//
// There is deliberately no retry loop. A failed phase leaves its watermark
// unadvanced and retries on the next sync trigger; on a battery device a timed
// retry loop is the wrong default.
class SyncOutbox {
 public:
  SyncPhase currentPhase() const { return phase; }
  bool isAborted() const { return aborted; }
  bool hadErrors() const { return errors; }

  // Applies one phase's outcome. Returns true when the sync should continue.
  // Auth and transport failures abort everything; other errors skip just this
  // phase.
  bool advance(const Error& phaseResult);

  // Persists state before the phase transition is considered durable.
  bool acknowledge(SyncPhase completed, SyncStateStore& state);

  SyncPhase nextPhase(SyncPhase from) const;

 private:
  SyncPhase phase = SyncPhase::Match;
  bool aborted = false;
  bool errors = false;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitOutbox.cpp`:

```cpp
#include "BookOrbitOutbox.h"

namespace bookorbit {

SyncPhase SyncOutbox::nextPhase(const SyncPhase from) const {
  switch (from) {
    case SyncPhase::Match: return SyncPhase::Stats;
    case SyncPhase::Stats: return SyncPhase::Progress;
    case SyncPhase::Progress: return SyncPhase::State;
    case SyncPhase::State: return SyncPhase::Annotations;
    case SyncPhase::Annotations: return SyncPhase::Bookmarks;
    case SyncPhase::Bookmarks: return SyncPhase::Done;
    case SyncPhase::Done: return SyncPhase::Done;
  }
  return SyncPhase::Done;
}

bool SyncOutbox::advance(const Error& phaseResult) {
  if (aborted || phase == SyncPhase::Done) return false;

  if (isAuthError(phaseResult) || phaseResult.status == Status::Transport) {
    aborted = true;
    errors = true;
    return false;
  }

  if (phaseResult.status != Status::Ok) {
    errors = true;  // watermark stays unadvanced; retried next trigger
  }

  phase = nextPhase(phase);
  return true;
}

bool SyncOutbox::acknowledge(const SyncPhase completed, SyncStateStore& state) {
  (void)completed;
  // The durability guarantee: state hits disk before the phase is considered
  // complete, so a crash re-runs the phase rather than skipping it.
  return state.flush();
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitOutbox --output-on-failure
```

Expected: 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitOutbox.h lib/BookOrbit/BookOrbitOutbox.cpp test/bookorbit_outbox test/CMakeLists.txt
git commit -m "feat: add BookOrbit sync phase orchestrator"
```

---

### Task 9: Reader hook and RTC gating

**Files:**
- Modify: `src/activities/reader/EpubReaderActivity.cpp` at `recordCurrentPageReadingTime()` (~line 1481) and the forward/back turn paths (~line 5347-5383)
- Modify: `src/activities/reader/EpubReaderActivity.h` (hold the log and a pending-event buffer)
- Create: `src/activities/reader/ReadingEventRecorder.{h,cpp}` (the glue, so the activity does not grow further)
- Modify: `src/CrossPointSettings.{h,cpp}` (expose `minSec`/`maxSec`)

**Interfaces:**
- Consumes: `ReadingEventLog` (Task 3), `ClampSettings`/`clampDwell` (Task 2), `PageResolver` (Task 4), `HalClock`.
- Produces: `class ReadingEventRecorder` with
  `void onPageShown(uint32_t nowMillis)`,
  `void onPageLeft(uint32_t nowMillis, const bookorbit::PageSource&)`,
  `bool flush()`,
  `void backfillPendingFromClock(uint32_t nowUnix)`.

**RTC gating.** `HalClock::isAvailable()` gates wall-clock time. Events recorded without a valid clock are buffered with a monotonic marker and backfilled on the next NTP sync. If no valid time arrives within `kBackfillWindowEvents = 200` buffered events, they are **discarded rather than uploaded with a fabricated `startTime`** — a wrong timestamp corrupts every derived statistic and cannot be undone server-side.

`EpubReaderActivity.cpp` is already large; putting the glue in its own file keeps the change reviewable and follows `AGENTS.md`'s guidance to keep readers focused on interaction.

- [ ] **Step 1: Add the clamp settings**

In `src/CrossPointSettings.h`, beside the existing
`readingIdleTimeThresholdUnits`, add `readingMinSec` (default 5) and
`readingMaxSec` (default 120), with accessors mirroring
`getReadingIdleTimeThresholdSeconds()`. The existing idle threshold stays — it
still governs CrossInk's legacy aggregate counters, which Task 10 preserves as a
separate baseline.

- [ ] **Step 2: Write the recorder**

`onPageLeft` computes `elapsed = (nowMillis - shownAtMillis) / 1000`, applies
`clampDwell`, resolves the page via `resolvePage`, and appends a `ReadingEvent`.
When `HalClock::isAvailable()` is false, the event is held in a pending buffer
with `startTime = 0` and a monotonic offset instead.

```cpp
void ReadingEventRecorder::onPageLeft(const uint32_t nowMillis, const bookorbit::PageSource& source) {
  if (shownAtMillis == 0) return;

  const uint32_t elapsedSeconds = (nowMillis - shownAtMillis) / 1000;
  uint16_t clamped = 0;
  if (!bookorbit::clampDwell(elapsedSeconds, settings, clamped)) {
    shownAtMillis = 0;
    return;  // below min_sec: discarded, exactly as KOReader does
  }

  bookorbit::ReadingEvent event;
  if (!bookorbit::resolvePage(source, event.page, event.totalPages)) {
    shownAtMillis = 0;
    return;
  }
  event.durationSeconds = clamped;

  if (!CLOCK.isAvailable()) {
    // No trustworthy wall clock. Hold the event rather than invent a
    // timestamp; a fabricated startTime corrupts every derived statistic.
    if (pendingUnclocked.size() < kBackfillWindowEvents) {
      pendingUnclocked.push_back({event, nowMillis});
    }
    shownAtMillis = 0;
    return;
  }

  event.startTime = CLOCK.nowUnix() - clamped;  // the moment the page was reached
  log.append(event);
  shownAtMillis = 0;
}
```

- [ ] **Step 3: Wire into the page-turn paths**

At `EpubReaderActivity.cpp:5347-5382` (forward turn) and the back-turn path
below it, call `recorder.onPageLeft(millis(), buildPageSource())` alongside the
existing `recordCurrentPageReadingTime()`, then `recorder.onPageShown(millis())`
after the new page renders. `buildPageSource()` populates `PageSource` from
`epub->hasStablePageNumbers()`, `epub->resolveReferencePage(...)`,
`epub->calculateSizeProgress(...)`, and `epub->getBookSize()`.

Call `recorder.flush()` in `onExit()`, beside the existing stats persistence, so
a clean close never loses buffered events.

- [ ] **Step 4: Verify builds and the smoke test**

```bash
pio run -e x4-pro && pio run -e default && pio run -e simulator
./scripts/run_simulator_smoke_test.py --page-turns 60
```

Expected: all build; the smoke test turns 60 pages (crossing the 50-event
auto-flush boundary) with no crash.

- [ ] **Step 5: Commit**

```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/activities/reader src/CrossPointSettings.h src/CrossPointSettings.cpp
git commit -m "feat: record reading events on page turns"
```

---

### Task 10: Stats upload phase and on-device display

**Files:**
- Create: `src/activities/bookorbit/BookOrbitStatsSync.{h,cpp}`
- Modify: `src/activities/reader/BookStatsView.cpp` (read from the event log)
- Modify: `lib/I18n/translations/en.yaml`, then regenerate
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: everything above, plus `BookOrbitClient` (P0 Task 6) and `SyncStateStore` (P0 Task 5).
- Produces: `bool uploadPageStats(BookOrbitClient&, ReadingEventLog&, SyncStateStore&, std::string_view md5, Error& outError)`.

- [ ] **Step 1: Write the upload loop**

Read `readAfter(book.statsWatermark, kStatsBatchSize, events)`, encode, POST to
`/koreader/plugin/page-stats` wrapped in `BookOrbitClient::withDevice(...)`,
decode the ack, then apply `nextWatermark(...)`. Loop while it returns true.
A hash reported `unmatched` does **not** advance its watermark.

- [ ] **Step 2: Add the legacy-baseline separation**

`BookStatsView` gains log-derived figures via `computeBookStats`. CrossInk's
pre-existing `global_stats.bin` totals were accumulated under the old 2 s/300 s
rules and must not be summed with log-derived ones. Display them as a separate
labelled line:

```yaml
STR_STATS_BEFORE_EVENT_LOG: "Before event logging"
STR_BOOKORBIT_STATS_UPLOADED: "Statistics uploaded"
STR_BOOKORBIT_STATS_PENDING: "%1 events waiting to upload"
```

```bash
python3 scripts/gen_i18n.py
```

- [ ] **Step 3: Verify**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
pio run -e x4-pro && pio run -e default
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
```

- [ ] **Step 4: Changelog**

```markdown
### Added
- Reading event log recording per-page reading time, with statistics matching KOReader's.
- BookOrbit statistics sync: reading events upload to a BookOrbit server incrementally.

### Changed
- Reading time now follows KOReader's rules: dwells under 5 seconds are ignored, and dwells over 120 seconds count as 120 rather than being discarded. Totals recorded before this change are shown separately.
```

- [ ] **Step 5: Commit**

```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/activities lib/I18n CHANGELOG.md
git commit -m "feat: upload reading events to BookOrbit and show log-derived stats"
```

---

## Hardware Verification

On an X4 Pro with an SD card and a configured BookOrbit server:

1. Open a book, read ~10 pages at a normal pace. Confirm `/.crosspoint/epub_<hash>/events.bin` appears and grows by 16 bytes per qualifying turn.
2. Turn a page within 4 seconds. Confirm the file does **not** grow — below `min_sec`.
3. Leave a page open for 5 minutes, then turn. Confirm one event is added with `durationSeconds == 120`, not 300 and not absent. This is the clamp behaviour.
4. Read 60 pages and pull the battery without a clean exit. On reboot, confirm at most 10 events were lost (the unflushed tail) and the log still parses.
5. Change the reader font size, then read more. Confirm `page`/`totalPages` in new events are on the same scale as the old ones — this is what reference pages buy.
6. Trigger a sync. Confirm the server receives the events and `statsWatermark` advances.
7. Sync again immediately with no new reading. Confirm no events are re-sent.
8. Boot with a dead RTC. Confirm events buffer rather than upload, and that no event carries a 1970 timestamp.

## Self-Review Notes

- **Spec coverage:** event record (Task 1), clamping (Task 2), append-only log with 50-event flush (Task 3), stable pages plus the 2048-byte fallback (Task 4), 500-event batches and the one-second back-off (Task 5), KOReader formulas including the per-page cap (Task 6), ground truth (Task 7), phase chain deferred from P0 (Task 8), reader hook and RTC gating (Task 9), upload and legacy-baseline separation (Task 10).
- **Placeholder scan:** Tasks 9 and 10 modify existing large files, so they give the exact call sites and the decisive code fragments rather than whole-file rewrites; every new unit is shown in full.
- **Type consistency:** `ReadingEvent` (Task 1) flows unchanged through Tasks 3, 5, 6, 7, 9. `ClampSettings` (Task 2) is used in Tasks 6, 7, 9. `kStatsBatchSize` (Task 5) is the batch size in Task 10's loop. `PageSource` (Task 4) is built in Task 9.
- **P0 change required:** Task 3 adds `append()` to `IBlobStore`, which means updating the two P0 test fakes. Called out in that task's step 3 so it is not discovered as a surprise build break.
