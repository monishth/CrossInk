# BookOrbit P3 — Book States Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Sync reading status, star rating, and review note two-way with BookOrbit through `POST /koreader/plugin/book-states`, mapping the wire `status` field onto CrossInk's existing `isCompleted` flag and "mark as finished" flow, and adding a 1–5 star rating UI plus date-only per-field conflict resolution.

**Architecture:** The codec, the conflict resolver, the durable local-state record, and the phase driver all live in `lib/BookOrbit/` behind P0's injected `IHttpTransport` / `IBlobStore` interfaces, so every one of them compiles and runs under the native GoogleTest suite with no Arduino dependency. Only three `src/` files change, all at existing "mark as finished" call sites, plus one new reader-menu rating flow.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), `lib/JsonParser/StreamingJsonParser` for response parsing, P0's `bookorbit::BookOrbitClient` for HTTP.

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md` — section "P3 — Book states".

**Depends on:** `docs/superpowers/plans/2026-09-11-bookorbit-p0-client-and-state.md` (Tasks 1–7 must be merged first).

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
- Every `/koreader/plugin/*` POST body additionally carries `deviceId`, `deviceModel`, `pluginVersion`, and `deviceTime` (local `%Y-%m-%d %H:%M:%S`) — injected by `BookOrbitClient::withDevice`, never by this phase's encoder.
- Batch limit for this phase: **200 books** (`kBookStateBatchSize = 200`).
- Wire status vocabulary is exactly `"reading" | "complete" | "abandoned"`. Anything else is dropped, never guessed.
- `statusModified` and `reviewModified` are date-only `YYYY-MM-DD` strings. Conflict rule: the side with the newer date wins; **a tie prefers the server**.
- Clearing is explicit: `ratingCleared: true` / `reviewCleared: true`. An absent field means "unchanged", never "cleared".
- `rating` is an integer 1–5. Values outside that range are dropped, not clamped silently at the wire boundary.
- Truncation limits, matching the Lua plugin: `reviewNote` ≤ 10000 bytes.
- Error convention: non-2xx yields `(status, decodedBody)`; transport failure yields a transport error. `401`/`403` and transport errors abort the whole sync; other numeric errors mark the phase failed, leave its watermark unadvanced, and move on. **No retry loops.**
- Per-book phase chain order is `match → stats → progress → state → annotations → bookmarks`. The state phase is durably acknowledged to disk before `annotations` starts.
- Do not edit generated files: `src/network/html/*.generated.h`, `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`, icon headers, hyphenation tries.
- I18n source of truth is `lib/I18n/translations/english.yaml`, regenerated with `python3 scripts/gen_i18n.py`.
- Add a `CHANGELOG.md` entry for user-facing changes, grouped under Added/Changed/Fixed.
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally: `pio run -e x4-pro` and `pio run -e default`.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/BookOrbitDate.{h,cpp}` | Date-only (`YYYY-MM-DD`) parse / format / compare, and the newer-date-wins resolver. |
| `lib/BookOrbit/BookOrbitBookState.{h,cpp}` | `BookStatus` vocabulary, `LocalBookState`, completion-flag mapping, rating/review normalization. |
| `lib/BookOrbit/BookStateCodec.{h,cpp}` | `book-states` request encode, response decode, and change detection (`buildStatePayload`). |
| `lib/BookOrbit/BookStateMerge.{h,cpp}` | Per-field conflict resolution between a local record and one server result. |
| `lib/BookOrbit/BookStateStore.{h,cpp}` | Durable per-book local state plus the last-known-server shadow, atomically written. |
| `lib/BookOrbit/BookStatePhase.{h,cpp}` | Drives one book's state phase: build → POST → decode → merge → persist. |
| `src/activities/reader/BookRatingMenuModel.h` | Pure option-list model for the reader menu's rating picker. Host-tested. |
| `src/activities/reader/EpubReaderMenuActivity.cpp` | Adds the rating row to the Settings tab. |
| `src/activities/reader/EpubReaderActivity.cpp` | Rating popup; stamps `statusModified` in `setBookCompleted`. |
| `src/activities/home/BookActions.cpp` | Stamps `statusModified` in `toggleBookCompleted`. |
| `test/bookorbit_*/` | One GoogleTest target per unit, registered in `test/CMakeLists.txt`. |

---

### Task 1: Date-only values and the conflict rule

**Files:**
- Create: `lib/BookOrbit/BookOrbitDate.h`, `lib/BookOrbit/BookOrbitDate.cpp`
- Create: `test/bookorbit_date/CMakeLists.txt`, `test/bookorbit_date/BookOrbitDateTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(bookorbit_date)` beside the existing entries)

**Interfaces:**
- Consumes: nothing.
- Produces:
  `struct bookorbit::DateOnly { uint16_t year = 0; uint8_t month = 0; uint8_t day = 0; bool valid() const; }`;
  `bool bookorbit::parseDateOnly(std::string_view text, DateOnly& out)`;
  `void bookorbit::formatDateOnly(const DateOnly& date, char* buf, size_t len)`;
  `int bookorbit::compareDateOnly(const DateOnly& lhs, const DateOnly& rhs)`;
  `enum class bookorbit::Winner { Local, Server }`;
  `Winner bookorbit::resolveByDate(const DateOnly& local, const DateOnly& server)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_date/BookOrbitDateTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitDate.h"

using bookorbit::compareDateOnly;
using bookorbit::DateOnly;
using bookorbit::formatDateOnly;
using bookorbit::parseDateOnly;
using bookorbit::resolveByDate;
using bookorbit::Winner;

namespace {

DateOnly date(const uint16_t year, const uint8_t month, const uint8_t day) {
  DateOnly out;
  out.year = year;
  out.month = month;
  out.day = day;
  return out;
}

}  // namespace

TEST(BookOrbitDate, ParsesIsoDate) {
  DateOnly out;
  ASSERT_TRUE(parseDateOnly("2026-08-21", out));
  EXPECT_EQ(out.year, 2026);
  EXPECT_EQ(out.month, 8);
  EXPECT_EQ(out.day, 21);
  EXPECT_TRUE(out.valid());
}

// The server also sends full timestamps in *UpdatedAt. The Lua plugin keys on
// the first ten characters; do the same rather than rejecting the value.
TEST(BookOrbitDate, ParsesDatePrefixOfATimestamp) {
  DateOnly out;
  ASSERT_TRUE(parseDateOnly("2026-08-21T14:05:09Z", out));
  EXPECT_EQ(out.day, 21);
}

TEST(BookOrbitDate, RejectsMalformedInput) {
  DateOnly out;
  EXPECT_FALSE(parseDateOnly("", out));
  EXPECT_FALSE(parseDateOnly("2026-8-21", out));
  EXPECT_FALSE(parseDateOnly("21-08-2026", out));
  EXPECT_FALSE(parseDateOnly("2026/08/21", out));
  EXPECT_FALSE(parseDateOnly("abcd-ef-gh", out));
}

TEST(BookOrbitDate, RejectsOutOfRangeComponents) {
  DateOnly out;
  EXPECT_FALSE(parseDateOnly("2026-13-01", out));
  EXPECT_FALSE(parseDateOnly("2026-00-01", out));
  EXPECT_FALSE(parseDateOnly("2026-02-30", out));
  EXPECT_FALSE(parseDateOnly("2026-01-00", out));
}

TEST(BookOrbitDate, AcceptsLeapDay) {
  DateOnly out;
  EXPECT_TRUE(parseDateOnly("2024-02-29", out));
  EXPECT_FALSE(parseDateOnly("2026-02-29", out));
}

TEST(BookOrbitDate, FormatsZeroPadded) {
  char buf[11] = {};
  formatDateOnly(date(2026, 8, 1), buf, sizeof(buf));
  EXPECT_STREQ(buf, "2026-08-01");
}

TEST(BookOrbitDate, FormatsInvalidDateAsEmpty) {
  char buf[11] = {'x', '\0'};
  formatDateOnly(DateOnly{}, buf, sizeof(buf));
  EXPECT_STREQ(buf, "");
}

TEST(BookOrbitDate, ComparesChronologically) {
  EXPECT_LT(compareDateOnly(date(2026, 8, 20), date(2026, 8, 21)), 0);
  EXPECT_GT(compareDateOnly(date(2026, 9, 1), date(2026, 8, 31)), 0);
  EXPECT_EQ(compareDateOnly(date(2026, 8, 21), date(2026, 8, 21)), 0);
  EXPECT_LT(compareDateOnly(date(2025, 12, 31), date(2026, 1, 1)), 0);
}

TEST(BookOrbitDate, NewerLocalDateWins) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 22), date(2026, 8, 21)), Winner::Local);
}

TEST(BookOrbitDate, NewerServerDateWins) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 20), date(2026, 8, 21)), Winner::Server);
}

// Date-only granularity means ties are common. The server is authoritative on
// a tie, so two devices editing on the same day converge instead of flapping.
TEST(BookOrbitDate, TiePrefersServer) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 21), date(2026, 8, 21)), Winner::Server);
}

TEST(BookOrbitDate, MissingLocalDateLosesToServer) {
  EXPECT_EQ(resolveByDate(DateOnly{}, date(2026, 8, 21)), Winner::Server);
}

TEST(BookOrbitDate, MissingServerDateLosesToLocal) {
  EXPECT_EQ(resolveByDate(date(2026, 8, 21), DateOnly{}), Winner::Local);
}

// Neither side is dated: the server value is the shared baseline, so take it.
TEST(BookOrbitDate, BothMissingPrefersServer) {
  EXPECT_EQ(resolveByDate(DateOnly{}, DateOnly{}), Winner::Server);
}
```

Create `test/bookorbit_date/CMakeLists.txt`:

```cmake
add_executable(BookOrbitDateTest
  BookOrbitDateTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
)

target_include_directories(BookOrbitDateTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitDateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitDateTest)
```

Add to `test/CMakeLists.txt`, after the last existing `add_subdirectory(...)` line:

```cmake
add_subdirectory(bookorbit_date)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/BookOrbitDate.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitDate.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// BookOrbit tracks per-field modification at date granularity only. Storing
// three integers rather than a string keeps the persisted record fixed-size.
struct DateOnly {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;

  bool valid() const { return year != 0 && month != 0 && day != 0; }
};

// Accepts "YYYY-MM-DD" and any longer string whose first ten characters are a
// valid date (the server sends full timestamps in ratingUpdatedAt).
bool parseDateOnly(std::string_view text, DateOnly& out);

// Writes "YYYY-MM-DD", or "" when the date is invalid. buf must hold 11 bytes.
void formatDateOnly(const DateOnly& date, char* buf, size_t len);

// < 0, 0, > 0 like strcmp.
int compareDateOnly(const DateOnly& lhs, const DateOnly& rhs);

enum class Winner : uint8_t { Local, Server };

// The side with the newer date wins. A tie prefers the server, and so does the
// case where neither side carries a usable date.
Winner resolveByDate(const DateOnly& local, const DateOnly& server);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitDate.cpp`:

```cpp
#include "BookOrbitDate.h"

#include <cstdio>

namespace bookorbit {
namespace {

bool isDigit(const char ch) { return ch >= '0' && ch <= '9'; }

bool isLeap(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeap(year)) return 29;
  return kDays[month - 1];
}

uint16_t digits(const std::string_view text, const size_t offset, const size_t count) {
  uint16_t value = 0;
  for (size_t i = 0; i < count; i++) {
    value = static_cast<uint16_t>(value * 10 + (text[offset + i] - '0'));
  }
  return value;
}

}  // namespace

bool parseDateOnly(const std::string_view text, DateOnly& out) {
  if (text.size() < 10) return false;
  for (size_t i = 0; i < 10; i++) {
    const bool wantDash = (i == 4 || i == 7);
    if (wantDash && text[i] != '-') return false;
    if (!wantDash && !isDigit(text[i])) return false;
  }

  const uint16_t year = digits(text, 0, 4);
  const uint16_t month = digits(text, 5, 2);
  const uint16_t day = digits(text, 8, 2);
  if (year == 0 || month < 1 || month > 12) return false;
  if (day < 1 || day > daysInMonth(year, static_cast<uint8_t>(month))) return false;

  out.year = year;
  out.month = static_cast<uint8_t>(month);
  out.day = static_cast<uint8_t>(day);
  return true;
}

void formatDateOnly(const DateOnly& date, char* buf, const size_t len) {
  if (buf == nullptr || len == 0) return;
  if (!date.valid()) {
    buf[0] = '\0';
    return;
  }
  snprintf(buf, len, "%04u-%02u-%02u", static_cast<unsigned>(date.year), static_cast<unsigned>(date.month),
           static_cast<unsigned>(date.day));
}

int compareDateOnly(const DateOnly& lhs, const DateOnly& rhs) {
  if (lhs.year != rhs.year) return lhs.year < rhs.year ? -1 : 1;
  if (lhs.month != rhs.month) return lhs.month < rhs.month ? -1 : 1;
  if (lhs.day != rhs.day) return lhs.day < rhs.day ? -1 : 1;
  return 0;
}

Winner resolveByDate(const DateOnly& local, const DateOnly& server) {
  if (!local.valid()) return Winner::Server;
  if (!server.valid()) return Winner::Local;
  return compareDateOnly(local, server) > 0 ? Winner::Local : Winner::Server;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitDate --output-on-failure
```

Expected: 13 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitDate.h lib/BookOrbit/BookOrbitDate.cpp test/bookorbit_date test/CMakeLists.txt
git commit -m "feat: add BookOrbit date-only values and conflict rule"
```

---

### Task 2: Status vocabulary and the local state record

**Files:**
- Create: `lib/BookOrbit/BookOrbitBookState.h`, `lib/BookOrbit/BookOrbitBookState.cpp`
- Create: `test/bookorbit_book_state/CMakeLists.txt`, `test/bookorbit_book_state/BookOrbitBookStateTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `src/activities/reader/BookReadingStats.h:13` (`isCompleted`), `src/activities/home/BookActions.cpp:149-186` (the toggle flow)

**Interfaces:**
- Consumes: `bookorbit::DateOnly` (Task 1).
- Produces:
  `enum class bookorbit::BookStatus : uint8_t { Reading, Complete, Abandoned }`;
  `const char* bookorbit::statusToString(BookStatus)`;
  `bool bookorbit::statusFromString(std::string_view, BookStatus&)`;
  `BookStatus bookorbit::statusFromCompletion(bool isCompleted, BookStatus previous)`;
  `bool bookorbit::normalizeRating(int raw, uint8_t& out)`;
  `std::string bookorbit::truncateReview(std::string_view note)`;
  `struct bookorbit::LocalBookState`.

`kReviewNoteMaxBytes = 10000`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_book_state/BookOrbitBookStateTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitBookState.h"

using bookorbit::BookStatus;
using bookorbit::kReviewNoteMaxBytes;
using bookorbit::LocalBookState;
using bookorbit::normalizeRating;
using bookorbit::statusFromCompletion;
using bookorbit::statusFromString;
using bookorbit::statusToString;
using bookorbit::truncateReview;

TEST(BookOrbitBookState, StatusRoundTripsThroughWireStrings) {
  EXPECT_STREQ(statusToString(BookStatus::Reading), "reading");
  EXPECT_STREQ(statusToString(BookStatus::Complete), "complete");
  EXPECT_STREQ(statusToString(BookStatus::Abandoned), "abandoned");

  BookStatus parsed = BookStatus::Reading;
  ASSERT_TRUE(statusFromString("complete", parsed));
  EXPECT_EQ(parsed, BookStatus::Complete);
  ASSERT_TRUE(statusFromString("abandoned", parsed));
  EXPECT_EQ(parsed, BookStatus::Abandoned);
  ASSERT_TRUE(statusFromString("reading", parsed));
  EXPECT_EQ(parsed, BookStatus::Reading);
}

// The vocabulary is closed. An unknown status is dropped, never guessed at.
TEST(BookOrbitBookState, UnknownStatusStringIsRejected) {
  BookStatus parsed = BookStatus::Complete;
  EXPECT_FALSE(statusFromString("finished", parsed));
  EXPECT_FALSE(statusFromString("", parsed));
  EXPECT_FALSE(statusFromString("Complete", parsed));
  EXPECT_EQ(parsed, BookStatus::Complete);  // untouched on failure
}

TEST(BookOrbitBookState, CompletedFlagMapsToComplete) {
  EXPECT_EQ(statusFromCompletion(true, BookStatus::Reading), BookStatus::Complete);
  EXPECT_EQ(statusFromCompletion(true, BookStatus::Abandoned), BookStatus::Complete);
}

TEST(BookOrbitBookState, ClearedFlagMapsToReading) {
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Complete), BookStatus::Reading);
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Reading), BookStatus::Reading);
}

// CrossInk has no "abandoned" gesture. A book the server calls abandoned keeps
// that status while isCompleted stays false, so a pull is never undone by the
// next push.
TEST(BookOrbitBookState, AbandonedSurvivesAnUncompletedFlag) {
  EXPECT_EQ(statusFromCompletion(false, BookStatus::Abandoned), BookStatus::Abandoned);
}

TEST(BookOrbitBookState, RatingAcceptsOneToFive) {
  uint8_t out = 0;
  for (int value = 1; value <= 5; value++) {
    ASSERT_TRUE(normalizeRating(value, out)) << value;
    EXPECT_EQ(out, static_cast<uint8_t>(value));
  }
}

TEST(BookOrbitBookState, RatingRejectsOutOfRange) {
  uint8_t out = 3;
  EXPECT_FALSE(normalizeRating(0, out));
  EXPECT_FALSE(normalizeRating(6, out));
  EXPECT_FALSE(normalizeRating(-1, out));
  EXPECT_EQ(out, 3);  // untouched on failure
}

TEST(BookOrbitBookState, ReviewIsTruncatedAtTheWireLimit) {
  const std::string long_note(kReviewNoteMaxBytes + 500, 'a');
  EXPECT_EQ(truncateReview(long_note).size(), kReviewNoteMaxBytes);
  EXPECT_EQ(truncateReview("short").size(), 5u);
}

TEST(BookOrbitBookState, LocalStateStartsEmpty) {
  LocalBookState state;
  EXPECT_FALSE(state.statusKnown);
  EXPECT_FALSE(state.ratingSet);
  EXPECT_FALSE(state.reviewSet);
  EXPECT_FALSE(state.statusModified.valid());
  EXPECT_FALSE(state.reviewModified.valid());
  EXPECT_TRUE(state.reviewNote.empty());
}

TEST(BookOrbitBookState, SetRatingStampsTheDate) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  ASSERT_TRUE(state.setRating(4, today));
  EXPECT_TRUE(state.ratingSet);
  EXPECT_EQ(state.rating, 4);
  EXPECT_EQ(state.statusModified.day, 21);
}

TEST(BookOrbitBookState, ClearRatingStampsTheDateAndUnsetsIt) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));
  ASSERT_TRUE(state.setRating(4, today));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-22", later));
  state.clearRating(later);
  EXPECT_FALSE(state.ratingSet);
  EXPECT_EQ(state.rating, 0);
  EXPECT_EQ(state.statusModified.day, 22);
}

TEST(BookOrbitBookState, SetReviewStampsTheReviewDateOnly) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  state.setReview("Great fun.", today);
  EXPECT_TRUE(state.reviewSet);
  EXPECT_EQ(state.reviewNote, "Great fun.");
  EXPECT_EQ(state.reviewModified.day, 21);
  EXPECT_FALSE(state.statusModified.valid());
}
```

Create `test/bookorbit_book_state/CMakeLists.txt`:

```cmake
add_executable(BookOrbitBookStateTest
  BookOrbitBookStateTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
)

target_include_directories(BookOrbitBookStateTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitBookStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBookStateTest)
```

Add `add_subdirectory(bookorbit_book_state)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitBookState.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitBookState.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "BookOrbitDate.h"

namespace bookorbit {

// Server-side truncation limit for a review note, matching bookorbit_sidecar.lua.
inline constexpr size_t kReviewNoteMaxBytes = 10000;

// The wire vocabulary is closed: "reading" | "complete" | "abandoned".
enum class BookStatus : uint8_t { Reading, Complete, Abandoned };

const char* statusToString(BookStatus status);

// Returns false and leaves `out` untouched for any string outside the
// vocabulary, including differently-cased spellings.
bool statusFromString(std::string_view text, BookStatus& out);

// Maps CrossInk's BookReadingStats::isCompleted onto the wire status.
// "abandoned" has no CrossInk gesture, so a previously pulled "abandoned"
// survives an un-completed flag instead of being overwritten with "reading".
BookStatus statusFromCompletion(bool isCompleted, BookStatus previous);

// Accepts 1..5 only. Out-of-range values are dropped, not clamped.
bool normalizeRating(int raw, uint8_t& out);

std::string truncateReview(std::string_view note);

// The device's own view of one book's state. `statusModified` covers both the
// status and the rating, exactly as buildStatePayload in the Lua plugin does;
// the review carries its own date.
struct LocalBookState {
  bool statusKnown = false;
  BookStatus status = BookStatus::Reading;
  DateOnly statusModified;
  bool ratingSet = false;
  uint8_t rating = 0;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly reviewModified;

  void setStatus(BookStatus value, const DateOnly& today);
  bool setRating(int value, const DateOnly& today);
  void clearRating(const DateOnly& today);
  void setReview(std::string_view note, const DateOnly& today);
  void clearReview(const DateOnly& today);
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitBookState.cpp`:

```cpp
#include "BookOrbitBookState.h"

namespace bookorbit {

const char* statusToString(const BookStatus status) {
  switch (status) {
    case BookStatus::Complete:
      return "complete";
    case BookStatus::Abandoned:
      return "abandoned";
    case BookStatus::Reading:
    default:
      return "reading";
  }
}

bool statusFromString(const std::string_view text, BookStatus& out) {
  if (text == "reading") {
    out = BookStatus::Reading;
    return true;
  }
  if (text == "complete") {
    out = BookStatus::Complete;
    return true;
  }
  if (text == "abandoned") {
    out = BookStatus::Abandoned;
    return true;
  }
  return false;
}

BookStatus statusFromCompletion(const bool isCompleted, const BookStatus previous) {
  if (isCompleted) return BookStatus::Complete;
  if (previous == BookStatus::Abandoned) return BookStatus::Abandoned;
  return BookStatus::Reading;
}

bool normalizeRating(const int raw, uint8_t& out) {
  if (raw < 1 || raw > 5) return false;
  out = static_cast<uint8_t>(raw);
  return true;
}

std::string truncateReview(const std::string_view note) {
  if (note.size() <= kReviewNoteMaxBytes) return std::string(note);
  return std::string(note.substr(0, kReviewNoteMaxBytes));
}

void LocalBookState::setStatus(const BookStatus value, const DateOnly& today) {
  statusKnown = true;
  status = value;
  statusModified = today;
}

bool LocalBookState::setRating(const int value, const DateOnly& today) {
  uint8_t normalized = 0;
  if (!normalizeRating(value, normalized)) return false;
  ratingSet = true;
  rating = normalized;
  statusModified = today;
  return true;
}

void LocalBookState::clearRating(const DateOnly& today) {
  ratingSet = false;
  rating = 0;
  statusModified = today;
}

void LocalBookState::setReview(const std::string_view note, const DateOnly& today) {
  reviewSet = true;
  reviewNote = truncateReview(note);
  reviewModified = today;
}

void LocalBookState::clearReview(const DateOnly& today) {
  reviewSet = false;
  reviewNote.clear();
  reviewModified = today;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitBookState --output-on-failure
```

Expected: 12 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitBookState.h lib/BookOrbit/BookOrbitBookState.cpp test/bookorbit_book_state test/CMakeLists.txt
git commit -m "feat: add BookOrbit book status vocabulary and local state record"
```

---

### Task 3: book-states request encoder and change detection

**Files:**
- Create: `lib/BookOrbit/BookStateCodec.h`, `lib/BookOrbit/BookStateCodec.cpp`
- Create: `test/bookorbit_state_encode/CMakeLists.txt`, `test/bookorbit_state_encode/BookStateEncodeTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `bookorbit_sidecar.lua:236-282` (`buildStatePayload` — the authoritative shape)

**Interfaces:**
- Consumes: `bookorbit::LocalBookState`, `BookStatus`, `statusToString` (Task 2); `DateOnly`, `formatDateOnly`, `compareDateOnly` (Task 1); `bookorbit::jsonEscape` (P0 Task 7).
- Produces:
  `inline constexpr size_t bookorbit::kBookStateBatchSize = 200`;
  `struct bookorbit::SyncedBookState { bool ratingKnown = false; bool ratingSet = false; uint8_t rating = 0; bool reviewKnown = false; bool reviewSet = false; std::string reviewNote; DateOnly statusSyncedModified; }`;
  `struct bookorbit::BookStatePayload { std::string hash; bool hasStatus = false; BookStatus status = BookStatus::Reading; DateOnly statusModified; bool hasRating = false; uint8_t rating = 0; bool ratingCleared = false; bool hasReview = false; std::string reviewNote; bool reviewCleared = false; DateOnly reviewModified; }`;
  `bool bookorbit::buildStatePayload(std::string_view hash, const LocalBookState&, const SyncedBookState&, bool forcePull, BookStatePayload& out)`;
  `std::string bookorbit::encodeBookStates(const std::vector<BookStatePayload>&)`.

The encoder emits `{"books":[…]}` only. The four device fields (`deviceId`, `deviceModel`, `pluginVersion`, `deviceTime`) are injected by `BookOrbitClient::withDevice`, never here.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_state_encode/BookStateEncodeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateCodec.h"

using bookorbit::BookStatePayload;
using bookorbit::BookStatus;
using bookorbit::buildStatePayload;
using bookorbit::encodeBookStates;
using bookorbit::LocalBookState;
using bookorbit::SyncedBookState;

namespace {

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kHash[] = "0f0a792b00a37cf80baa5e50c078b31f";

}  // namespace

TEST(BookStateEncode, BatchSizeIsTwoHundred) {
  EXPECT_EQ(bookorbit::kBookStateBatchSize, 200u);
}

TEST(BookStateEncode, EncodesStatusAndDate) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasStatus = true;
  payload.status = BookStatus::Complete;
  payload.statusModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("books":[)"), std::string::npos);
  EXPECT_NE(json.find(std::string(R"("hash":")") + kHash + '"'), std::string::npos);
  EXPECT_NE(json.find(R"("status":"complete")"), std::string::npos);
  EXPECT_NE(json.find(R"("statusModified":"2026-08-21")"), std::string::npos);
}

TEST(BookStateEncode, EncodesRatingAsAnInteger) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasRating = true;
  payload.rating = 4;
  payload.statusModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("rating":4)"), std::string::npos);
  EXPECT_EQ(json.find("ratingCleared"), std::string::npos);
}

// Clearing is explicit. An absent field means "unchanged", so a cleared rating
// must ride on ratingCleared:true or the server keeps the old value forever.
TEST(BookStateEncode, ClearingRatingIsExplicit) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.ratingCleared = true;
  payload.statusModified = day("2026-08-22");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("ratingCleared":true)"), std::string::npos);
  EXPECT_EQ(json.find(R"("rating":)"), std::string::npos);
}

TEST(BookStateEncode, ClearingReviewIsExplicit) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.reviewCleared = true;
  payload.reviewModified = day("2026-08-22");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"("reviewCleared":true)"), std::string::npos);
  EXPECT_NE(json.find(R"("reviewModified":"2026-08-22")"), std::string::npos);
  EXPECT_EQ(json.find(R"("reviewNote":)"), std::string::npos);
}

TEST(BookStateEncode, EscapesReviewText) {
  BookStatePayload payload;
  payload.hash = kHash;
  payload.hasReview = true;
  payload.reviewNote = "Line \"one\"\nLine two";
  payload.reviewModified = day("2026-08-21");

  const std::string json = encodeBookStates({payload});
  EXPECT_NE(json.find(R"(Line \"one\"\nLine two)"), std::string::npos);
}

TEST(BookStateEncode, EncodesSeveralBooksInOneArray) {
  BookStatePayload first;
  first.hash = "aaa";
  first.hasStatus = true;
  first.status = BookStatus::Reading;
  first.statusModified = day("2026-08-21");
  BookStatePayload second;
  second.hash = "bbb";
  second.hasStatus = true;
  second.status = BookStatus::Abandoned;
  second.statusModified = day("2026-08-20");

  const std::string json = encodeBookStates({first, second});
  EXPECT_NE(json.find(R"({"hash":"aaa")"), std::string::npos);
  EXPECT_NE(json.find(R"({"hash":"bbb")"), std::string::npos);
  EXPECT_NE(json.find(R"("status":"abandoned")"), std::string::npos);
}

TEST(BookStateEncode, EmptyBatchStillEncodesAnArray) {
  EXPECT_EQ(encodeBookStates({}), R"({"books":[]})");
}

TEST(BookStateEncode, NothingChangedProducesNoPayload) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  EXPECT_FALSE(buildStatePayload(kHash, local, synced, false, payload));
}

// A forced pull is the only way a rating written on the web reaches the device.
TEST(BookStateEncode, ForcedPullSendsHashOnly) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, true, payload));
  EXPECT_EQ(payload.hash, kHash);
  EXPECT_FALSE(payload.hasStatus);
  EXPECT_FALSE(payload.hasRating);
  EXPECT_FALSE(payload.ratingCleared);
  EXPECT_FALSE(payload.hasReview);
}

TEST(BookStateEncode, ChangedStatusDateProducesAStatusPayload) {
  LocalBookState local;
  local.setStatus(BookStatus::Complete, day("2026-08-22"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasStatus);
  EXPECT_EQ(payload.status, BookStatus::Complete);
  EXPECT_EQ(payload.statusModified.day, 22);
}

TEST(BookStateEncode, FirstRatingIsAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-21"));
  SyncedBookState synced;  // nothing known server-side yet

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasRating);
  EXPECT_EQ(payload.rating, 5);
  EXPECT_EQ(payload.statusModified.day, 21);
}

TEST(BookStateEncode, SameRatingIsNotAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-21"));
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-21");
  synced.ratingKnown = true;
  synced.ratingSet = true;
  synced.rating = 5;

  BookStatePayload payload;
  EXPECT_FALSE(buildStatePayload(kHash, local, synced, false, payload));
}

TEST(BookStateEncode, RemovingAKnownRatingProducesRatingCleared) {
  LocalBookState local;  // no rating locally
  local.statusModified = day("2026-08-22");
  SyncedBookState synced;
  synced.statusSyncedModified = day("2026-08-22");
  synced.ratingKnown = true;
  synced.ratingSet = true;
  synced.rating = 5;

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.ratingCleared);
  EXPECT_FALSE(payload.hasRating);
}

TEST(BookStateEncode, ChangedReviewProducesAReviewPayload) {
  LocalBookState local;
  local.setReview("Better than expected.", day("2026-08-22"));
  SyncedBookState synced;
  synced.reviewKnown = true;
  synced.reviewSet = true;
  synced.reviewNote = "Old note.";

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.hasReview);
  EXPECT_EQ(payload.reviewNote, "Better than expected.");
  EXPECT_EQ(payload.reviewModified.day, 22);
}

TEST(BookStateEncode, RemovingAKnownReviewProducesReviewCleared) {
  LocalBookState local;
  local.reviewModified = day("2026-08-22");
  SyncedBookState synced;
  synced.reviewKnown = true;
  synced.reviewSet = true;
  synced.reviewNote = "Old note.";

  BookStatePayload payload;
  ASSERT_TRUE(buildStatePayload(kHash, local, synced, false, payload));
  EXPECT_TRUE(payload.reviewCleared);
  EXPECT_FALSE(payload.hasReview);
}
```

Create `test/bookorbit_state_encode/CMakeLists.txt`:

```cmake
add_executable(BookStateEncodeTest
  BookStateEncodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookStateEncodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateEncodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateEncodeTest)
```

`BookOrbitMatch.cpp` is linked only for `bookorbit::jsonEscape`, which P0 Task 7 declares in `BookOrbitMatch.h`. Do not re-implement escaping here.

Add `add_subdirectory(bookorbit_state_encode)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookStateCodec.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookStateCodec.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitBookState.h"
#include "BookOrbitDate.h"

namespace bookorbit {

// POST /koreader/plugin/book-states accepts at most this many books.
inline constexpr size_t kBookStateBatchSize = 200;

// What the server was last known to hold for one book. Persisted alongside the
// local state so an unchanged book costs no request at all.
struct SyncedBookState {
  bool ratingKnown = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  bool reviewKnown = false;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly statusSyncedModified;  // mirrors BookSyncState::statusSyncedModified
};

// One entry of the request's "books" array. Every optional field is explicit:
// hasX means "send X", xCleared means "unset X server-side", and neither means
// "leave it alone".
struct BookStatePayload {
  std::string hash;
  bool hasStatus = false;
  BookStatus status = BookStatus::Reading;
  DateOnly statusModified;
  bool hasRating = false;
  uint8_t rating = 0;
  bool ratingCleared = false;
  bool hasReview = false;
  std::string reviewNote;
  bool reviewCleared = false;
  DateOnly reviewModified;
};

// Returns false when nothing changed and no pull was forced — the caller then
// skips the request entirely. Mirrors buildStatePayload in bookorbit_sidecar.lua.
bool buildStatePayload(std::string_view hash, const LocalBookState& local, const SyncedBookState& synced,
                       bool forcePull, BookStatePayload& out);

// Emits {"books":[...]} only. Device fields are added by BookOrbitClient::withDevice.
std::string encodeBookStates(const std::vector<BookStatePayload>& books);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookStateCodec.cpp`:

```cpp
#include "BookStateCodec.h"

#include "BookOrbitMatch.h"  // jsonEscape

namespace bookorbit {
namespace {

void appendDate(std::string& json, const char* key, const DateOnly& date) {
  if (!date.valid()) return;
  char buf[11] = {};
  formatDateOnly(date, buf, sizeof(buf));
  json += ",\"";
  json += key;
  json += "\":\"";
  json += buf;
  json += '"';
}

}  // namespace

bool buildStatePayload(const std::string_view hash, const LocalBookState& local, const SyncedBookState& synced,
                       const bool forcePull, BookStatePayload& out) {
  out = BookStatePayload{};
  out.hash = std::string(hash);

  const bool statusChanged =
      local.statusKnown && compareDateOnly(local.statusModified, synced.statusSyncedModified) != 0;

  const bool ratingKnown = synced.ratingKnown;
  bool ratingChanged = false;
  if (local.ratingSet) {
    ratingChanged = !ratingKnown || !synced.ratingSet || local.rating != synced.rating;
  } else if (ratingKnown && synced.ratingSet) {
    ratingChanged = true;  // the user removed a rating the server still holds
  }

  const bool reviewKnown = synced.reviewKnown;
  bool reviewChanged = false;
  if (local.reviewSet) {
    reviewChanged = !reviewKnown || !synced.reviewSet || local.reviewNote != synced.reviewNote;
  } else if (reviewKnown && synced.reviewSet) {
    reviewChanged = true;
  }

  if (!statusChanged && !ratingChanged && !reviewChanged) {
    // A forced pull is a bare hash: no local field is asserted, but the server
    // still answers with its rating and review.
    return forcePull;
  }

  if (statusChanged) {
    out.hasStatus = true;
    out.status = local.status;
    out.statusModified = local.statusModified;
  }
  if (ratingChanged) {
    if (local.ratingSet) {
      out.hasRating = true;
      out.rating = local.rating;
    } else {
      out.ratingCleared = true;
    }
    if (!out.statusModified.valid()) {
      out.statusModified = local.statusModified;
    }
  }
  if (reviewChanged) {
    if (local.reviewSet) {
      out.hasReview = true;
      out.reviewNote = local.reviewNote;
    } else {
      out.reviewCleared = true;
    }
    out.reviewModified = local.reviewModified;
  }
  return true;
}

std::string encodeBookStates(const std::vector<BookStatePayload>& books) {
  std::string json = R"({"books":[)";
  for (size_t i = 0; i < books.size(); i++) {
    const auto& book = books[i];
    if (i > 0) json += ',';
    json += R"({"hash":")";
    json += jsonEscape(book.hash);
    json += '"';
    if (book.hasStatus) {
      json += R"(,"status":")";
      json += statusToString(book.status);
      json += '"';
    }
    appendDate(json, "statusModified", book.statusModified);
    if (book.hasRating) {
      json += R"(,"rating":)";
      json += std::to_string(static_cast<unsigned>(book.rating));
    }
    if (book.ratingCleared) {
      json += R"(,"ratingCleared":true)";
    }
    if (book.hasReview) {
      json += R"(,"reviewNote":")";
      json += jsonEscape(book.reviewNote);
      json += '"';
    }
    if (book.reviewCleared) {
      json += R"(,"reviewCleared":true)";
    }
    appendDate(json, "reviewModified", book.reviewModified);
    json += '}';
  }
  json += "]}";
  return json;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookStateEncode --output-on-failure
```

Expected: 16 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookStateCodec.h lib/BookOrbit/BookStateCodec.cpp test/bookorbit_state_encode test/CMakeLists.txt
git commit -m "feat: add BookOrbit book-states request encoder"
```

---

### Task 4: book-states response decoder

**Files:**
- Modify: `lib/BookOrbit/BookStateCodec.h`, `lib/BookOrbit/BookStateCodec.cpp`
- Create: `test/bookorbit_state_decode/CMakeLists.txt`, `test/bookorbit_state_decode/BookStateDecodeTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `lib/JsonParser/StreamingJsonParser.h` (C-callback API), `bookorbit_sidecar.lua:196-210` (`stateFromServerResult`)

**Interfaces:**
- Consumes: `StreamingJsonParser`, `JsonCallbacks` (`lib/JsonParser/StreamingJsonParser.h`); `parseDateOnly` (Task 1).
- Produces:
  `struct bookorbit::ServerBookState { std::string hash; bool ratingKnown = false; bool ratingSet = false; uint8_t rating = 0; DateOnly ratingUpdatedAt; bool reviewKnown = false; bool reviewNoteSet = false; std::string reviewNote; DateOnly reviewUpdatedAt; }`;
  `bool bookorbit::decodeBookStates(std::string_view json, std::vector<std::string>& unmatched, std::vector<ServerBookState>& results)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_state_decode/BookStateDecodeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateCodec.h"

using bookorbit::decodeBookStates;
using bookorbit::ServerBookState;

TEST(BookStateDecode, DecodesRatingAndReview) {
  const std::string body = R"({
    "unmatched": [],
    "results": [
      {"hash": "abc", "ratingSet": true, "rating": 4, "ratingUpdatedAt": "2026-08-21",
       "reviewNoteSet": true, "reviewNote": "Great fun.", "reviewUpdatedAt": "2026-08-22"}
    ]
  })";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].hash, "abc");
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_TRUE(results[0].ratingSet);
  EXPECT_EQ(results[0].rating, 4);
  EXPECT_EQ(results[0].ratingUpdatedAt.day, 21);
  EXPECT_TRUE(results[0].reviewKnown);
  EXPECT_TRUE(results[0].reviewNoteSet);
  EXPECT_EQ(results[0].reviewNote, "Great fun.");
  EXPECT_EQ(results[0].reviewUpdatedAt.day, 22);
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookStateDecode, DecodesUnmatchedHashes) {
  const std::string body = R"({"unmatched":["abc","def"],"results":[]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(unmatched.size(), 2u);
  EXPECT_EQ(unmatched[0], "abc");
  EXPECT_EQ(unmatched[1], "def");
  EXPECT_TRUE(results.empty());
}

// ratingSet:false means "the server holds no rating" — a definitive answer,
// distinct from the field being absent, which means "no opinion".
TEST(BookStateDecode, RatingSetFalseIsKnownAndUnset) {
  const std::string body = R"({"results":[{"hash":"abc","ratingSet":false}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].ratingSet);
  EXPECT_EQ(results[0].rating, 0);
}

TEST(BookStateDecode, AbsentFlagsLeaveTheFieldUnknown) {
  const std::string body = R"({"results":[{"hash":"abc"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].reviewKnown);
}

TEST(BookStateDecode, ReviewNoteSetFalseIsKnownAndEmpty) {
  const std::string body = R"({"results":[{"hash":"abc","reviewNoteSet":false,"reviewUpdatedAt":"2026-08-22"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].reviewKnown);
  EXPECT_FALSE(results[0].reviewNoteSet);
  EXPECT_TRUE(results[0].reviewNote.empty());
  EXPECT_EQ(results[0].reviewUpdatedAt.day, 22);
}

TEST(BookStateDecode, AcceptsTimestampsInUpdatedAtFields) {
  const std::string body =
      R"({"results":[{"hash":"abc","ratingSet":true,"rating":3,"ratingUpdatedAt":"2026-08-21T09:14:00Z"}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].ratingUpdatedAt.year, 2026);
  EXPECT_EQ(results[0].ratingUpdatedAt.month, 8);
  EXPECT_EQ(results[0].ratingUpdatedAt.day, 21);
}

TEST(BookStateDecode, DecodesSeveralResults) {
  const std::string body = R"({"results":[
    {"hash":"abc","ratingSet":true,"rating":1},
    {"hash":"def","ratingSet":true,"rating":5}
  ]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].rating, 1);
  EXPECT_EQ(results[1].hash, "def");
  EXPECT_EQ(results[1].rating, 5);
}

TEST(BookStateDecode, RejectsMalformedJson) {
  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  EXPECT_FALSE(decodeBookStates("{not json", unmatched, results));
  EXPECT_TRUE(results.empty());
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookStateDecode, EmptyBodyObjectIsSuccessWithNothing) {
  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates("{}", unmatched, results));
  EXPECT_TRUE(results.empty());
  EXPECT_TRUE(unmatched.empty());
}

// Out-of-range ratings are dropped rather than stored, matching the encoder.
TEST(BookStateDecode, DropsOutOfRangeRating) {
  const std::string body = R"({"results":[{"hash":"abc","ratingSet":true,"rating":9}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(results[0].ratingKnown);
  EXPECT_FALSE(results[0].ratingSet);
}

TEST(BookStateDecode, ResultsWithoutHashAreDropped) {
  const std::string body = R"({"results":[{"ratingSet":true,"rating":3}]})";

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  ASSERT_TRUE(decodeBookStates(body, unmatched, results));
  EXPECT_TRUE(results.empty());
}
```

Create `test/bookorbit_state_decode/CMakeLists.txt`:

```cmake
add_executable(BookStateDecodeTest
  BookStateDecodeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookStateDecodeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateDecodeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateDecodeTest)
```

Add `add_subdirectory(bookorbit_state_decode)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'decodeBookStates' is not a member of 'bookorbit'`.

- [ ] **Step 3: Write minimal implementation**

Append to `lib/BookOrbit/BookStateCodec.h`, inside `namespace bookorbit`, before the closing brace:

```cpp
// One entry of the response's "results" array. The *Known flags distinguish
// "the server answered for this field" from "the field was absent".
struct ServerBookState {
  std::string hash;
  bool ratingKnown = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  DateOnly ratingUpdatedAt;
  bool reviewKnown = false;
  bool reviewNoteSet = false;
  std::string reviewNote;
  DateOnly reviewUpdatedAt;
};

// Returns false only on malformed JSON. A well-formed body with neither array
// is a success with both outputs empty.
bool decodeBookStates(std::string_view json, std::vector<std::string>& unmatched,
                      std::vector<ServerBookState>& results);
```

Append to `lib/BookOrbit/BookStateCodec.cpp`, inside `namespace bookorbit`:

```cpp
namespace {

// StreamingJsonParser is a C-callback tokenizer with a void* ctx, a 512-byte
// token buffer and 32 nesting levels, so decoding is a small state machine
// rather than a DOM walk. Two sibling arrays have to be told apart, which is
// what `section` tracks.
enum class StateSection : uint8_t { None, Unmatched, Results };

struct StateDecodeCtx {
  std::vector<std::string>* unmatched = nullptr;
  std::vector<ServerBookState>* results = nullptr;
  std::string key;
  StateSection section = StateSection::None;
  int objectDepth = 0;
  ServerBookState current;
};

void stateOnKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void stateOnString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  const std::string_view text(value, len);
  if (ctx->section == StateSection::Unmatched) {
    ctx->unmatched->emplace_back(text);
    return;
  }
  if (ctx->section != StateSection::Results) return;
  if (ctx->key == "hash") {
    ctx->current.hash.assign(text);
  } else if (ctx->key == "reviewNote") {
    ctx->current.reviewNote = truncateReview(text);
  } else if (ctx->key == "ratingUpdatedAt") {
    parseDateOnly(text, ctx->current.ratingUpdatedAt);
  } else if (ctx->key == "reviewUpdatedAt") {
    parseDateOnly(text, ctx->current.reviewUpdatedAt);
  }
}

void stateOnNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section != StateSection::Results || ctx->key != "rating") return;
  const std::string digits(value, len);
  uint8_t normalized = 0;
  if (normalizeRating(atoi(digits.c_str()), normalized)) {
    ctx->current.rating = normalized;
  } else {
    // Out of range: the server says it holds a rating we cannot represent.
    // Treat it as "known, unset" rather than inventing a value.
    ctx->current.rating = 0;
    ctx->current.ratingSet = false;
  }
}

void stateOnBool(void* raw, const bool value) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section != StateSection::Results) return;
  if (ctx->key == "ratingSet") {
    ctx->current.ratingKnown = true;
    ctx->current.ratingSet = value;
  } else if (ctx->key == "reviewNoteSet") {
    ctx->current.reviewKnown = true;
    ctx->current.reviewNoteSet = value;
  }
}

void stateOnNull(void*) {}

void stateOnArrayStart(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->key == "unmatched") {
    ctx->section = StateSection::Unmatched;
  } else if (ctx->key == "results") {
    ctx->section = StateSection::Results;
  }
}

void stateOnArrayEnd(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->section = StateSection::None;
  ctx->key.clear();
}

void stateOnObjectStart(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->objectDepth++;
  if (ctx->section == StateSection::Results) {
    ctx->current = ServerBookState{};
  }
}

void stateOnObjectEnd(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section == StateSection::Results && !ctx->current.hash.empty()) {
    // A rating that never passed normalizeRating leaves ratingSet false, so a
    // bad value degrades to "server holds nothing" instead of a wrong star count.
    if (ctx->current.ratingSet && ctx->current.rating == 0) {
      ctx->current.ratingSet = false;
    }
    if (!ctx->current.reviewNoteSet) {
      ctx->current.reviewNote.clear();
    }
    ctx->results->push_back(ctx->current);
  }
  ctx->objectDepth--;
  ctx->key.clear();
}

}  // namespace

bool decodeBookStates(const std::string_view json, std::vector<std::string>& unmatched,
                      std::vector<ServerBookState>& results) {
  unmatched.clear();
  results.clear();

  StateDecodeCtx ctx;
  ctx.unmatched = &unmatched;
  ctx.results = &results;

  const JsonCallbacks callbacks{
      &ctx,
      stateOnKey,
      stateOnString,
      stateOnNumber,
      stateOnBool,
      stateOnNull,
      stateOnObjectStart,
      stateOnObjectEnd,
      stateOnArrayStart,
      stateOnArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  // json.data() is not null-terminated, but feed() takes an explicit length, so
  // passing it here is safe. Never hand it to a C string API.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    unmatched.clear();
    results.clear();
    return false;
  }
  return true;
}
```

Add `#include <cstdlib>` (for `atoi`) and `#include "StreamingJsonParser.h"` to the top of `BookStateCodec.cpp`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookStateDecode --output-on-failure
```

Expected: 11 tests PASS. `ctest -R BookStateEncode` must still pass — the encoder and decoder share one translation unit.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookStateCodec.h lib/BookOrbit/BookStateCodec.cpp test/bookorbit_state_decode test/CMakeLists.txt
git commit -m "feat: add BookOrbit book-states response decoder"
```

---

### Task 5: Per-field conflict resolution

**Files:**
- Create: `lib/BookOrbit/BookStateMerge.h`, `lib/BookOrbit/BookStateMerge.cpp`
- Create: `test/bookorbit_state_merge/CMakeLists.txt`, `test/bookorbit_state_merge/BookStateMergeTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `LocalBookState` (Task 2); `ServerBookState` (Task 4); `resolveByDate`, `Winner` (Task 1).
- Produces:
  `struct bookorbit::MergeDecision { bool applyRating = false; bool ratingSet = false; uint8_t rating = 0; bool applyReview = false; bool reviewSet = false; std::string reviewNote; DateOnly ratingModified; DateOnly reviewModified; bool changedAnything() const; }`;
  `MergeDecision bookorbit::resolveBookState(const LocalBookState& local, const ServerBookState& server)`;
  `void bookorbit::applyMerge(const MergeDecision& decision, LocalBookState& local)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_state_merge/BookStateMergeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookStateMerge.h"

using bookorbit::applyMerge;
using bookorbit::LocalBookState;
using bookorbit::MergeDecision;
using bookorbit::resolveBookState;
using bookorbit::ServerBookState;

namespace {

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

}  // namespace

TEST(BookStateMerge, UnknownServerFieldsChangeNothing) {
  LocalBookState local;
  local.setRating(3, day("2026-08-21"));
  ServerBookState server;
  server.hash = "abc";

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
  EXPECT_FALSE(decision.applyReview);
  EXPECT_FALSE(decision.changedAnything());
}

TEST(BookStateMerge, NewerServerRatingWins) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_TRUE(decision.ratingSet);
  EXPECT_EQ(decision.rating, 5);
  EXPECT_EQ(decision.ratingModified.day, 21);
}

TEST(BookStateMerge, NewerLocalRatingIsKept) {
  LocalBookState local;
  local.setRating(3, day("2026-08-22"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
}

// Date-only granularity makes same-day edits a tie, and the server wins those.
TEST(BookStateMerge, SameDayTiePrefersServer) {
  LocalBookState local;
  local.setRating(3, day("2026-08-21"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_EQ(decision.rating, 5);
}

TEST(BookStateMerge, ServerClearedRatingIsApplied) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = false;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_FALSE(decision.ratingSet);
  EXPECT_EQ(decision.rating, 0);
}

TEST(BookStateMerge, ServerRatingArrivesWhenDeviceHasNone) {
  LocalBookState local;
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 2;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  ASSERT_TRUE(decision.applyRating);
  EXPECT_EQ(decision.rating, 2);
}

TEST(BookStateMerge, ReviewUsesItsOwnDate) {
  LocalBookState local;
  local.setRating(3, day("2026-08-25"));       // newer status date
  local.setReview("Old note.", day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "Server note.";
  server.reviewUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);  // server said nothing about the rating
  ASSERT_TRUE(decision.applyReview);
  EXPECT_EQ(decision.reviewNote, "Server note.");
  EXPECT_EQ(decision.reviewModified.day, 21);
}

TEST(BookStateMerge, NewerLocalReviewIsKept) {
  LocalBookState local;
  local.setReview("Local note.", day("2026-08-22"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "Server note.";
  server.reviewUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyReview);
}

TEST(BookStateMerge, IdenticalValuesAreNotAChange) {
  LocalBookState local;
  local.setRating(5, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  const MergeDecision decision = resolveBookState(local, server);
  EXPECT_FALSE(decision.applyRating);
  EXPECT_FALSE(decision.changedAnything());
}

TEST(BookStateMerge, ApplyMergeWritesRatingAndItsDate) {
  LocalBookState local;
  local.setRating(3, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.ratingSet);
  EXPECT_EQ(local.rating, 5);
  EXPECT_EQ(local.statusModified.day, 21);
}

TEST(BookStateMerge, ApplyMergeWritesReviewAndItsDate) {
  LocalBookState local;
  local.setReview("Old.", day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.reviewKnown = true;
  server.reviewNoteSet = true;
  server.reviewNote = "New.";
  server.reviewUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.reviewSet);
  EXPECT_EQ(local.reviewNote, "New.");
  EXPECT_EQ(local.reviewModified.day, 21);
}

TEST(BookStateMerge, ApplyMergeLeavesTheStatusFlagAlone) {
  LocalBookState local;
  local.setStatus(bookorbit::BookStatus::Complete, day("2026-08-20"));
  ServerBookState server;
  server.hash = "abc";
  server.ratingKnown = true;
  server.ratingSet = true;
  server.rating = 5;
  server.ratingUpdatedAt = day("2026-08-21");

  applyMerge(resolveBookState(local, server), local);
  EXPECT_TRUE(local.statusKnown);
  EXPECT_EQ(local.status, bookorbit::BookStatus::Complete);
}
```

Create `test/bookorbit_state_merge/CMakeLists.txt`:

```cmake
add_executable(BookStateMergeTest
  BookStateMergeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateMerge.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookStateMergeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStateMergeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateMergeTest)
```

Add `add_subdirectory(bookorbit_state_merge)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookStateMerge.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookStateMerge.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "BookOrbitBookState.h"
#include "BookOrbitDate.h"
#include "BookStateCodec.h"

namespace bookorbit {

// What the device should adopt from one server result. Each field is decided
// independently, because the server dates them independently.
struct MergeDecision {
  bool applyRating = false;
  bool ratingSet = false;
  uint8_t rating = 0;
  DateOnly ratingModified;
  bool applyReview = false;
  bool reviewSet = false;
  std::string reviewNote;
  DateOnly reviewModified;

  bool changedAnything() const { return applyRating || applyReview; }
};

// Newer date wins; a tie prefers the server. A field the server did not answer
// for is never touched, and a server value identical to the local one is not
// reported as a change so callers can skip a pointless write.
MergeDecision resolveBookState(const LocalBookState& local, const ServerBookState& server);

// Writes an accepted decision into the local record, stamping the server's date
// so the next buildStatePayload does not treat the pulled value as a local edit.
void applyMerge(const MergeDecision& decision, LocalBookState& local);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookStateMerge.cpp`:

```cpp
#include "BookStateMerge.h"

namespace bookorbit {

MergeDecision resolveBookState(const LocalBookState& local, const ServerBookState& server) {
  MergeDecision decision;

  if (server.ratingKnown) {
    const bool sameValue = (local.ratingSet == server.ratingSet) && (!server.ratingSet || local.rating == server.rating);
    if (!sameValue && resolveByDate(local.statusModified, server.ratingUpdatedAt) == Winner::Server) {
      decision.applyRating = true;
      decision.ratingSet = server.ratingSet;
      decision.rating = server.ratingSet ? server.rating : 0;
      decision.ratingModified = server.ratingUpdatedAt;
    }
  }

  if (server.reviewKnown) {
    const bool sameValue =
        (local.reviewSet == server.reviewNoteSet) && (!server.reviewNoteSet || local.reviewNote == server.reviewNote);
    if (!sameValue && resolveByDate(local.reviewModified, server.reviewUpdatedAt) == Winner::Server) {
      decision.applyReview = true;
      decision.reviewSet = server.reviewNoteSet;
      decision.reviewNote = server.reviewNoteSet ? truncateReview(server.reviewNote) : std::string();
      decision.reviewModified = server.reviewUpdatedAt;
    }
  }

  return decision;
}

void applyMerge(const MergeDecision& decision, LocalBookState& local) {
  if (decision.applyRating) {
    local.ratingSet = decision.ratingSet;
    local.rating = decision.ratingSet ? decision.rating : 0;
    if (decision.ratingModified.valid()) {
      local.statusModified = decision.ratingModified;
    }
  }
  if (decision.applyReview) {
    local.reviewSet = decision.reviewSet;
    local.reviewNote = decision.reviewSet ? decision.reviewNote : std::string();
    if (decision.reviewModified.valid()) {
      local.reviewModified = decision.reviewModified;
    }
  }
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookStateMerge --output-on-failure
```

Expected: 12 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookStateMerge.h lib/BookOrbit/BookStateMerge.cpp test/bookorbit_state_merge test/CMakeLists.txt
git commit -m "feat: add BookOrbit book-state conflict resolution"
```

---

### Task 6: Durable book-state store

**Files:**
- Create: `lib/BookOrbit/BookStateStore.h`, `lib/BookOrbit/BookStateStore.cpp`
- Create: `test/bookorbit_state_store/CMakeLists.txt`, `test/bookorbit_state_store/BookStateStoreTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `lib/BookOrbit/BookOrbitSyncState.cpp` (P0 Task 5) — same serialization idiom

**Interfaces:**
- Consumes: `bookorbit::IBlobStore`, `atomicWriteBlob`, `readBlobWithBackup` (P0 Task 4); `LocalBookState` (Task 2); `SyncedBookState` (Task 3).
- Produces:
  `struct bookorbit::BookStateRecord { char md5[33] = {}; LocalBookState local; SyncedBookState synced; uint32_t statePulledAt = 0; }`;
  `class bookorbit::BookStateStore` with
  `BookStateStore(IBlobStore&, std::string path)`,
  `bool load()`, `bool flush()`,
  `BookStateRecord* find(std::string_view md5)`,
  `BookStateRecord& findOrCreate(std::string_view md5)`,
  `bool needsStatePull(const BookStateRecord&, uint32_t nowUnix) const`,
  `static void markStatePulled(BookStateRecord&, uint32_t nowUnix)`.

`kStatePullMaxAgeSeconds = 86400`; `kFormatVersion = 1`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_state_store/BookStateStoreTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookStateStore.h"
#include "lib/BookOrbit/IBlobStore.h"

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

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr char kPath[] = "/bookorbit_states.bin";

}  // namespace

using bookorbit::BookStateStore;
using bookorbit::BookStatus;

TEST(BookStateStore, FindReturnsNullForUnknownBook) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, FindOrCreateStartsEmpty) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  const auto& record = store.findOrCreate(kMd5);
  EXPECT_FALSE(record.local.statusKnown);
  EXPECT_FALSE(record.local.ratingSet);
  EXPECT_FALSE(record.synced.ratingKnown);
  EXPECT_EQ(record.statePulledAt, 0u);
}

TEST(BookStateStore, RoundTripsThroughFlushAndLoad) {
  FakeBlobStore blobs;
  {
    BookStateStore store(blobs, kPath);
    auto& record = store.findOrCreate(kMd5);
    record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
    record.local.setRating(4, day("2026-08-21"));
    record.local.setReview("Great fun.", day("2026-08-22"));
    record.synced.ratingKnown = true;
    record.synced.ratingSet = true;
    record.synced.rating = 4;
    record.synced.reviewKnown = true;
    record.synced.reviewSet = true;
    record.synced.reviewNote = "Great fun.";
    record.synced.statusSyncedModified = day("2026-08-21");
    record.statePulledAt = 1787561449u;
    ASSERT_TRUE(store.flush());
  }

  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  const auto* record = reloaded.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->local.status, BookStatus::Complete);
  EXPECT_EQ(record->local.rating, 4);
  EXPECT_EQ(record->local.reviewNote, "Great fun.");
  EXPECT_EQ(record->local.reviewModified.day, 22);
  EXPECT_TRUE(record->synced.reviewSet);
  EXPECT_EQ(record->synced.statusSyncedModified.day, 21);
  EXPECT_EQ(record->statePulledAt, 1787561449u);
}

TEST(BookStateStore, LoadOnMissingFileSucceedsEmpty) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.load());
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, CorruptBlobLoadsEmptyRatherThanCrashing) {
  FakeBlobStore blobs;
  blobs.files[kPath] = {0xFF, 0xFF, 0x01};
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.load());
  EXPECT_EQ(store.find(kMd5), nullptr);
}

TEST(BookStateStore, MultipleBooksPersistIndependently) {
  FakeBlobStore blobs;
  constexpr char kOther[] = "6fba8d1c39745a4fe79813f76dbb314a";
  {
    BookStateStore store(blobs, kPath);
    store.findOrCreate(kMd5).local.setRating(1, day("2026-08-21"));
    store.findOrCreate(kOther).local.setRating(5, day("2026-08-22"));
    ASSERT_TRUE(store.flush());
  }
  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->local.rating, 1);
  EXPECT_EQ(reloaded.find(kOther)->local.rating, 5);
}

TEST(BookStateStore, LongReviewIsTruncatedOnPersist) {
  FakeBlobStore blobs;
  const std::string huge(bookorbit::kReviewNoteMaxBytes + 100, 'x');
  {
    BookStateStore store(blobs, kPath);
    store.findOrCreate(kMd5).local.setReview(huge, day("2026-08-21"));
    ASSERT_TRUE(store.flush());
  }
  BookStateStore reloaded(blobs, kPath);
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->local.reviewNote.size(), bookorbit::kReviewNoteMaxBytes);
}

TEST(BookStateStore, NeverPulledNeedsAPull) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  EXPECT_TRUE(store.needsStatePull(store.findOrCreate(kMd5), 1000u));
}

TEST(BookStateStore, RecentPullDoesNotNeedAnother) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 1000u);
  EXPECT_FALSE(store.needsStatePull(record, 1000u + 86400u));
}

TEST(BookStateStore, PullExpiresAfterTwentyFourHours) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 1000u);
  EXPECT_TRUE(store.needsStatePull(record, 1000u + 86401u));
}

// A clock that jumped backwards must force a pull, not silence one forever.
TEST(BookStateStore, ClockGoingBackwardsForcesAPull) {
  FakeBlobStore blobs;
  BookStateStore store(blobs, kPath);
  auto& record = store.findOrCreate(kMd5);
  BookStateStore::markStatePulled(record, 5000u);
  EXPECT_TRUE(store.needsStatePull(record, 1000u));
}
```

Create `test/bookorbit_state_store/CMakeLists.txt`:

```cmake
add_executable(BookStateStoreTest
  BookStateStoreTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateStore.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(BookStateStoreTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookStateStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStateStoreTest)
```

Add `add_subdirectory(bookorbit_state_store)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookStateStore.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookStateStore.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitBookState.h"
#include "BookStateCodec.h"
#include "IBlobStore.h"

namespace bookorbit {

// A forced pull is the only way a rating written on the web reaches the device,
// so it is bounded by age rather than run on every sync.
inline constexpr uint32_t kStatePullMaxAgeSeconds = 86400;

struct BookStateRecord {
  char md5[33] = {};
  LocalBookState local;
  SyncedBookState synced;
  uint32_t statePulledAt = 0;
};

// Holds every book's status/rating/review plus the last-known-server shadow in
// one atomically-written blob, beside BookOrbitSyncState's watermark blob.
class BookStateStore {
 public:
  BookStateStore(IBlobStore& blobs, std::string path);

  // Returns true on success, including when no file exists yet or the blob is
  // unreadable — a corrupt state file degrades to "re-sync", never to a crash.
  bool load();
  bool flush();

  BookStateRecord* find(std::string_view md5);
  BookStateRecord& findOrCreate(std::string_view md5);

  bool needsStatePull(const BookStateRecord& record, uint32_t nowUnix) const;
  static void markStatePulled(BookStateRecord& record, uint32_t nowUnix);

 private:
  static constexpr uint8_t kFormatVersion = 1;

  IBlobStore& blobs;
  std::string path;
  std::vector<BookStateRecord> records;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookStateStore.cpp`:

```cpp
#include "BookStateStore.h"

#include <cstring>

#include "AtomicBlobWriter.h"

namespace bookorbit {
namespace {

void appendU8(std::vector<uint8_t>& out, const uint8_t value) { out.push_back(value); }

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool readU8(const std::vector<uint8_t>& in, size_t& pos, uint8_t& value) {
  if (pos >= in.size()) return false;
  value = in[pos++];
  return true;
}

bool readU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& value) {
  if (pos + 4 > in.size()) return false;
  value = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
  pos += 4;
  return true;
}

void appendDate(std::vector<uint8_t>& out, const DateOnly& date) {
  appendU32(out, date.year);
  appendU8(out, date.month);
  appendU8(out, date.day);
}

bool readDate(const std::vector<uint8_t>& in, size_t& pos, DateOnly& date) {
  uint32_t year = 0;
  if (!readU32(in, pos, year)) return false;
  if (!readU8(in, pos, date.month)) return false;
  if (!readU8(in, pos, date.day)) return false;
  date.year = static_cast<uint16_t>(year);
  return true;
}

// Review notes reach 10000 bytes, so the length prefix is 32-bit rather than
// the single byte BookOrbitSyncState uses for its short fixed fields.
void appendString(std::vector<uint8_t>& out, const std::string& value) {
  appendU32(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

bool readString(const std::vector<uint8_t>& in, size_t& pos, std::string& value) {
  uint32_t len = 0;
  if (!readU32(in, pos, len)) return false;
  if (len > kReviewNoteMaxBytes) return false;
  if (pos + len > in.size()) return false;
  value.assign(reinterpret_cast<const char*>(in.data() + pos), len);
  pos += len;
  return true;
}

bool readFixed(const std::vector<uint8_t>& in, size_t& pos, char (&dest)[33]) {
  uint8_t len = 0;
  if (!readU8(in, pos, len)) return false;
  if (len >= sizeof(dest) || pos + len > in.size()) return false;
  std::memcpy(dest, in.data() + pos, len);
  dest[len] = '\0';
  pos += len;
  return true;
}

}  // namespace

BookStateStore::BookStateStore(IBlobStore& blobStore, std::string statePath)
    : blobs(blobStore), path(std::move(statePath)) {}

bool BookStateStore::load() {
  records.clear();

  std::vector<uint8_t> raw;
  if (!readBlobWithBackup(blobs, path, raw)) {
    return true;  // nothing persisted yet
  }

  size_t pos = 0;
  uint8_t version = 0;
  if (!readU8(raw, pos, version) || version != kFormatVersion) {
    return true;  // unknown or corrupt format: start clean
  }

  uint32_t count = 0;
  if (!readU32(raw, pos, count)) return true;

  for (uint32_t i = 0; i < count; i++) {
    BookStateRecord record;
    uint8_t statusKnown = 0;
    uint8_t status = 0;
    uint8_t ratingSet = 0;
    uint8_t reviewSet = 0;
    uint8_t syncedRatingKnown = 0;
    uint8_t syncedRatingSet = 0;
    uint8_t syncedReviewKnown = 0;
    uint8_t syncedReviewSet = 0;

    const bool ok = readFixed(raw, pos, record.md5) && readU8(raw, pos, statusKnown) && readU8(raw, pos, status) &&
                    readDate(raw, pos, record.local.statusModified) && readU8(raw, pos, ratingSet) &&
                    readU8(raw, pos, record.local.rating) && readU8(raw, pos, reviewSet) &&
                    readString(raw, pos, record.local.reviewNote) && readDate(raw, pos, record.local.reviewModified) &&
                    readU8(raw, pos, syncedRatingKnown) && readU8(raw, pos, syncedRatingSet) &&
                    readU8(raw, pos, record.synced.rating) && readU8(raw, pos, syncedReviewKnown) &&
                    readU8(raw, pos, syncedReviewSet) && readString(raw, pos, record.synced.reviewNote) &&
                    readDate(raw, pos, record.synced.statusSyncedModified) && readU32(raw, pos, record.statePulledAt);
    if (!ok) {
      records.clear();
      return true;
    }

    record.local.statusKnown = statusKnown != 0;
    record.local.status = status <= static_cast<uint8_t>(BookStatus::Abandoned) ? static_cast<BookStatus>(status)
                                                                               : BookStatus::Reading;
    record.local.ratingSet = ratingSet != 0;
    record.local.reviewSet = reviewSet != 0;
    record.synced.ratingKnown = syncedRatingKnown != 0;
    record.synced.ratingSet = syncedRatingSet != 0;
    record.synced.reviewKnown = syncedReviewKnown != 0;
    record.synced.reviewSet = syncedReviewSet != 0;
    records.push_back(record);
  }
  return true;
}

bool BookStateStore::flush() {
  std::vector<uint8_t> raw;
  raw.reserve(64 + records.size() * 96);
  appendU8(raw, kFormatVersion);
  appendU32(raw, static_cast<uint32_t>(records.size()));

  for (const auto& record : records) {
    const size_t md5Len = std::strlen(record.md5);
    appendU8(raw, static_cast<uint8_t>(md5Len));
    raw.insert(raw.end(), record.md5, record.md5 + md5Len);
    appendU8(raw, record.local.statusKnown ? 1 : 0);
    appendU8(raw, static_cast<uint8_t>(record.local.status));
    appendDate(raw, record.local.statusModified);
    appendU8(raw, record.local.ratingSet ? 1 : 0);
    appendU8(raw, record.local.rating);
    appendU8(raw, record.local.reviewSet ? 1 : 0);
    appendString(raw, truncateReview(record.local.reviewNote));
    appendDate(raw, record.local.reviewModified);
    appendU8(raw, record.synced.ratingKnown ? 1 : 0);
    appendU8(raw, record.synced.ratingSet ? 1 : 0);
    appendU8(raw, record.synced.rating);
    appendU8(raw, record.synced.reviewKnown ? 1 : 0);
    appendU8(raw, record.synced.reviewSet ? 1 : 0);
    appendString(raw, truncateReview(record.synced.reviewNote));
    appendDate(raw, record.synced.statusSyncedModified);
    appendU32(raw, record.statePulledAt);
  }

  return atomicWriteBlob(blobs, path, raw.data(), raw.size());
}

BookStateRecord* BookStateStore::find(const std::string_view md5) {
  for (auto& record : records) {
    if (md5 == record.md5) return &record;
  }
  return nullptr;
}

BookStateRecord& BookStateStore::findOrCreate(const std::string_view md5) {
  if (auto* existing = find(md5)) return *existing;
  BookStateRecord record;
  const size_t len = md5.size() < sizeof(record.md5) - 1 ? md5.size() : sizeof(record.md5) - 1;
  std::memcpy(record.md5, md5.data(), len);
  record.md5[len] = '\0';
  records.push_back(record);
  return records.back();
}

bool BookStateStore::needsStatePull(const BookStateRecord& record, const uint32_t nowUnix) const {
  if (record.statePulledAt == 0) return true;
  if (nowUnix < record.statePulledAt) return true;  // clock went backwards
  return (nowUnix - record.statePulledAt) > kStatePullMaxAgeSeconds;
}

void BookStateStore::markStatePulled(BookStateRecord& record, const uint32_t nowUnix) {
  record.statePulledAt = nowUnix;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookStateStore --output-on-failure
```

Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookStateStore.h lib/BookOrbit/BookStateStore.cpp test/bookorbit_state_store test/CMakeLists.txt
git commit -m "feat: add durable BookOrbit book-state store"
```

---

### Task 7: State phase in the per-book sync sequence

**Files:**
- Create: `lib/BookOrbit/BookStatePhase.h`, `lib/BookOrbit/BookStatePhase.cpp`
- Create: `test/bookorbit_state_phase/CMakeLists.txt`, `test/bookorbit_state_phase/BookStatePhaseTest.cpp`
- Modify: `test/CMakeLists.txt`
- Reference: `bookorbit_book_sync.lua:610-698` (`stepState` — phase ordering and acknowledgement)

**Interfaces:**
- Consumes: `bookorbit::BookOrbitClient::postJson`, `bookorbit::Error`, `bookorbit::Status`, `bookorbit::isAuthError` (P0 Tasks 2 and 6); `bookorbit::SyncStateStore` and `BookSyncState::statusSyncedModified` (P0 Task 5); Tasks 3–6 of this plan.
- Produces:
  `enum class bookorbit::StatePhaseOutcome : uint8_t { Skipped, Synced, Unmatched, AuthFailed, Failed }`;
  `class bookorbit::BookStatePhase` with
  `BookStatePhase(BookOrbitClient& client, SyncStateStore& syncState, BookStateStore& stateStore)`,
  `StatePhaseOutcome run(std::string_view md5, bool forcePull, uint32_t nowUnix)`,
  `const std::string& lastRequestBody() const`.

Path: `/koreader/plugin/book-states`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_state_phase/BookStatePhaseTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/BookStatePhase.h"
#include "lib/BookOrbit/IBlobStore.h"
#include "lib/BookOrbit/IHttpTransport.h"

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

class ScriptedTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  bookorbit::HttpResponse next{200, false, R"({"unmatched":[],"results":[]})"};

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    return next;
  }
};

bookorbit::DateOnly day(const char* text) {
  bookorbit::DateOnly out;
  bookorbit::parseDateOnly(text, out);
  return out;
}

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";
constexpr uint32_t kNow = 1787561449u;

struct Fixture {
  FakeBlobStore blobs;
  ScriptedTransport transport;
  bookorbit::SyncStateStore syncState{blobs, "/bookorbit_state.bin"};
  bookorbit::BookStateStore stateStore{blobs, "/bookorbit_states.bin"};
  bookorbit::BookOrbitClient client{transport, "https://books.example.com/api/v1", "monish",
                                    "5f4dcc3b5aa765d61d8327deb882cf99",
                                    bookorbit::DeviceIdentity{"crossink-abc123", "Xteink X4 Pro", "0.1.0"}};
  bookorbit::BookStatePhase phase{client, syncState, stateStore};
};

}  // namespace

using bookorbit::BookStatus;
using bookorbit::StatePhaseOutcome;

TEST(BookStatePhase, UnchangedBookSendsNothing) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow);
  auto& book = fx.syncState.findOrCreate(kMd5);
  std::snprintf(book.statusSyncedModified, sizeof(book.statusSyncedModified), "2026-08-21");

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Skipped);
  EXPECT_TRUE(fx.transport.sent.empty());
}

TEST(BookStatePhase, PostsToTheBookStatesPath) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  ASSERT_EQ(fx.transport.sent.size(), 1u);
  EXPECT_EQ(fx.transport.sent[0].method, "POST");
  EXPECT_EQ(fx.transport.sent[0].url, "https://books.example.com/api/v1/koreader/plugin/book-states");
  EXPECT_NE(fx.transport.sent[0].body.find(R"("rating":4)"), std::string::npos);
}

// withDevice injection is the client's job, but the phase must go through it.
TEST(BookStatePhase, RequestCarriesDeviceFields) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const std::string& body = fx.transport.sent[0].body;
  EXPECT_NE(body.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  EXPECT_NE(body.find(R"("deviceModel":"Xteink X4 Pro")"), std::string::npos);
  EXPECT_NE(body.find(R"("pluginVersion":"0.1.0")"), std::string::npos);
  EXPECT_NE(body.find(R"("deviceTime":)"), std::string::npos);
}

TEST(BookStatePhase, ForcedPullSendsABareHash) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow);

  ASSERT_EQ(fx.phase.run(kMd5, true, kNow), StatePhaseOutcome::Synced);
  ASSERT_EQ(fx.transport.sent.size(), 1u);
  EXPECT_NE(fx.transport.sent[0].body.find(std::string(R"("hash":")") + kMd5 + '"'), std::string::npos);
  EXPECT_EQ(fx.transport.sent[0].body.find(R"("status":)"), std::string::npos);
}

TEST(BookStatePhase, StalePullAgeForcesARequestWithoutAnyChange) {
  Fixture fx;
  auto& record = fx.stateStore.findOrCreate(kMd5);
  record.local.setStatus(BookStatus::Complete, day("2026-08-21"));
  record.synced.statusSyncedModified = day("2026-08-21");
  bookorbit::BookStateStore::markStatePulled(record, kNow - 86401u);

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  EXPECT_EQ(fx.transport.sent.size(), 1u);
}

TEST(BookStatePhase, ServerRatingIsAppliedLocally) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(2, day("2026-08-20"));
  fx.transport.next = {200, false,
                       R"({"unmatched":[],"results":[{"hash":"0f0a792b00a37cf80baa5e50c078b31f",)"
                       R"("ratingSet":true,"rating":5,"ratingUpdatedAt":"2026-08-21"}]})"};

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* record = fx.stateStore.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->local.rating, 5);
  EXPECT_TRUE(record->synced.ratingKnown);
  EXPECT_EQ(record->synced.rating, 5);
}

TEST(BookStatePhase, SyncedShadowRecordsWhatWasUploaded) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* record = fx.stateStore.find(kMd5);
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->synced.ratingKnown);
  EXPECT_TRUE(record->synced.ratingSet);
  EXPECT_EQ(record->synced.rating, 4);
}

// A server-kept tie still counts as synced: the device value was considered.
TEST(BookStatePhase, StatusWatermarkAdvancesOnSuccess) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));

  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);
  const auto* book = fx.syncState.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->statusSyncedModified, "2026-08-21");
  EXPECT_EQ(fx.stateStore.find(kMd5)->synced.statusSyncedModified.day, 21);
}

TEST(BookStatePhase, UnmatchedBookDoesNotAdvanceTheWatermark) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {200, false, R"({"unmatched":["0f0a792b00a37cf80baa5e50c078b31f"],"results":[]})"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Unmatched);
  const auto* book = fx.syncState.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_STREQ(book->statusSyncedModified, "");
}

TEST(BookStatePhase, ServerErrorLeavesTheWatermarkUnadvanced) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {503, false, "upstream unavailable"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

TEST(BookStatePhase, AuthErrorIsReportedDistinctly) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {401, false, "unauthorized"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::AuthFailed);
}

TEST(BookStatePhase, TransportFailureIsAFailureNotACrash) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {0, true, ""};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

TEST(BookStatePhase, MalformedResponseFails) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setStatus(BookStatus::Complete, day("2026-08-21"));
  fx.transport.next = {200, false, "{not json"};

  EXPECT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Failed);
  EXPECT_STREQ(fx.syncState.find(kMd5)->statusSyncedModified, "");
}

// The phase must be durable before annotations run: a crash after the POST but
// before the flush would otherwise re-upload, and re-uploads are idempotent,
// but a lost pull would be silently dropped.
TEST(BookStatePhase, SuccessIsPersistedBeforeReturning) {
  Fixture fx;
  fx.stateStore.findOrCreate(kMd5).local.setRating(4, day("2026-08-21"));
  ASSERT_EQ(fx.phase.run(kMd5, false, kNow), StatePhaseOutcome::Synced);

  bookorbit::BookStateStore reloaded(fx.blobs, "/bookorbit_states.bin");
  ASSERT_TRUE(reloaded.load());
  ASSERT_NE(reloaded.find(kMd5), nullptr);
  EXPECT_EQ(reloaded.find(kMd5)->synced.rating, 4);

  bookorbit::SyncStateStore reloadedSync(fx.blobs, "/bookorbit_state.bin");
  ASSERT_TRUE(reloadedSync.load());
  ASSERT_NE(reloadedSync.find(kMd5), nullptr);
  EXPECT_STREQ(reloadedSync.find(kMd5)->statusSyncedModified, "2026-08-21");
}
```

Create `test/bookorbit_state_phase/CMakeLists.txt`:

```cmake
add_executable(BookStatePhaseTest
  BookStatePhaseTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStatePhase.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateStore.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateMerge.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookStateCodec.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBookState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitDate.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookStatePhaseTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookStatePhaseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookStatePhaseTest)
```

Add `add_subdirectory(bookorbit_state_phase)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookStatePhase.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookStatePhase.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "BookOrbitClient.h"
#include "BookOrbitSyncState.h"
#include "BookStateStore.h"

namespace bookorbit {

// The state phase's place in the per-book chain is
// match -> stats -> progress -> state -> annotations -> bookmarks.
inline constexpr char kBookStatesPath[] = "/koreader/plugin/book-states";

enum class StatePhaseOutcome : uint8_t {
  Skipped,      // nothing changed and no pull was due — no request was made
  Synced,       // request succeeded; watermark advanced and persisted
  Unmatched,    // the server does not hold this book; watermark unchanged
  AuthFailed,   // 401/403 — the caller aborts the whole sync
  Failed,       // any other error; watermark unchanged, retried next trigger
};

// Runs one book's state phase. There is no retry loop by design: a failure
// leaves the watermark unadvanced and is retried on the next sync trigger.
class BookStatePhase {
 public:
  BookStatePhase(BookOrbitClient& client, SyncStateStore& syncState, BookStateStore& stateStore);

  StatePhaseOutcome run(std::string_view md5, bool forcePull, uint32_t nowUnix);

  const std::string& lastRequestBody() const { return requestBody; }

 private:
  BookOrbitClient& client;
  SyncStateStore& syncState;
  BookStateStore& stateStore;
  std::string requestBody;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookStatePhase.cpp`:

```cpp
#include "BookStatePhase.h"

#include <cstring>
#include <vector>

#include "BookStateCodec.h"
#include "BookStateMerge.h"

namespace bookorbit {
namespace {

void copyDateInto(char (&dest)[11], const DateOnly& date) { formatDateOnly(date, dest, sizeof(dest)); }

}  // namespace

BookStatePhase::BookStatePhase(BookOrbitClient& httpClient, SyncStateStore& sync, BookStateStore& states)
    : client(httpClient), syncState(sync), stateStore(states) {}

StatePhaseOutcome BookStatePhase::run(const std::string_view md5, const bool forcePull, const uint32_t nowUnix) {
  requestBody.clear();

  BookStateRecord& record = stateStore.findOrCreate(md5);
  const bool pullDue = forcePull || stateStore.needsStatePull(record, nowUnix);

  BookStatePayload payload;
  if (!buildStatePayload(md5, record.local, record.synced, pullDue, payload)) {
    return StatePhaseOutcome::Skipped;
  }

  std::vector<BookStatePayload> batch;
  batch.reserve(1);
  batch.push_back(payload);
  requestBody = encodeBookStates(batch);

  std::string responseBody;
  const Error error = client.postJson(kBookStatesPath, requestBody, responseBody);
  if (isAuthError(error)) {
    return StatePhaseOutcome::AuthFailed;
  }
  if (error.status != Status::Ok) {
    return StatePhaseOutcome::Failed;
  }

  std::vector<std::string> unmatched;
  std::vector<ServerBookState> results;
  if (!decodeBookStates(responseBody, unmatched, results)) {
    return StatePhaseOutcome::Failed;
  }
  for (const auto& hash : unmatched) {
    if (hash == record.md5) {
      return StatePhaseOutcome::Unmatched;
    }
  }

  const ServerBookState* result = nullptr;
  for (const auto& candidate : results) {
    if (candidate.hash == record.md5) {
      result = &candidate;
      break;
    }
  }

  if (result != nullptr) {
    applyMerge(resolveBookState(record.local, *result), record.local);
    if (result->ratingKnown) {
      record.synced.ratingKnown = true;
      record.synced.ratingSet = record.local.ratingSet;
      record.synced.rating = record.local.rating;
    }
    if (result->reviewKnown) {
      record.synced.reviewKnown = true;
      record.synced.reviewSet = record.local.reviewSet;
      record.synced.reviewNote = record.local.reviewNote;
    }
  }

  // Whatever was asserted in the payload is now the server's view, even when
  // the server kept its own value on a tie: the device value was considered.
  if (payload.hasRating || payload.ratingCleared) {
    record.synced.ratingKnown = true;
    if (result == nullptr || !result->ratingKnown) {
      record.synced.ratingSet = payload.hasRating;
      record.synced.rating = payload.hasRating ? payload.rating : 0;
    }
  }
  if (payload.hasReview || payload.reviewCleared) {
    record.synced.reviewKnown = true;
    if (result == nullptr || !result->reviewKnown) {
      record.synced.reviewSet = payload.hasReview;
      record.synced.reviewNote = payload.hasReview ? payload.reviewNote : std::string();
    }
  }
  if (payload.statusModified.valid()) {
    record.synced.statusSyncedModified = payload.statusModified;
  }
  if (pullDue) {
    BookStateStore::markStatePulled(record, nowUnix);
  }

  // The watermark lives in P0's per-book record so every phase's durability
  // story stays in one file.
  BookSyncState& book = syncState.findOrCreate(md5);
  if (payload.statusModified.valid()) {
    copyDateInto(book.statusSyncedModified, payload.statusModified);
  }

  // Persist before returning: the annotations phase must never start on top of
  // an unacknowledged state result.
  if (!stateStore.flush() || !syncState.flush()) {
    return StatePhaseOutcome::Failed;
  }
  return StatePhaseOutcome::Synced;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookStatePhase --output-on-failure
```

Expected: 14 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookStatePhase.h lib/BookOrbit/BookStatePhase.cpp test/bookorbit_state_phase test/CMakeLists.txt
git commit -m "feat: wire BookOrbit book-state phase into the per-book sequence"
```

---

### Task 8: Rating menu model and the 1–5 star reader UI

**Files:**
- Create: `src/activities/reader/BookRatingMenuModel.h`
- Create: `test/book_rating_menu_model/CMakeLists.txt`, `test/book_rating_menu_model/BookRatingMenuModelTest.cpp`
- Modify: `test/CMakeLists.txt`
- Modify: `lib/I18n/translations/english.yaml` (add `STR_BOOKORBIT_RATING*` keys, then regenerate)
- Modify: `src/activities/reader/EpubReaderMenuModel.h` (add `SET_RATING` to `EpubReaderMenuAction`)
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp` (add the Settings-tab row)
- Modify: `src/activities/reader/EpubReaderActivity.h`, `src/activities/reader/EpubReaderActivity.cpp` (handle the action)
- Reference: `src/activities/reader/EpubReaderMenuActivity.cpp:276-277` (the `TOGGLE_COMPLETED` row it sits beside), `src/components/OptionPopup.h:29-40` (the popup API)

**Interfaces:**
- Consumes: `bookorbit::BookStateStore`, `LocalBookState::setRating` / `clearRating` (Tasks 2 and 6).
- Produces:
  `inline constexpr int kRatingOptionCount = 6`;
  `constexpr int ratingForOptionIndex(int index)` — `0` means "not rated";
  `constexpr int optionIndexForRating(bool ratingSet, uint8_t rating)`;
  `void ratingStarsLabel(int rating, char* buf, size_t len)`.

- [ ] **Step 1: Write the failing test**

Create `test/book_rating_menu_model/BookRatingMenuModelTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "src/activities/reader/BookRatingMenuModel.h"

TEST(BookRatingMenuModel, HasSixOptions) {
  EXPECT_EQ(kRatingOptionCount, 6);
}

TEST(BookRatingMenuModel, FirstOptionIsNotRated) {
  EXPECT_EQ(ratingForOptionIndex(0), 0);
}

TEST(BookRatingMenuModel, OptionsOneThroughFiveMapToStars) {
  for (int index = 1; index <= 5; index++) {
    EXPECT_EQ(ratingForOptionIndex(index), index) << index;
  }
}

TEST(BookRatingMenuModel, OutOfRangeIndexFallsBackToNotRated) {
  EXPECT_EQ(ratingForOptionIndex(-1), 0);
  EXPECT_EQ(ratingForOptionIndex(6), 0);
  EXPECT_EQ(ratingForOptionIndex(99), 0);
}

TEST(BookRatingMenuModel, UnsetRatingSelectsTheFirstOption) {
  EXPECT_EQ(optionIndexForRating(false, 0), 0);
  EXPECT_EQ(optionIndexForRating(false, 4), 0);
}

TEST(BookRatingMenuModel, SetRatingSelectsItsOwnOption) {
  EXPECT_EQ(optionIndexForRating(true, 1), 1);
  EXPECT_EQ(optionIndexForRating(true, 5), 5);
}

TEST(BookRatingMenuModel, CorruptRatingSelectsNotRated) {
  EXPECT_EQ(optionIndexForRating(true, 0), 0);
  EXPECT_EQ(optionIndexForRating(true, 9), 0);
}

TEST(BookRatingMenuModel, LabelDrawsFilledAndEmptyStars) {
  char buf[32] = {};
  ratingStarsLabel(3, buf, sizeof(buf));
  EXPECT_STREQ(buf, "***..");
  ratingStarsLabel(5, buf, sizeof(buf));
  EXPECT_STREQ(buf, "*****");
  ratingStarsLabel(1, buf, sizeof(buf));
  EXPECT_STREQ(buf, "*....");
}

TEST(BookRatingMenuModel, LabelForNotRatedIsEmpty) {
  char buf[32] = {'x', '\0'};
  ratingStarsLabel(0, buf, sizeof(buf));
  EXPECT_STREQ(buf, "");
}

TEST(BookRatingMenuModel, LabelNeverOverrunsASmallBuffer) {
  char buf[4] = {};
  ratingStarsLabel(5, buf, sizeof(buf));
  EXPECT_EQ(std::string(buf).size(), 3u);
}
```

Create `test/book_rating_menu_model/CMakeLists.txt`:

```cmake
add_executable(BookRatingMenuModelTest
  BookRatingMenuModelTest.cpp
)

target_include_directories(BookRatingMenuModelTest PRIVATE
  ${REPO_ROOT}
)

target_link_libraries(BookRatingMenuModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookRatingMenuModelTest)
```

Add `add_subdirectory(book_rating_menu_model)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `src/activities/reader/BookRatingMenuModel.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `src/activities/reader/BookRatingMenuModel.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>

// Option list for the reader menu's rating picker: "Not rated" plus 1..5 stars.
// Header-only and free of device types so the mapping is host-testable.
inline constexpr int kRatingOptionCount = 6;

// Returns the rating an option index selects; 0 means "not rated".
constexpr int ratingForOptionIndex(const int index) {
  if (index < 1 || index >= kRatingOptionCount) return 0;
  return index;
}

// Returns the option index that should start focused for a stored rating.
constexpr int optionIndexForRating(const bool ratingSet, const uint8_t rating) {
  if (!ratingSet || rating < 1 || rating > 5) return 0;
  return static_cast<int>(rating);
}

// Writes an ASCII star bar such as "***..". Empty for "not rated". The reader
// renders this beside the row label, so it must fit whatever buffer it is given.
inline void ratingStarsLabel(const int rating, char* buf, const size_t len) {
  if (buf == nullptr || len == 0) return;
  if (rating < 1 || rating > 5) {
    buf[0] = '\0';
    return;
  }
  size_t written = 0;
  for (int i = 0; i < 5 && written + 1 < len; i++) {
    buf[written++] = (i < rating) ? '*' : '.';
  }
  buf[written] = '\0';
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookRatingMenuModel --output-on-failure
```

Expected: 10 tests PASS.

- [ ] **Step 5: Add translation keys and wire the UI**

Add to `lib/I18n/translations/english.yaml`, beside the existing `STR_MARK_FINISHED` entry:

```yaml
STR_BOOKORBIT_RATING: "Rating"
STR_BOOKORBIT_RATING_NONE: "Not rated"
STR_BOOKORBIT_RATING_1: "1 star"
STR_BOOKORBIT_RATING_2: "2 stars"
STR_BOOKORBIT_RATING_3: "3 stars"
STR_BOOKORBIT_RATING_4: "4 stars"
STR_BOOKORBIT_RATING_5: "5 stars"
STR_BOOKORBIT_RATING_SAVED: "Rating saved"
```

Regenerate:

```bash
python3 scripts/gen_i18n.py
```

Do not hand-edit `lib/I18n/I18nKeys.h` or `I18nStrings.{h,cpp}` — they are generated.

Add `SET_RATING,` to `EpubReaderMenuAction` in `src/activities/reader/EpubReaderMenuModel.h`, after `TOGGLE_COMPLETED`.

In `src/activities/reader/EpubReaderMenuActivity.cpp`, in `buildMenuItems`, insert the row immediately before the `TOGGLE_COMPLETED` push_back at line 276:

```cpp
  settingsItems.push_back({MenuAction::SET_RATING, StrId::STR_BOOKORBIT_RATING});
```

In `src/activities/reader/EpubReaderActivity.cpp`, handle the action beside the existing `TOGGLE_COMPLETED` case by showing the shared `OptionPopup`:

```cpp
void EpubReaderActivity::showRatingPopup() {
  static const StrId kRatingOptions[kRatingOptionCount] = {
      StrId::STR_BOOKORBIT_RATING_NONE, StrId::STR_BOOKORBIT_RATING_1, StrId::STR_BOOKORBIT_RATING_2,
      StrId::STR_BOOKORBIT_RATING_3,    StrId::STR_BOOKORBIT_RATING_4, StrId::STR_BOOKORBIT_RATING_5};

  auto& record = BOOKORBIT_STATES.findOrCreate(bookOrbitHash);
  const int current = optionIndexForRating(record.local.ratingSet, record.local.rating);

  ratingPopup.show(StrId::STR_BOOKORBIT_RATING, kRatingOptions, kRatingOptionCount, current, [this](const int index) {
    auto& selected = BOOKORBIT_STATES.findOrCreate(bookOrbitHash);
    bookorbit::DateOnly today;
    if (!currentLocalDateOnly(today)) {
      // Without the RTC there is no date to resolve a conflict with, so the
      // edit is refused rather than stamped with a fabricated day.
      LOG_ERR("BookOrbit: rating edit refused, no wall clock");
      return;
    }
    const int rating = ratingForOptionIndex(index);
    if (rating == 0) {
      selected.local.clearRating(today);
    } else {
      selected.local.setRating(rating, today);
    }
    if (!BOOKORBIT_STATES.flush()) {
      LOG_ERR("BookOrbit: could not persist rating");
      return;
    }
    showTransientToast(tr(STR_BOOKORBIT_RATING_SAVED));
  });
}
```

Declare `void showRatingPopup();`, `OptionPopup ratingPopup;`, and `std::string bookOrbitHash;` in `src/activities/reader/EpubReaderActivity.h` beside the existing popup members. `bookOrbitHash` is filled in `onEnter()` from `KOReaderDocumentId::calculate()`, the same partial MD5 every other phase keys on. `currentLocalDateOnly` converts `getCurrentLocalReadingStatsDateTime()` into a `bookorbit::DateOnly`; add it next to the other reader helpers:

```cpp
bool EpubReaderActivity::currentLocalDateOnly(bookorbit::DateOnly& out) const {
  ReadingStatsDateTime now;
  if (!getCurrentLocalReadingStatsDateTime(now)) return false;
  out.year = now.date.year;
  out.month = now.date.month;
  out.day = now.date.day;
  return out.valid();
}
```

- [ ] **Step 6: Verify it builds and runs**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
pio run -e x4-pro && pio run -e default && pio run -e sticky
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib test -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

Expected: all tests pass, all four environments link, the smoke test completes with no crash.

- [ ] **Step 7: Commit**

```bash
git add src/activities/reader/BookRatingMenuModel.h src/activities/reader/EpubReaderMenuModel.h src/activities/reader/EpubReaderMenuActivity.cpp src/activities/reader/EpubReaderActivity.h src/activities/reader/EpubReaderActivity.cpp lib/I18n/translations/english.yaml lib/I18n test/book_rating_menu_model test/CMakeLists.txt
git commit -m "feat: add 1-5 star book rating to the reader menu"
```

---

### Task 9: Map "mark as finished" onto the status field

**Files:**
- Modify: `lib/BookOrbit/BookOrbitBookState.h`, `lib/BookOrbit/BookOrbitBookState.cpp` (add `applyCompletionToggle`)
- Modify: `test/bookorbit_book_state/BookOrbitBookStateTest.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp` (`setBookCompleted`, line 5058)
- Modify: `src/activities/reader/XtcReaderActivity.cpp` (`setBookCompleted`, line 763)
- Modify: `src/activities/home/BookActions.cpp` (`toggleBookCompleted`, line 149)
- Modify: `src/activities/reader/BookStatsActivity.cpp` (`applyCompletedState`, line 132)
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `LocalBookState`, `statusFromCompletion` (Task 2); `BookStateStore` (Task 6).
- Produces: `bool bookorbit::applyCompletionToggle(LocalBookState& local, bool isCompleted, const DateOnly& today)` — returns false and changes nothing when the date is unusable or the status already matches.

- [ ] **Step 1: Write the failing test**

Append to `test/bookorbit_book_state/BookOrbitBookStateTest.cpp`:

```cpp
TEST(BookOrbitBookState, CompletionToggleStampsCompleteAndTheDate) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));

  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, today));
  EXPECT_TRUE(state.statusKnown);
  EXPECT_EQ(state.status, BookStatus::Complete);
  EXPECT_EQ(state.statusModified.day, 21);
}

TEST(BookOrbitBookState, UncompletingStampsReadingAndTheDate) {
  LocalBookState state;
  bookorbit::DateOnly first;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", first));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, first));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, false, later));
  EXPECT_EQ(state.status, BookStatus::Reading);
  EXPECT_EQ(state.statusModified.day, 25);
}

// Re-marking an already-complete book must not bump the date, or a harmless
// re-open would win every conflict against a genuine server edit.
TEST(BookOrbitBookState, RepeatedToggleDoesNotBumpTheDate) {
  LocalBookState state;
  bookorbit::DateOnly first;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", first));
  ASSERT_TRUE(bookorbit::applyCompletionToggle(state, true, first));

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, true, later));
  EXPECT_EQ(state.statusModified.day, 21);
}

// Without the RTC there is no date to resolve a conflict with. Refuse rather
// than stamping a fabricated day, exactly as the event log refuses a startTime.
TEST(BookOrbitBookState, InvalidDateRefusesTheToggle) {
  LocalBookState state;
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, true, bookorbit::DateOnly{}));
  EXPECT_FALSE(state.statusKnown);
}

TEST(BookOrbitBookState, AbandonedIsNotOverwrittenByAnUncompletedToggle) {
  LocalBookState state;
  bookorbit::DateOnly today;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-21", today));
  state.setStatus(BookStatus::Abandoned, today);

  bookorbit::DateOnly later;
  ASSERT_TRUE(bookorbit::parseDateOnly("2026-08-25", later));
  EXPECT_FALSE(bookorbit::applyCompletionToggle(state, false, later));
  EXPECT_EQ(state.status, BookStatus::Abandoned);
  EXPECT_EQ(state.statusModified.day, 21);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'applyCompletionToggle' is not a member of 'bookorbit'`.

- [ ] **Step 3: Write minimal implementation**

Add to `lib/BookOrbit/BookOrbitBookState.h`, inside `namespace bookorbit` after `statusFromCompletion`:

```cpp
// Applies CrossInk's "mark as finished" toggle to the synced state. Returns
// false — changing nothing — when the resulting status already matches or the
// date is unusable, so an idle re-open never wins a conflict.
bool applyCompletionToggle(LocalBookState& local, bool isCompleted, const DateOnly& today);
```

Add to `lib/BookOrbit/BookOrbitBookState.cpp`:

```cpp
bool applyCompletionToggle(LocalBookState& local, const bool isCompleted, const DateOnly& today) {
  if (!today.valid()) return false;
  const BookStatus next = statusFromCompletion(isCompleted, local.statusKnown ? local.status : BookStatus::Reading);
  if (local.statusKnown && local.status == next) return false;
  local.setStatus(next, today);
  return true;
}
```

Then call it from each of CrossInk's four completion-toggle sites. In
`src/activities/reader/EpubReaderActivity.cpp:5058`, after `stats.isCompleted = isCompleted;`:

```cpp
  bookorbit::DateOnly today;
  if (currentLocalDateOnly(today)) {
    auto& state = BOOKORBIT_STATES.findOrCreate(bookOrbitHash);
    if (bookorbit::applyCompletionToggle(state.local, isCompleted, today) && !BOOKORBIT_STATES.flush()) {
      LOG_ERR("BookOrbit: could not persist book status");
    }
  }
```

Apply the same three lines, with the activity's own hash and date helper, in
`src/activities/reader/XtcReaderActivity.cpp:768`,
`src/activities/home/BookActions.cpp:182` (after `stats.isCompleted = completed;`), and
`src/activities/reader/BookStatsActivity.cpp:137` (after `stats.isCompleted = completed;`).
In the two non-reader sites the hash comes from `KOReaderDocumentId::calculate(fullPath)`;
the date from `getCurrentLocalReadingStatsDateTime()`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
pio run -e x4-pro && pio run -e default && pio run -e sticky
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib test -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

Expected: 17 `BookOrbitBookState` tests PASS, the whole CTest suite is green, all four environments link, the smoke test completes.

- [ ] **Step 5: Add the changelog entry and commit**

Under an `### Added` heading in `CHANGELOG.md`:

```markdown
- BookOrbit book states: reading status, a 1-5 star rating, and review notes now sync two-way with your BookOrbit server. Marking a book as finished sets its status to "complete", and the newer of the two sides wins when the same book was changed in both places.
```

```bash
git add lib/BookOrbit/BookOrbitBookState.h lib/BookOrbit/BookOrbitBookState.cpp test/bookorbit_book_state src/activities/reader/EpubReaderActivity.cpp src/activities/reader/XtcReaderActivity.cpp src/activities/reader/BookStatsActivity.cpp src/activities/home/BookActions.cpp CHANGELOG.md
git commit -m "feat: map mark-as-finished onto the BookOrbit status field"
```

---

## Hardware Verification

After Task 9, on an X4 Pro with an SD card, a working RTC, and a configured BookOrbit server:

1. Open a book that `match-check` has already matched. Reader menu → Settings tab → **Rating**. Pick 4 stars. Expect the `Rating saved` toast.
2. Trigger a sync (close the book, or Settings → BookOrbit Sync → sync now). In the serial log expect one `POST /koreader/plugin/book-states` carrying `"rating":4` and a `statusModified` date equal to today.
3. Open the same book on the BookOrbit web UI. Expect 4 stars.
4. Change the rating to 2 on the web, wait past the forced-pull window or trigger a manual sync, and reopen the rating row on the device. Expect 2 stars — the server's date is newer, so it wins.
5. Set the rating to "Not rated" on the device and sync. Expect `"ratingCleared":true` in the request body and an empty rating on the web. An absent field here would be a bug.
6. Reader menu → **Mark as Finished**. Sync. Expect `"status":"complete"` on the wire and the book shown as complete on the web.
7. Un-mark it, sync, and confirm the status returns to `reading`.
8. Mark a book as abandoned on the web, sync, then un-mark "finished" on the device. The status must stay `abandoned` — CrossInk has no abandoned gesture and must not overwrite one.
9. Edit a rating on the device and the same book on the web **on the same day**. Sync. Expect the server's value to win the tie, on the device, with no flapping on a second sync.
10. Pull the battery immediately after the `book-states` POST returns. On reboot, sync again: the state must not be re-applied twice, and no watermark may have advanced past unacknowledged data (`/.crosspoint/bookorbit/bookorbit_states.bin` and `bookorbit_state.bin` are both flushed before the phase returns).
11. Disable the RTC (or boot with no NTP sync) and open the rating row. Expect the edit to be refused with `BookOrbit: rating edit refused, no wall clock` in the log rather than a rating stamped with a fabricated date.
12. Point at an unreachable server and sync. Expect the phase to fail, the log to show no retry loop, and the local rating to survive untouched for the next trigger.

## Self-Review Notes

- **Spec coverage:** every clause of the spec's P3 section is claimed by a task. The endpoint and its 200-book batch (Task 3, `kBookStateBatchSize`); the request shape including `status`, `statusModified`, `rating`, `reviewNote`, `reviewModified` (Task 3); the response shape `ratingSet` / `rating` / `ratingUpdatedAt` / `reviewNoteSet` / `reviewNote` / `reviewUpdatedAt` plus `unmatched` (Task 4); explicit `ratingCleared` / `reviewCleared` (Tasks 3 and 4, with tests that assert the absent-field case is *not* used); the `isCompleted` mapping (Tasks 2 and 9); the 1–5 rating UI (Task 8); date-only modification tracking and conflict resolution (Tasks 1 and 5); phase wiring (Task 7).
- **Type consistency:** `DateOnly` (Task 1) is the only date type across Tasks 2–9 — no `std::string` dates survive past the codec boundary. `LocalBookState` (Task 2) is what the store persists (Task 6), what the encoder reads (Task 3), and what the merge writes (Task 5). `SyncedBookState.statusSyncedModified` is the in-memory mirror of P0's `BookSyncState::statusSyncedModified`; Task 7 is the only place that writes the P0 field, so the two cannot drift.
- **Batching:** Task 7 posts one book at a time because the per-book phase chain is per-book by construction. `kBookStateBatchSize` and the vector-taking `encodeBookStates` exist so the library-wide sweep — which is not part of P3 — can fill a 200-book batch without a second encoder. The encoder is tested with multi-book input for that reason.
- **Deliberately out of scope:** authoring a review note on-device. The spec calls for "a 1–5 rating UI" and says nothing about a review editor; P3 pulls, stores, conflict-resolves, and re-uploads review notes faithfully, but the only way to write one is the web UI. The store and codec already carry everything a later text-entry flow would need.
- **Why "abandoned" is sticky:** CrossInk's UI is a two-state toggle, so a naive mapping would rewrite a server-set `abandoned` to `reading` on the next push and undo the user's web edit. `statusFromCompletion` preserves it instead, and Task 9's test locks that behaviour in.
- **Why a refused edit beats a fabricated date:** conflict resolution is entirely date-driven. An edit stamped with a wrong date wins or loses arbitrarily and corrupts the other side. The spec already takes this position for event `startTime`; the same rule is applied here.
- **Not touched:** `BookReadingStats`'s on-disk format. Ratings and reviews live in the new `bookorbit_states.bin` rather than a `stats_v6.bin`, so no cache-format version bump and no `docs/file-formats.md` migration entry are needed, and the fork stays mergeable against upstream CrossInk.
