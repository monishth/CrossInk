# BookOrbit P2 — High-Fidelity Progress Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two-way reading position at xpointer fidelity. Fix the defect that makes every real KOReader xpointer miss (`ProgressMapper` matches the literal `"/body/DocFragment["` and emits paths without `[1]` indices), extend the resolver from paragraph granularity to full element ancestry, and add the three BookOrbit progress endpoints with a hard "never degrade silently" rule.

> **Before starting: the corpus does not exist yet.** Task 1's generator
> scripts are specified in this plan but have **not been run**, and no fixture
> CSVs are committed. Earlier drafts of this plan quoted specific corpus figures
> (404 rows, 16 synthetic) as measured; those numbers were not reproducible and
> have been removed. Generate the corpus first, record the real counts it
> prints, then pin them in the guard tests. Treat any remaining figure in this
> document as illustrative until you have produced it yourself.

**Ground truth is generated, not guessed.** Task 1 drives the KOReader emulator on this machine to emit genuine crengine xpointers for every EPUB in `test/epubs/`, at two DOM versions, and commits them as CSV fixtures. The corpus covers every fixture EPUB at two DOM versions. Every later task is measured against it.

**Architecture:** The xpointer grammar, the streaming ancestry resolver, the codecs, and the degrade policy all live in `lib/BookOrbit/` behind plain buffers — no Arduino, no `Epub`, no HTTP — so the whole phase runs under the native GoogleTest suite. `lib/KOReaderSync/ProgressMapper.cpp` and `ChapterXPathResolver.cpp` are then reduced to *callers* of that grammar; they keep their existing public signatures so no activity changes shape.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), expat for XHTML streaming, `lib/JsonParser/StreamingJsonParser` for response parsing. Corpus generation only: the KOReader emulator at `/home/monish/repos/koreader/koreader-emulator-x86_64-pc-linux-gnu-debug/koreader` (LuaJIT 2.1.1785763465), never needed by CI.

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md` — section "P2 — High-fidelity progress sync"

**Depends on:** P0 (`docs/superpowers/plans/2026-09-11-bookorbit-p0-client-and-state.md`) for `BookOrbitClient`, `IHttpTransport`, `Error`/`Status`, `CapabilityCache`, and `SyncStateStore`.

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
- Every `/koreader/plugin/*` POST body additionally carries `deviceId`, `deviceModel`, `pluginVersion`, and `deviceTime` (local `%Y-%m-%d %H:%M:%S`), injected by `BookOrbitClient`'s `withDevice` path (P0 Task 6) — the P2 encoders never emit them.
- Batch limit: bulk progress **100** items (`kBulkProgressBatchSize = 100`).
- Endpoints this phase owns: `GET /koreader/syncs/progress/{digest}`, `PUT /koreader/syncs/progress`, `POST /koreader/plugin/progress`.
- **Never degrade silently:** always send *both* `progress` (xpointer) and `percentage`. On receive prefer the xpointer; fall back to percentage only when resolution fails, and surface that in the UI when the resulting jump exceeds a threshold.
- Error convention: non-2xx yields `(status, decodedBody)`; transport failure yields a string reason. `401`/`403` and transport errors abort the whole sync; other numeric errors mark the phase failed, leave its watermark unadvanced, and move on.
- Canonical xpointer form: explicit index on **every** step, `.N` offset suffix — `/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0`.
- **Xpointers are only comparable within a crengine DOM version.** crengine normalizes xpointers from DOM version **20200223** onward (`getDomVersionWithNormalizedXPointers()`); below it the same position is written without `[1]` indices. Oldest supported is **20171225**, latest is **20260812**. The device emits the normalized form; older forms are accepted on ingest, best effort, and never re-emitted verbatim.
- **crengine inserts synthetic elements that are not in the source XHTML** and they appear as xpointer steps: `autoBoxing`, `tabularBox`, `rubyBox`, `mathBox`, `floatBox`, `inlineBox` (the boxing range `EL_BOXING_START`..`EL_BOXING_END`, `crengine/include/fb2def.h:40-64`) plus `pseudoElem`. They must be stripped before an ancestry is matched against streamed XHTML, and stripping can lose same-name sibling disambiguation — a documented, bounded loss, never a silent one.
- Ground-truth fixtures live in `test/bookorbit_xpointer_corpus/fixtures/` and are committed. Regenerating them needs the KOReader emulator and `SDL_VIDEODRIVER=dummy`; running the tests does not.
- Large responses are read through `lib/JsonParser/StreamingJsonParser` (512-byte token buffer, 32 nesting levels, constant memory), never ArduinoJson.
- Do not edit generated files: `src/network/html/*.generated.h`, `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`, icon headers, hyphenation tries.
- Add a `CHANGELOG.md` entry for user-facing changes, grouped under Added/Changed/Fixed.
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally: `pio run -e x4-pro` and `pio run -e default`.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/XPointer.{h,cpp}` | crengine xpointer grammar: tolerant parse, canonical emit, normalize, DocFragment helpers, DOM-version constants and synthetic-element stripping. **The defect fix.** |
| `lib/BookOrbit/XPointerResolver.{h,cpp}` | Streaming XHTML ancestry resolver: xpointer ↔ visible-codepoint offset, full element ancestry with same-name sibling counting. |
| `lib/BookOrbit/BookOrbitProgress.{h,cpp}` | `GET /koreader/syncs/progress/{digest}` and `PUT /koreader/syncs/progress` codecs. |
| `lib/BookOrbit/BookOrbitBulkProgress.{h,cpp}` | `POST /koreader/plugin/progress` bulk codec, 100 items per batch. |
| `lib/BookOrbit/ProgressResolution.{h,cpp}` | "Never degrade silently" policy: outbound completeness guard, inbound source choice, degraded-jump threshold. |
| `lib/BookOrbit/NativePosition.{h,cpp}` | **FOLLOW-ON.** Approach B `position` blob codec. |
| `lib/KOReaderSync/ChapterXPathResolver.cpp` | Modified: emits canonical xpointers via `buildCanonicalXPointer`; gains `findXPointerForXPointer`. |
| `lib/KOReaderSync/ProgressMapper.cpp` | Modified: all four literal `"/body/DocFragment["` sites and the `:1006` emitter delegate to `XPointer.h`. |
| `src/activities/reader/EpubReaderActivity.cpp` | Modified: degraded-jump notice on inbound progress. |
| `test/bookorbit_xpointer_corpus/generate_ground_truth.lua` | Drives the KOReader emulator's `CreDocument` to emit real xpointers per page. |
| `test/bookorbit_xpointer_corpus/generate_ground_truth.sh` | Runs the generator over every `test/epubs/*.epub` at both DOM versions and extracts the spine XHTML. |
| `test/bookorbit_xpointer_corpus/GroundTruthCorpus.h` | Header-only fixture loader, shared by four test targets; also holds the two verbatim sidecar strings. |
| `test/bookorbit_xpointer_corpus/fixtures/` | Committed ground truth: one CSV per EPUB per DOM version, plus the extracted spine XHTML. |
| `test/bookorbit_xpointer_corpus/` | Fixture guard tests — distinctness, row counts, DOM-version shape. Written first; must fail. |
| `test/bookorbit_xpointer/` | Grammar round-trip over the generated corpus. |
| `test/bookorbit_xpointer_dom/` | DOM-version boundary and synthetic boxing elements. |
| `test/bookorbit_xpointer_resolver/` | Ancestry resolution of every corpus xpointer against the real spine XHTML. |
| `test/bookorbit_xpointer_bridge/` | The fixed-size step adapter `ProgressMapper` consumes. |
| `test/bookorbit_progress/` | Progress GET/PUT codec fixtures. |
| `test/bookorbit_bulk_progress/` | Bulk codec fixtures and batch splitting. |
| `test/bookorbit_progress_resolution/` | Degrade policy. |
| `test/bookorbit_native_position/` | **FOLLOW-ON.** Approach B codec. |

---

### Task 1: Ground-truth generator and fixture corpus

There is a working KOReader emulator on this machine, and it exposes exactly the
API needed to emit genuine crengine xpointers for our own test EPUBs. That
removes any need for a synthesized corpus: the ground truth is generated by the
implementation we are trying to interoperate with.

**Verified facts — do not re-derive:**

```bash
cd /home/monish/repos/koreader/koreader-emulator-x86_64-pc-linux-gnu-debug/koreader
./luajit -e 'print(jit.version)'    # LuaJIT 2.1.1785763465
```

`reader.lua` and `luajit` are both valid symlinks that resolve.
`/home/monish/repos/koreader/frontend/document/credocument.lua` provides:

| Function | Line | Use |
|---|---|---|
| `CreDocument:getPageXPointer(page)` | 888 | the xpointer for a rendered page — our corpus source |
| `CreDocument:getXPointer()` | 884 | the current position |
| `CreDocument:gotoXPointer(xp)` | 879 | verification direction |
| `CreDocument:compareXPointers(xp1, xp2)` | 750 | orders two xpointers |
| `CreDocument:getNormalizedXPointer(xp)` | 1002 | normalizes under the requested DOM version |
| `CreDocument:getDomVersionWithNormalizedXPointers()` | 191 | **20200223** |
| `CreDocument:getLatestDomVersion()` | 195 | **20260812** |
| `CreDocument:getOldestDomVersion()` | 199 | **20171225** |

Three environment facts, each established by running the generator below:

- **`SDL_VIDEODRIVER=dummy` is required.** Without it `require("device")`
  segfaults on a headless machine. With it the generator runs to completion.
- **A fake `CanvasContext` device is required** — `canvascontext.lua:14-66`
  documents headless use and needs only `screen` plus a handful of predicates.
- **crengine reads the EPUB zip directly**, so the generator runs against
  `test/epubs/*.epub` as committed. Nothing needs unpacking for the corpus.

The spine XHTML *is* needed by the resolver test in Task 4, because the C++ side
has no zip reader in the native suite. The shell wrapper below extracts it with
`python3 -m zipfile`-style logic, naming each file by its DocFragment index —
`DocFragment[N]` is the Nth spine itemref, verified against
`test_tables.epub` (`DocFragment[2]` ↔ `EPUB/text/title_page.xhtml`).

**Files:**
- Create: `test/bookorbit_xpointer_corpus/generate_ground_truth.lua`
- Create: `test/bookorbit_xpointer_corpus/generate_ground_truth.sh`
- Create: `test/bookorbit_xpointer_corpus/GroundTruthCorpus.h`
- Create: `test/bookorbit_xpointer_corpus/CorpusFixtureTest.cpp`, `test/bookorbit_xpointer_corpus/CMakeLists.txt`
- Create (generated, committed): `test/bookorbit_xpointer_corpus/fixtures/*.csv`, `test/bookorbit_xpointer_corpus/fixtures/*.xhtml`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(bookorbit_xpointer_corpus)` beside the existing entries)

**Interfaces:**
- Consumes: nothing. This task deliberately has no dependency on `XPointer.h`, so
  the corpus exists and is guarded before any grammar is written.
- Produces:
  `struct corpus::Row { int page; std::string xpointer; }`;
  `struct corpus::Fixture { std::string epub; int domVersion; int domVersionWithNormalizedXPointers; std::vector<Row> rows; }`;
  `bool corpus::loadFixture(const std::string& path, Fixture& out)`;
  `std::vector<std::string> corpus::fixturePaths(int domVersion)`;
  `std::string corpus::spinePath(const std::string& epub, int docFragment)`;
  `struct corpus::SidecarEntry` and `corpus::kSidecarEntries` — the two verbatim real-sidecar strings, kept as an additional regression case.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_xpointer_corpus/GroundTruthCorpus.h`:

```cpp
#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// Ground truth for crengine xpointers, generated by driving the KOReader
// emulator (generate_ground_truth.sh) and committed as CSV so CI never needs
// the emulator. Header-only so several test targets can share it.
namespace corpus {

struct Row {
  int page = 0;
  std::string xpointer;
};

struct Fixture {
  std::string epub;
  int domVersion = 0;
  int domVersionWithNormalizedXPointers = 0;
  std::vector<Row> rows;
};

// crengine's two DOM versions of interest. Below kNormalizedDomVersion the
// emitter omits [1] indices; at or above it every step is indexed.
inline constexpr int kOldestDomVersion = 20171225;
inline constexpr int kNormalizedDomVersion = 20200223;
inline constexpr int kLatestDomVersion = 20260812;

// Every EPUB in test/epubs/, all of which the generator handles.
inline const std::vector<std::string>& epubNames() {
  static const std::vector<std::string> names = {
      "font-prewarm-benchmark",     "test_br_section_break",  "test_display_none",
      "test_emoji_ranges",          "test_force_paragraph_indents", "test_jpeg_images",
      "test_kerning_ligature",      "test_mixed_images",      "test_png_images",
      "test_reader_rendering_matrix", "test_supsub",          "test_synthetic_unicode_glyphs",
      "test_tables",
  };
  return names;
}

inline std::string fixtureDir() { return std::string(CORPUS_FIXTURE_DIR); }

inline std::string fixturePath(const std::string& epub, const int domVersion) {
  return fixtureDir() + "/" + epub + "_dom" + std::to_string(domVersion) + ".csv";
}

inline std::vector<std::string> fixturePaths(const int domVersion) {
  std::vector<std::string> paths;
  paths.reserve(epubNames().size());
  for (const auto& name : epubNames()) {
    paths.push_back(fixturePath(name, domVersion));
  }
  return paths;
}

// The spine item behind DocFragment[N], extracted from the EPUB zip by the
// generator wrapper. docFragment is 1-based, as in the xpointer.
inline std::string spinePath(const std::string& epub, const int docFragment) {
  return fixtureDir() + "/" + epub + "_frag" + std::to_string(docFragment) + ".xhtml";
}

inline std::string readFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

inline bool loadFixture(const std::string& path, Fixture& out) {
  out = Fixture{};
  std::ifstream stream(path);
  if (!stream) return false;

  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;

    if (line[0] == '#') {
      const auto eq = line.find('=');
      if (eq == std::string::npos) continue;
      const std::string key = line.substr(2, eq - 2);
      const std::string value = line.substr(eq + 1);
      if (key == "epub") out.epub = value;
      else if (key == "dom_version") out.domVersion = std::atoi(value.c_str());
      else if (key == "dom_version_with_normalized_xpointers")
        out.domVersionWithNormalizedXPointers = std::atoi(value.c_str());
      continue;
    }
    if (line.rfind("page,", 0) == 0) continue;  // column header

    const auto comma = line.find(',');
    if (comma == std::string::npos) return false;
    Row row;
    row.page = std::atoi(line.substr(0, comma).c_str());
    row.xpointer = line.substr(comma + 1);
    if (row.page <= 0 || row.xpointer.empty()) return false;
    out.rows.push_back(row);
  }
  return !out.rows.empty();
}

// Verbatim from real KOReader sidecars on the development machine, kept
// alongside the generated corpus as an independent regression case:
//   grep -h last_xpointer /home/monish/repos/imprint-dev-books/*.sdr/metadata.epub.lua
struct SidecarEntry {
  const char* raw;
  const char* canonical;
  const char* provenance;
};

inline constexpr SidecarEntry kSidecarEntries[] = {
    {"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0",
     "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0",
     "Sidecar: Japanese Gothic - Kylie Lee Baker (a70862e0e4138fad5f93ca2bc467bb03)"},
    {"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0",
     "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0",
     "Sidecar: We Solve Murders - Richard Osman (d18e399f0f79f24d68a8f70b76d59914)"},
};

inline constexpr size_t kSidecarEntryCount = sizeof(kSidecarEntries) / sizeof(kSidecarEntries[0]);

}  // namespace corpus
```

Create `test/bookorbit_xpointer_corpus/CorpusFixtureTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"

using corpus::Fixture;
using corpus::kLatestDomVersion;
using corpus::kNormalizedDomVersion;
using corpus::kOldestDomVersion;
using corpus::loadFixture;

namespace {

// Total rows the generator produces across all 13 EPUBs at one DOM version.
// Corpus size depends on the fixture EPUBs; record the real figure on first run.
constexpr size_t kRowsPerDomVersion = 202;

std::vector<Fixture> loadAll(const int domVersion) {
  std::vector<Fixture> fixtures;
  for (const auto& path : corpus::fixturePaths(domVersion)) {
    Fixture fixture;
    EXPECT_TRUE(loadFixture(path, fixture)) << path;
    fixtures.push_back(fixture);
  }
  return fixtures;
}

}  // namespace

TEST(CorpusFixture, EveryEpubHasAFixtureAtBothDomVersions) {
  for (const auto& name : corpus::epubNames()) {
    Fixture oldest;
    Fixture latest;
    EXPECT_TRUE(loadFixture(corpus::fixturePath(name, kOldestDomVersion), oldest)) << name;
    EXPECT_TRUE(loadFixture(corpus::fixturePath(name, kLatestDomVersion), latest)) << name;
  }
}

TEST(CorpusFixture, HeadersRecordTheDomVersion) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    EXPECT_EQ(fixture.domVersion, kLatestDomVersion) << fixture.epub;
    EXPECT_EQ(fixture.domVersionWithNormalizedXPointers, kNormalizedDomVersion) << fixture.epub;
  }
}

TEST(CorpusFixture, TotalRowCountMatchesTheGenerator) {
  size_t rows = 0;
  for (const auto& fixture : loadAll(kLatestDomVersion)) rows += fixture.rows.size();
  EXPECT_EQ(rows, kRowsPerDomVersion);
}

// The guard that matters. The original hand-written corpus degenerated to a
// single cover-page xpointer repeated twice; if the generator ever regresses to
// that, this fails loudly rather than leaving a corpus that proves nothing.
TEST(CorpusFixture, XpointersAreOverwhelminglyDistinct) {
  std::set<std::string> distinct;
  size_t rows = 0;
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      distinct.insert(row.xpointer);
      rows++;
    }
  }
  ASSERT_GT(rows, 150u);
  EXPECT_GT(distinct.size(), 150u);
  // Every page of every fixture book lands somewhere different.
  EXPECT_EQ(distinct.size(), rows);
}

TEST(CorpusFixture, NoSingleFixtureIsAllOneXpointer) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    if (fixture.rows.size() < 3) continue;
    std::set<std::string> distinct;
    for (const auto& row : fixture.rows) distinct.insert(row.xpointer);
    EXPECT_GT(distinct.size(), 1u) << fixture.epub;
  }
}

TEST(CorpusFixture, PagesAreOneBasedAndAscending) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    int previous = 0;
    for (const auto& row : fixture.rows) {
      EXPECT_GT(row.page, previous) << fixture.epub;
      previous = row.page;
    }
    EXPECT_EQ(fixture.rows.front().page, 1) << fixture.epub;
  }
}

// The two DOM versions differ in shape, which is the whole reason both are
// generated: the oldest omits [1] indices, the latest never does.
TEST(CorpusFixture, OldestDomVersionCarriesUnindexedSteps) {
  size_t unindexed = 0;
  for (const auto& fixture : loadAll(kOldestDomVersion)) {
    for (const auto& row : fixture.rows) {
      if (row.xpointer.rfind("/body/DocFragment[", 0) == 0) unindexed++;
    }
  }
  EXPECT_GT(unindexed, 100u);
}

TEST(CorpusFixture, LatestDomVersionIsFullyIndexed) {
  for (const auto& fixture : loadAll(kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_EQ(row.xpointer.rfind("/body[1]/DocFragment[", 0), 0u) << row.xpointer;
    }
  }
}

TEST(CorpusFixture, EverySpineFileReferencedByTheCorpusExists) {
  for (const auto& name : corpus::epubNames()) {
    Fixture fixture;
    ASSERT_TRUE(loadFixture(corpus::fixturePath(name, kLatestDomVersion), fixture));
    for (const auto& row : fixture.rows) {
      const auto open = row.xpointer.find("DocFragment[");
      ASSERT_NE(open, std::string::npos) << row.xpointer;
      const int frag = std::atoi(row.xpointer.c_str() + open + 12);
      ASSERT_GT(frag, 0) << row.xpointer;
      EXPECT_FALSE(corpus::readFile(corpus::spinePath(name, frag)).empty())
          << corpus::spinePath(name, frag);
    }
  }
}

TEST(CorpusFixture, SidecarRegressionEntriesArePresent) {
  ASSERT_EQ(corpus::kSidecarEntryCount, 2u);
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_STREQ(corpus::kSidecarEntries[i].raw, "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0");
  }
}

TEST(CorpusFixture, MissingFixtureIsReportedNotCrashed) {
  Fixture fixture;
  EXPECT_FALSE(loadFixture(corpus::fixtureDir() + "/does_not_exist_dom1.csv", fixture));
}
```

Create `test/bookorbit_xpointer_corpus/CMakeLists.txt`:

```cmake
add_executable(BookOrbitCorpusFixtureTest
  CorpusFixtureTest.cpp
)

target_include_directories(BookOrbitCorpusFixtureTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(BookOrbitCorpusFixtureTest PRIVATE
  CORPUS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(BookOrbitCorpusFixtureTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCorpusFixtureTest)
```

Add to `test/CMakeLists.txt`, after the last existing `add_subdirectory(...)` line:

```cmake
add_subdirectory(bookorbit_xpointer_corpus)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitCorpusFixture --output-on-failure
```

Expected: the target builds, and every test FAILS — the `fixtures/` directory
does not exist yet, so `loadFixture` returns false on the first path and
`CorpusFixture.EveryEpubHasAFixtureAtBothDomVersions` reports
`.../fixtures/font-prewarm-benchmark_dom20171225.csv`.

- [ ] **Step 3: Write minimal implementation**

Create `test/bookorbit_xpointer_corpus/generate_ground_truth.lua`:

```lua
-- Emits real crengine xpointers for one EPUB by driving KOReader's CreDocument.
--
-- Run through the KOReader emulator's luajit, from the emulator directory:
--   SDL_VIDEODRIVER=dummy ./luajit <this file> <epub> <out.csv> [dom_version]
--
-- SDL_VIDEODRIVER=dummy is mandatory on a headless machine: require("device")
-- segfaults without a video driver. The fake device below is the headless
-- CanvasContext contract documented at frontend/document/canvascontext.lua:14-66.
io.stdout:setvbuf("line")
os.setlocale("C", "numeric")

require("setupkoenv")

local DataStorage = require("datastorage")
G_defaults = require("luadefaults"):open()
G_reader_settings = require("luasettings"):open(DataStorage:getDataDir() .. "/settings.reader.lua")

local screen = {
    getWidth = function() return 600 end,
    getHeight = function() return 800 end,
    getDPI = function() return 160 end,
    getSize = function() return {x = 0, y = 0, w = 600, h = 800} end,
    scaleBySize = function(_, n) return n end,
    isColorEnabled = function() return false end,
    fb_bpp = 8,
}
local device = {
    screen = screen,
    hasBGRFrameBuffer = function() return false end,
    hasEinkScreen = function() return true end,
    isAndroid = function() return false end,
    isDesktop = function() return true end,
    isEmulator = function() return true end,
    isKindle = function() return false end,
    isPocketBook = function() return false end,
    hasSystemFonts = function() return false end,
    canHWDither = function() return false end,
}
require("document/canvascontext"):init(device)

local DocumentRegistry = require("document/documentregistry")

local epub_path = assert(arg[1], "usage: generate_ground_truth.lua <epub> <out.csv> [dom_version]")
local out_path = assert(arg[2], "usage: generate_ground_truth.lua <epub> <out.csv> [dom_version]")

local doc = assert(DocumentRegistry:openDocument(epub_path), "cannot open " .. epub_path)
doc:loadDocument()

-- The DOM version decides xpointer shape, so it is an explicit input and is
-- recorded in the output header.
local dom_version = tonumber(arg[3]) or doc:getLatestDomVersion()
doc:requestDomVersion(dom_version)

doc:setViewMode("page")
doc:setViewDimen({w = 600, h = 800})
doc:render()

local out = assert(io.open(out_path, "w"))
out:write("# epub=", epub_path:gsub(".*/", ""), "\n")
out:write("# dom_version=", tostring(dom_version), "\n")
out:write("# dom_version_with_normalized_xpointers=", tostring(doc:getDomVersionWithNormalizedXPointers()), "\n")
out:write("# oldest_dom_version=", tostring(doc:getOldestDomVersion()), "\n")
out:write("# latest_dom_version=", tostring(doc:getLatestDomVersion()), "\n")
out:write("page,xpointer\n")

local pages = doc:getPageCount()
for page = 1, pages do
    local xp = doc:getPageXPointer(page)
    if xp and xp ~= "" then
        -- The CSV has exactly two columns; a comma inside an xpointer would
        -- break the loader silently, so fail loudly instead.
        assert(not xp:find(","), "xpointer contains a comma: " .. xp)
        out:write(page, ",", xp, "\n")
    end
end
out:close()
doc:close()

io.write("wrote ", out_path, " (", tostring(pages), " pages, dom ", tostring(dom_version), ")\n")
```

Create `test/bookorbit_xpointer_corpus/generate_ground_truth.sh`:

```bash
#!/usr/bin/env bash
#
# Regenerates the crengine xpointer ground-truth corpus and the spine XHTML the
# resolver test reads. Needs the KOReader emulator; CI does not, because the
# output is committed.
#
#   test/bookorbit_xpointer_corpus/generate_ground_truth.sh
#
# Override the emulator location with KOREADER_EMULATOR_DIR.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
GENERATOR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/generate_ground_truth.lua"
EMULATOR_DIR="${KOREADER_EMULATOR_DIR:-/home/monish/repos/koreader/koreader-emulator-x86_64-pc-linux-gnu-debug/koreader}"

if [ ! -x "${EMULATOR_DIR}/luajit" ]; then
  echo "KOReader emulator not found at ${EMULATOR_DIR}" >&2
  echo "Set KOREADER_EMULATOR_DIR, or leave the committed fixtures as they are." >&2
  exit 1
fi

# Both DOM versions: the oldest predates xpointer normalization and emits
# unindexed steps, the latest emits fully-indexed ones. Our parser must accept
# both, so both are corpus material.
DOM_VERSIONS=(20171225 20260812)

mkdir -p "${FIXTURE_DIR}"
rm -f "${FIXTURE_DIR}"/*.csv "${FIXTURE_DIR}"/*.xhtml

for epub in "${REPO_ROOT}"/test/epubs/*.epub; do
  name="$(basename "${epub}" .epub)"

  for dom in "${DOM_VERSIONS[@]}"; do
    # crengine reads the zip directly; nothing is unpacked for the corpus.
    ( cd "${EMULATOR_DIR}" && SDL_VIDEODRIVER=dummy ./luajit "${GENERATOR}" \
        "${epub}" "${FIXTURE_DIR}/${name}_dom${dom}.csv" "${dom}" ) >/dev/null
    echo "generated ${name}_dom${dom}.csv"
  done

  # The C++ resolver test has no zip reader, so each spine item is extracted
  # under its DocFragment index: DocFragment[N] is the Nth spine itemref.
  python3 - "${epub}" "${FIXTURE_DIR}/${name}" <<'PYEOF'
import os
import re
import sys
import zipfile

epub_path, prefix = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(epub_path) as archive:
    opf = next(n for n in archive.namelist() if n.endswith(".opf"))
    manifest = archive.read(opf).decode("utf-8")

    hrefs = {}
    for item in re.finditer(r"<item\b[^>]*>", manifest):
        tag = item.group(0)
        item_id = re.search(r'id="([^"]+)"', tag)
        href = re.search(r'href="([^"]+)"', tag)
        if item_id and href:
            hrefs[item_id.group(1)] = href.group(1)

    base = os.path.dirname(opf)
    for index, idref in enumerate(re.findall(r'<itemref[^>]*idref="([^"]+)"', manifest), start=1):
        entry = os.path.normpath(os.path.join(base, hrefs[idref])).replace("\\", "/")
        with open("%s_frag%d.xhtml" % (prefix, index), "wb") as out:
            out.write(archive.read(entry))
PYEOF
  echo "extracted spine of ${name}"
done

echo "corpus rows: $(cat "${FIXTURE_DIR}"/*.csv | grep -c '^[0-9]')"
```

Make it executable and run it:

```bash
chmod +x test/bookorbit_xpointer_corpus/generate_ground_truth.sh
./test/bookorbit_xpointer_corpus/generate_ground_truth.sh
```

Expected output ends with `corpus rows: <N>` — 13 EPUBs × 2 DOM versions, N/2
rows per DOM version. Verified output for `test_tables.epub` at the latest DOM
version, `fixtures/test_tables_dom20260812.csv`:

```
# epub=test_tables.epub
# dom_version=20260812
# dom_version_with_normalized_xpointers=20200223
# oldest_dom_version=20171225
# latest_dom_version=20260812
page,xpointer
1,/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0
2,/body[1]/DocFragment[2]/body[1]/section[1]/h1[1]/text()[1].0
3,/body[1]/DocFragment[3]/body[1]/section[1]/h1[1]/text()[1].0
4,/body[1]/DocFragment[4]/body[1]/section[1]/h1[1]/text()[1].0
5,/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0
```

and the same book at the oldest DOM version,
`fixtures/test_tables_dom20171225.csv`, line 5 — note the missing indices and
the extra `tabularBox[1]` step crengine inserts:

```
5,/body/DocFragment[4]/body/section/table/tabularBox/tbody/tr/td/text().0
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitCorpusFixture --output-on-failure
```

Expected: 11 tests PASS, with `CorpusFixture.XpointersAreOverwhelminglyDistinct`
reporting distinct xpointers equal to total rows (no duplicates).

- [ ] **Step 5: Commit**

```bash
git add test/bookorbit_xpointer_corpus test/CMakeLists.txt
git commit -m "test: generate crengine xpointer ground truth from KOReader"
```

---

### Task 2: The xpointer grammar

This is the defect fix. `ProgressMapper.cpp:62,134,192,257` all search for the
literal `"/body/DocFragment["`, and `ProgressMapper.cpp:1006` emits
`"/body/DocFragment[N]/body"`. The corpus from Task 1 settles what real
xpointers look like: at the latest DOM version **every one of the 202 rows**
begins `/body[1]/DocFragment[`, so every one of those `find()` calls misses.

**Files:**
- Create: `lib/BookOrbit/XPointer.h`, `lib/BookOrbit/XPointer.cpp`
- Create: `test/bookorbit_xpointer/CMakeLists.txt`, `test/bookorbit_xpointer/XPointerTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `corpus::loadFixture`, `corpus::fixturePaths`, `corpus::kSidecarEntries` (Task 1).
- Produces:
  `struct bookorbit::XPointerStep { std::string name; int index; }`;
  `struct bookorbit::XPointer { int docFragment; std::vector<XPointerStep> steps; int textNodeIndex; long charOffset; bool valid; }`;
  `bool bookorbit::parseXPointer(std::string_view raw, XPointer& out)`;
  `std::string bookorbit::emitXPointer(const XPointer& p)`;
  `std::string bookorbit::normalizeXPointer(std::string_view raw)`;
  `bool bookorbit::sameLocation(std::string_view a, std::string_view b)`;
  `int bookorbit::xpointerDocFragmentIndex(std::string_view raw)`;
  `bool bookorbit::rewriteDocFragmentIndex(std::string& xpointer, int oneBasedIndex)`;
  `bool bookorbit::isChapterStartXPointer(std::string_view raw)`;
  `std::string bookorbit::buildCanonicalXPointer(int spineIndex, const std::vector<XPointerStep>& steps, int textNodeIndex, long charOffset)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_xpointer/XPointerTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"

using bookorbit::buildCanonicalXPointer;
using bookorbit::emitXPointer;
using bookorbit::isChapterStartXPointer;
using bookorbit::normalizeXPointer;
using bookorbit::parseXPointer;
using bookorbit::rewriteDocFragmentIndex;
using bookorbit::sameLocation;
using bookorbit::XPointer;
using bookorbit::XPointerStep;
using bookorbit::xpointerDocFragmentIndex;

namespace {

// Verbatim reproduction of the matching rule at ProgressMapper.cpp:62 as it
// stands before this task. Kept in the test forever as the defect record.
bool legacyLiteralMatch(const std::string& xpath) {
  return xpath.find("/body/DocFragment[") != std::string::npos;
}

std::vector<corpus::Fixture> loadAll(const int domVersion) {
  std::vector<corpus::Fixture> fixtures;
  for (const auto& path : corpus::fixturePaths(domVersion)) {
    corpus::Fixture fixture;
    EXPECT_TRUE(corpus::loadFixture(path, fixture)) << path;
    fixtures.push_back(fixture);
  }
  return fixtures;
}

}  // namespace

// ---- The defect, proven against generated ground truth ---------------------

// Every xpointer a current KOReader writes misses the literal search. This is
// the whole bug, measured rather than argued.
TEST(XPointerDefect, LegacyLiteralMatchMissesEveryModernXPointer) {
  size_t rows = 0;
  size_t missed = 0;
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      rows++;
      if (!legacyLiteralMatch(row.xpointer)) missed++;
    }
  }
  ASSERT_GT(rows, 150u);
  EXPECT_EQ(missed, rows);
}

TEST(XPointerDefect, LegacyLiteralMatchMissesBothRealSidecarXPointers) {
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_FALSE(legacyLiteralMatch(corpus::kSidecarEntries[i].raw)) << corpus::kSidecarEntries[i].provenance;
  }
}

TEST(XPointerDefect, TheGrammarMatchesWhereTheLiteralDidNot) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_GT(xpointerDocFragmentIndex(row.xpointer), 0) << row.xpointer;
    }
  }
}

// ---- Corpus round trip, both DOM versions ----------------------------------

TEST(XPointerCorpus, EveryGeneratedXPointerParses) {
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& fixture : loadAll(dom)) {
      for (const auto& row : fixture.rows) {
        XPointer parsed;
        EXPECT_TRUE(parseXPointer(row.xpointer, parsed)) << row.xpointer;
        EXPECT_TRUE(parsed.valid) << row.xpointer;
      }
    }
  }
}

// The canonical form is what the device emits, so it must be stable under
// re-normalization or a pushed position would drift on every sync.
TEST(XPointerCorpus, CanonicalFormIsAFixedPoint) {
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& fixture : loadAll(dom)) {
      for (const auto& row : fixture.rows) {
        const std::string once = normalizeXPointer(row.xpointer);
        ASSERT_FALSE(once.empty()) << row.xpointer;
        EXPECT_EQ(normalizeXPointer(once), once) << row.xpointer;
      }
    }
  }
}

// crengine already emits the canonical form at the latest DOM version, so
// normalization must be the identity there.
TEST(XPointerCorpus, LatestDomVersionNormalizesToItself) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      EXPECT_EQ(normalizeXPointer(row.xpointer), row.xpointer) << row.xpointer;
    }
  }
}

TEST(XPointerCorpus, EmitIsTheInverseOfParse) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    for (const auto& row : fixture.rows) {
      XPointer parsed;
      ASSERT_TRUE(parseXPointer(row.xpointer, parsed)) << row.xpointer;
      EXPECT_EQ(emitXPointer(parsed), row.xpointer) << row.xpointer;
    }
  }
}

TEST(XPointerCorpus, NormalizationNeverCollapsesDistinctPositions) {
  for (const auto& fixture : loadAll(corpus::kLatestDomVersion)) {
    std::set<std::string> normalized;
    for (const auto& row : fixture.rows) normalized.insert(normalizeXPointer(row.xpointer));
    EXPECT_EQ(normalized.size(), fixture.rows.size()) << fixture.epub;
  }
}

TEST(XPointerCorpus, SidecarEntriesNormalizeToTheirCanonicalForm) {
  for (size_t i = 0; i < corpus::kSidecarEntryCount; i++) {
    EXPECT_EQ(normalizeXPointer(corpus::kSidecarEntries[i].raw), corpus::kSidecarEntries[i].canonical)
        << corpus::kSidecarEntries[i].provenance;
  }
}

// ---- Grammar details -------------------------------------------------------

TEST(XPointer, ParsesIndexedStepsAndOffset) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0", p));
  EXPECT_EQ(p.docFragment, 1);
  ASSERT_EQ(p.steps.size(), 2u);
  EXPECT_EQ(p.steps[0].name, "div");
  EXPECT_EQ(p.steps[0].index, 1);
  EXPECT_EQ(p.steps[1].name, "svg");
  EXPECT_EQ(p.steps[1].index, 1);
  EXPECT_EQ(p.textNodeIndex, 0);
  EXPECT_EQ(p.charOffset, 0);
}

// The oldest DOM version's shape, taken verbatim from
// fixtures/test_reader_rendering_matrix_dom20171225.csv line 1.
TEST(XPointer, UnindexedStepsDefaultToOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[1]/body/h1/text().0", p));
  EXPECT_EQ(p.docFragment, 1);
  ASSERT_EQ(p.steps.size(), 1u);
  EXPECT_EQ(p.steps[0].name, "h1");
  EXPECT_EQ(p.steps[0].index, 1);
  EXPECT_EQ(p.textNodeIndex, 1);
  EXPECT_EQ(p.charOffset, 0);
}

TEST(XPointer, TextNodeWithoutIndexIsNodeOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[2]/body/p[8]/text().85", p));
  EXPECT_EQ(p.textNodeIndex, 1);
  EXPECT_EQ(p.charOffset, 85);
}

TEST(XPointer, ExplicitTextNodeIndexSurvives) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[3]/body[1]/ul[1]/li[4]/text()[2].51", p));
  EXPECT_EQ(p.textNodeIndex, 2);
  EXPECT_EQ(p.charOffset, 51);
}

TEST(XPointer, MissingOffsetIsNegativeOne) {
  XPointer p;
  ASSERT_TRUE(parseXPointer("/body/DocFragment[1]/body/p[5]", p));
  EXPECT_EQ(p.charOffset, -1);
}

TEST(XPointer, RejectsGarbage) {
  XPointer p;
  EXPECT_FALSE(parseXPointer("", p));
  EXPECT_FALSE(parseXPointer("not-an-xpointer", p));
  EXPECT_FALSE(parseXPointer("/html/body/p[1]", p));
  EXPECT_EQ(normalizeXPointer("/html/body/p[1]"), "");
}

TEST(XPointer, IndexedAndUnindexedFormsAreTheSameLocation) {
  EXPECT_TRUE(sameLocation("/body/DocFragment[1]/body/p[5]", "/body[1]/DocFragment[1]/body[1]/p[5]"));
  EXPECT_FALSE(sameLocation("/body/DocFragment[1]/body/p[5]", "/body[1]/DocFragment[1]/body[1]/p[6]"));
  EXPECT_FALSE(sameLocation("garbage", "garbage"));
}

TEST(XPointer, ChapterStartRecognisedInBothForms) {
  EXPECT_TRUE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1]"));
  EXPECT_TRUE(isChapterStartXPointer("/body/DocFragment[12]"));
  EXPECT_TRUE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1].0"));
  EXPECT_FALSE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1]/p[2]"));
  EXPECT_FALSE(isChapterStartXPointer("/body[1]/DocFragment[12]/body[1].17"));
}

TEST(XPointer, RewritesDocFragmentIndexInBothForms) {
  std::string indexed = "/body[1]/DocFragment[1]/body[1]/p[5]";
  ASSERT_TRUE(rewriteDocFragmentIndex(indexed, 7));
  EXPECT_EQ(indexed, "/body[1]/DocFragment[7]/body[1]/p[5]");

  std::string unindexed = "/body/DocFragment[1]/body/p[5]";
  ASSERT_TRUE(rewriteDocFragmentIndex(unindexed, 7));
  EXPECT_EQ(unindexed, "/body[1]/DocFragment[7]/body[1]/p[5]");

  std::string bad = "nope";
  EXPECT_FALSE(rewriteDocFragmentIndex(bad, 7));
  EXPECT_EQ(bad, "nope");
}

TEST(XPointer, BuildCanonicalMatchesTheEmitter) {
  const std::vector<XPointerStep> steps = {{"section", 1}, {"table", 1}, {"tbody", 1}, {"tr", 1}, {"td", 1}};
  EXPECT_EQ(buildCanonicalXPointer(3, steps, 1, 0),
            "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
  EXPECT_EQ(buildCanonicalXPointer(3, steps, 0, -1),
            "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]");
  EXPECT_EQ(buildCanonicalXPointer(0, {}, 0, -1), "/body[1]/DocFragment[1]/body[1]");
}
```

Create `test/bookorbit_xpointer/CMakeLists.txt`:

```cmake
add_executable(BookOrbitXPointerTest
  XPointerTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerTest)
```

Add `add_subdirectory(bookorbit_xpointer)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/XPointer.cpp`, then `lib/BookOrbit/XPointer.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/XPointer.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One element step below the DocFragment's <body>, always carrying an explicit
// 1-based same-name sibling index.
struct XPointerStep {
  std::string name;
  int index = 1;
};

// A parsed crengine xpointer.
//
// crengine emits two shapes for the same location, and which one you get
// depends on the document's DOM version (see Task 3):
//   >= 20200223: /body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0
//   <  20200223: /body/DocFragment[1]/body/div/svg.0
// Both arrive over the wire from real installs. Parsing tolerates both;
// emitXPointer() always produces the indexed form.
struct XPointer {
  int docFragment = 0;              // 1-based spine fragment; 0 means absent
  std::vector<XPointerStep> steps;  // element ancestry below the fragment body
  int textNodeIndex = 0;            // N from /text()[N]; 0 when no text() step
  long charOffset = -1;             // the trailing .N; -1 when absent
  bool valid = false;
};

// Tolerant parse. Returns false (and leaves out.valid false) when raw is not a
// crengine xpointer at all.
bool parseXPointer(std::string_view raw, XPointer& out);

// Canonical fully-indexed emission.
std::string emitXPointer(const XPointer& p);

// parse + emit. Returns "" when raw does not parse.
std::string normalizeXPointer(std::string_view raw);

// True when both parse and normalize to the same canonical string.
bool sameLocation(std::string_view a, std::string_view b);

// 1-based DocFragment index, or 0 when raw is not an xpointer. Replaces the
// literal "/body/DocFragment[" searches in ProgressMapper.cpp.
int xpointerDocFragmentIndex(std::string_view raw);

// Rewrites the DocFragment index in place, normalizing the string as a side
// effect. Returns false and leaves the string untouched when it does not parse.
bool rewriteDocFragmentIndex(std::string& xpointer, int oneBasedIndex);

// A position at the very start of a chapter: no element steps below the
// fragment body and no non-zero character offset.
bool isChapterStartXPointer(std::string_view raw);

// Builds the canonical string directly. spineIndex is 0-based; the emitted
// DocFragment index is spineIndex + 1.
std::string buildCanonicalXPointer(int spineIndex, const std::vector<XPointerStep>& steps, int textNodeIndex,
                                   long charOffset);

}  // namespace bookorbit
```

Create `lib/BookOrbit/XPointer.cpp`:

```cpp
#include "XPointer.h"

#include <cstdlib>

namespace bookorbit {
namespace {

constexpr char kTextNode[] = "text()";

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool allDigits(const std::string_view value) {
  if (value.empty()) return false;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

long toLong(const std::string_view digits) {
  long value = 0;
  for (const char c : digits) {
    value = value * 10 + (c - '0');
  }
  return value;
}

// Splits "name[3]" into ("name", 3). A bare "name" yields index 1, which is
// exactly how crengine reads a pre-normalization step.
bool splitSegment(const std::string_view segment, std::string_view& name, int& index) {
  if (segment.empty()) return false;
  const auto open = segment.find('[');
  if (open == std::string_view::npos) {
    name = segment;
    index = 1;
    return true;
  }
  if (segment.back() != ']') return false;
  const std::string_view digits = segment.substr(open + 1, segment.size() - open - 2);
  if (!allDigits(digits)) return false;
  name = segment.substr(0, open);
  index = static_cast<int>(toLong(digits));
  return !name.empty() && index > 0;
}

}  // namespace

bool parseXPointer(const std::string_view raw, XPointer& out) {
  out = XPointer{};

  std::string_view text = trim(raw);
  if (text.size() < 2 || text.front() != '/') return false;

  // Trailing ".N" character offset. Element names never contain '.', so the
  // last dot followed only by digits is unambiguously the offset.
  const auto dot = text.rfind('.');
  if (dot != std::string_view::npos && allDigits(text.substr(dot + 1))) {
    out.charOffset = toLong(text.substr(dot + 1));
    text = text.substr(0, dot);
  }

  std::vector<std::string_view> segments;
  size_t pos = 0;
  while (pos < text.size()) {
    if (text[pos] != '/') return false;
    const auto next = text.find('/', pos + 1);
    const std::string_view segment =
        (next == std::string_view::npos) ? text.substr(pos + 1) : text.substr(pos + 1, next - pos - 1);
    if (segment.empty()) return false;
    segments.push_back(segment);
    if (next == std::string_view::npos) break;
    pos = next;
  }

  if (segments.size() < 2) return false;

  std::string_view name;
  int index = 1;
  if (!splitSegment(segments[0], name, index) || name != "body") return false;
  if (!splitSegment(segments[1], name, index) || name != "DocFragment") return false;
  out.docFragment = index;

  size_t first = 2;
  if (segments.size() > 2) {
    if (!splitSegment(segments[2], name, index)) return false;
    // The fragment's own <body> is optional in the pre-normalization form.
    if (name == "body") first = 3;
  }

  for (size_t i = first; i < segments.size(); i++) {
    if (segments[i].compare(0, sizeof(kTextNode) - 1, kTextNode) == 0) {
      const std::string_view tail = segments[i].substr(sizeof(kTextNode) - 1);
      if (tail.empty()) {
        out.textNodeIndex = 1;
      } else if (tail.size() >= 3 && tail.front() == '[' && tail.back() == ']' &&
                 allDigits(tail.substr(1, tail.size() - 2))) {
        out.textNodeIndex = static_cast<int>(toLong(tail.substr(1, tail.size() - 2)));
      } else {
        return false;
      }
      if (i + 1 != segments.size()) return false;  // text() is always terminal
      break;
    }
    if (!splitSegment(segments[i], name, index)) return false;
    out.steps.push_back({std::string(name), index});
  }

  out.valid = out.docFragment > 0;
  return out.valid;
}

std::string emitXPointer(const XPointer& p) {
  if (!p.valid || p.docFragment <= 0) return {};

  std::string result = "/body[1]/DocFragment[";
  result += std::to_string(p.docFragment);
  result += "]/body[1]";
  for (const auto& step : p.steps) {
    result += '/';
    result += step.name;
    result += '[';
    result += std::to_string(step.index > 0 ? step.index : 1);
    result += ']';
  }
  if (p.textNodeIndex > 0) {
    result += "/text()[";
    result += std::to_string(p.textNodeIndex);
    result += ']';
  }
  if (p.charOffset >= 0) {
    result += '.';
    result += std::to_string(p.charOffset);
  }
  return result;
}

std::string normalizeXPointer(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return {};
  return emitXPointer(parsed);
}

bool sameLocation(const std::string_view a, const std::string_view b) {
  const std::string left = normalizeXPointer(a);
  if (left.empty()) return false;
  return left == normalizeXPointer(b);
}

int xpointerDocFragmentIndex(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return 0;
  return parsed.docFragment;
}

bool rewriteDocFragmentIndex(std::string& xpointer, const int oneBasedIndex) {
  XPointer parsed;
  if (oneBasedIndex <= 0 || !parseXPointer(xpointer, parsed)) return false;
  parsed.docFragment = oneBasedIndex;
  xpointer = emitXPointer(parsed);
  return true;
}

bool isChapterStartXPointer(const std::string_view raw) {
  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return false;
  return parsed.steps.empty() && parsed.textNodeIndex == 0 && parsed.charOffset <= 0;
}

std::string buildCanonicalXPointer(const int spineIndex, const std::vector<XPointerStep>& steps,
                                   const int textNodeIndex, const long charOffset) {
  XPointer p;
  p.docFragment = spineIndex + 1;
  p.steps = steps;
  p.textNodeIndex = textNodeIndex;
  p.charOffset = charOffset;
  p.valid = p.docFragment > 0;
  return emitXPointer(p);
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitXPointer --output-on-failure
```

Expected: 19 tests PASS, including
`XPointerDefect.LegacyLiteralMatchMissesEveryModernXPointer` reporting 202
misses out of 202 rows.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/XPointer.h lib/BookOrbit/XPointer.cpp test/bookorbit_xpointer test/CMakeLists.txt
git commit -m "fix: parse and normalize indexed crengine xpointers"
```

---

### Task 3: DOM versions and crengine's synthetic boxing elements

Generating the corpus at both DOM versions surfaced a compatibility dimension
the design did not account for, and it must be handled before the resolver is
written.

**Fact 1 — xpointer shape is a function of the DOM version.**
`getDomVersionWithNormalizedXPointers()` is **20200223**. Below it crengine
omits `[1]` indices; at or above it every step is indexed. Both forms are in the
corpus, and Task 2's parser already accepts both.

**Fact 2 — crengine inserts synthetic elements into the DOM that do not exist
in the source XHTML,** and they appear as real steps in an xpointer. The
authoritative set is the boxing range at
`base/thirdparty/kpvcrlib/crengine/crengine/include/fb2def.h:40-64` —
`EL_BOXING_START = el_autoBoxing` through `EL_BOXING_END = el_inlineBox`:
`autoBoxing`, `tabularBox`, `rubyBox`, `mathBox`, `floatBox`, `inlineBox`; plus
`pseudoElem`, declared immediately after as a synthetic non-boxing element.

Expected to be a small minority of rows. **Record the real count from your first `generate_ground_truth.sh` run and pin it here.**
Real examples, the same page under the two DOM versions:

```
20171225: /body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tabularBox[1]/tbody[1]/tr[1]/td[1]/text()[1].0
20260812: /body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0

20171225: /body[1]/DocFragment[4]/body[1]/autoBoxing[2]/img[1].0
20260812: /body[1]/DocFragment[4]/body[1]/img[2].0
```

The resolver streams the **source** XHTML, where no such element exists, so
synthetic steps must be dropped before matching. The second example shows the
limit of that: dropping `autoBoxing[2]` leaves `img[1]`, while the same position
under the current DOM version is `img[2]`. A stripped legacy xpointer can
therefore land on the wrong same-name sibling — which is exactly why
**xpointers are only comparable within a DOM version.** The device emits and
consumes the normalized (≥ 20200223) form; legacy input is accepted on a
best-effort basis and the ambiguity is recorded rather than hidden.

**Files:**
- Create: `test/bookorbit_xpointer_dom/CMakeLists.txt`, `test/bookorbit_xpointer_dom/XPointerDomTest.cpp`
- Modify: `lib/BookOrbit/XPointer.h`, `lib/BookOrbit/XPointer.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::XPointer`, `parseXPointer` (Task 2); `corpus::loadFixture` (Task 1).
- Produces:
  `inline constexpr int bookorbit::kNormalizedXPointerDomVersion = 20200223`;
  `bool bookorbit::isSyntheticCrengineElement(std::string_view name)`;
  `int bookorbit::stripSyntheticSteps(XPointer& p)` returning how many steps were removed;
  `bool bookorbit::hasSyntheticSteps(const XPointer& p)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_xpointer_dom/XPointerDomTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"

using bookorbit::emitXPointer;
using bookorbit::hasSyntheticSteps;
using bookorbit::isSyntheticCrengineElement;
using bookorbit::kNormalizedXPointerDomVersion;
using bookorbit::parseXPointer;
using bookorbit::stripSyntheticSteps;
using bookorbit::XPointer;

namespace {

XPointer parse(const char* raw) {
  XPointer p;
  EXPECT_TRUE(parseXPointer(raw, p)) << raw;
  return p;
}

}  // namespace

TEST(XPointerDom, NormalizationBoundaryMatchesCrengine) {
  EXPECT_EQ(kNormalizedXPointerDomVersion, 20200223);
  EXPECT_EQ(kNormalizedXPointerDomVersion, corpus::kNormalizedDomVersion);
}

// The authoritative set, fb2def.h:40-64 (EL_BOXING_START..EL_BOXING_END plus
// pseudoElem).
TEST(XPointerDom, RecognisesEverySyntheticElement) {
  EXPECT_TRUE(isSyntheticCrengineElement("autoBoxing"));
  EXPECT_TRUE(isSyntheticCrengineElement("tabularBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("rubyBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("mathBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("floatBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("inlineBox"));
  EXPECT_TRUE(isSyntheticCrengineElement("pseudoElem"));
}

TEST(XPointerDom, RealElementsAreNotSynthetic) {
  EXPECT_FALSE(isSyntheticCrengineElement("div"));
  EXPECT_FALSE(isSyntheticCrengineElement("table"));
  EXPECT_FALSE(isSyntheticCrengineElement("tbody"));
  EXPECT_FALSE(isSyntheticCrengineElement("img"));
  EXPECT_FALSE(isSyntheticCrengineElement("body"));
  EXPECT_FALSE(isSyntheticCrengineElement("DocFragment"));
  EXPECT_FALSE(isSyntheticCrengineElement(""));
  // Case matters: crengine's names are camelCase and an author's <autoboxing>
  // would be a real element.
  EXPECT_FALSE(isSyntheticCrengineElement("autoboxing"));
}

// Verbatim from fixtures/test_tables_dom20171225.csv line 5.
TEST(XPointerDom, StripsTabularBoxFromALegacyTableXPointer) {
  XPointer p = parse("/body/DocFragment[4]/body/section/table/tabularBox/tbody/tr/td/text().0");
  EXPECT_TRUE(hasSyntheticSteps(p));
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_FALSE(hasSyntheticSteps(p));
  EXPECT_EQ(emitXPointer(p),
            "/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
}

// Verbatim from fixtures/test_mixed_images_dom20171225.csv line 2.
TEST(XPointerDom, StripsAutoBoxingFromALegacyImageXPointer) {
  XPointer p = parse("/body/DocFragment[3]/body/autoBoxing/img.0");
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(emitXPointer(p), "/body[1]/DocFragment[3]/body[1]/img[1].0");
}

TEST(XPointerDom, StrippingIsIdempotent) {
  XPointer p = parse("/body/DocFragment[3]/body/autoBoxing/img.0");
  EXPECT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(stripSyntheticSteps(p), 0);
}

TEST(XPointerDom, ModernXPointersHaveNothingToStrip) {
  XPointer p = parse("/body[1]/DocFragment[4]/body[1]/section[1]/table[1]/tbody[1]/tr[1]/td[1]/text()[1].0");
  EXPECT_FALSE(hasSyntheticSteps(p));
  EXPECT_EQ(stripSyntheticSteps(p), 0);
}

// The honest limitation: dropping autoBoxing[2] loses the disambiguation that
// the current DOM version expresses as img[2]. Documented, not hidden.
TEST(XPointerDom, StrippingCanLoseSiblingDisambiguation) {
  XPointer legacy = parse("/body/DocFragment[4]/body/autoBoxing[2]/img[1].0");
  ASSERT_EQ(stripSyntheticSteps(legacy), 1);
  EXPECT_EQ(emitXPointer(legacy), "/body[1]/DocFragment[4]/body[1]/img[1].0");
  EXPECT_NE(emitXPointer(legacy), "/body[1]/DocFragment[4]/body[1]/img[2].0");
}

// Corpus-wide: exactly the measured number of rows carry synthetic steps, and
// they are confined to the pre-normalization DOM version plus a handful of
// tabularBox cases.
TEST(XPointerDom, CorpusSyntheticStepCountIsStable) {
  size_t rows = 0;
  size_t synthetic = 0;
  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& path : corpus::fixturePaths(dom)) {
      corpus::Fixture fixture;
      ASSERT_TRUE(corpus::loadFixture(path, fixture)) << path;
      for (const auto& row : fixture.rows) {
        XPointer p;
        ASSERT_TRUE(parseXPointer(row.xpointer, p)) << row.xpointer;
        rows++;
        if (hasSyntheticSteps(p)) synthetic++;
      }
    }
  }
  // Do NOT assert a magic corpus size. Record the real numbers printed by
  // generate_ground_truth.sh on first run, then pin them here so drift is
  // caught. Until then, assert only the properties that must hold.
  EXPECT_GT(rows, 100u) << "corpus too small to prove anything";
  EXPECT_GT(synthetic, 0u) << "no synthetic steps found - is the corpus real?";
  EXPECT_LT(synthetic, rows) << "every row synthetic - generator is wrong";
}

TEST(XPointerDom, StrippingNeverTouchesTheDocFragmentOrOffset) {
  XPointer p = parse("/body/DocFragment[7]/body/autoBoxing/p[3]/text()[2].44");
  ASSERT_EQ(stripSyntheticSteps(p), 1);
  EXPECT_EQ(p.docFragment, 7);
  EXPECT_EQ(p.textNodeIndex, 2);
  EXPECT_EQ(p.charOffset, 44);
}
```

Create `test/bookorbit_xpointer_dom/CMakeLists.txt`:

```cmake
add_executable(BookOrbitXPointerDomTest
  XPointerDomTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerDomTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerDomTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerDomTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerDomTest)
```

Add `add_subdirectory(bookorbit_xpointer_dom)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'isSyntheticCrengineElement' is not a member of 'bookorbit'`.

- [ ] **Step 3: Write minimal implementation**

Append to `lib/BookOrbit/XPointer.h`, inside `namespace bookorbit`, before the closing brace:

```cpp
// crengine normalizes xpointers — an explicit index on every step — from this
// DOM version onward. Documents rendered under an older version emit the
// unindexed form. Xpointers are only comparable within a DOM version.
inline constexpr int kNormalizedXPointerDomVersion = 20200223;

// True for elements crengine synthesizes into the DOM that do not exist in the
// source XHTML. The set is the boxing range at
// crengine/include/fb2def.h:40-64 (EL_BOXING_START..EL_BOXING_END) plus
// pseudoElem, which is declared alongside them.
bool isSyntheticCrengineElement(std::string_view name);

bool hasSyntheticSteps(const XPointer& p);

// Removes synthetic steps so the ancestry can be matched against the source
// XHTML, and returns how many were removed.
//
// This is lossy by nature: a legacy "/autoBoxing[2]/img[1]" becomes "/img[1]"
// where the current DOM version would say "/img[2]". Accepting the ambiguity is
// the price of reading pre-2020 xpointers at all; positions the device itself
// writes never contain synthetic steps.
int stripSyntheticSteps(XPointer& p);
```

Append to `lib/BookOrbit/XPointer.cpp`, inside `namespace bookorbit`, before the closing brace:

```cpp
bool isSyntheticCrengineElement(const std::string_view name) {
  return name == "autoBoxing" || name == "tabularBox" || name == "rubyBox" || name == "mathBox" ||
         name == "floatBox" || name == "inlineBox" || name == "pseudoElem";
}

bool hasSyntheticSteps(const XPointer& p) {
  for (const auto& step : p.steps) {
    if (isSyntheticCrengineElement(step.name)) return true;
  }
  return false;
}

int stripSyntheticSteps(XPointer& p) {
  int removed = 0;
  std::vector<XPointerStep> kept;
  kept.reserve(p.steps.size());
  for (auto& step : p.steps) {
    if (isSyntheticCrengineElement(step.name)) {
      removed++;
      continue;
    }
    kept.push_back(std::move(step));
  }
  if (removed > 0) p.steps = std::move(kept);
  return removed;
}
```

Add `#include <utility>` to `lib/BookOrbit/XPointer.cpp`'s include block for `std::move`.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitXPointerDom --output-on-failure
```

Expected: 10 tests PASS. Note the synthetic/total counts it prints and pin them in the test.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/XPointer.h lib/BookOrbit/XPointer.cpp test/bookorbit_xpointer_dom test/CMakeLists.txt
git commit -m "feat: tolerate crengine DOM versions and synthetic boxing steps"
```

---

### Task 4: Full element-ancestry streaming resolver

`ChapterXPathResolver` already counts same-name siblings while streaming
(`ChapterXPathResolver.cpp:31-45`, `:201-290`), but only ever *searches for* the
Nth `p` or `li` (`findXPathForElement`, `:522`). It cannot answer "where is
`/body[1]/DocFragment[1]/body[1]/div[1]/svg[1]`" — the shape the corpus is full
of. This task adds the general form, decoupled from `Epub` so it is
host-testable over a plain buffer.

The corpus makes this verifiable end to end: **all generated xpointers
resolve against the extracted spine XHTML** once synthetic steps are stripped.
That number is asserted, so a regression cannot pass quietly.

**Files:**
- Create: `lib/BookOrbit/XPointerResolver.h`, `lib/BookOrbit/XPointerResolver.cpp`
- Create: `test/bookorbit_xpointer_resolver/CMakeLists.txt`, `test/bookorbit_xpointer_resolver/XPointerResolverTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::XPointer`, `parseXPointer`, `emitXPointer`, `stripSyntheticSteps` (Tasks 2-3); `corpus::loadFixture`, `corpus::spinePath` (Task 1).
- Produces:
  `bool bookorbit::resolveXPointerToOffset(std::string_view xhtml, const XPointer& target, uint32_t& visibleOffset)`;
  `bool bookorbit::resolveOffsetToXPointer(std::string_view xhtml, uint32_t visibleOffset, int spineIndex, XPointer& out)`;
  `bool bookorbit::visibleTextLength(std::string_view xhtml, uint32_t& length)`.

Offsets are zero-based **visible Unicode codepoints**, the same stable unit
`ChapterXPathResolver::findXPathForVisibleTextOffset` already uses, with the
same non-visible tag set (`head`, `style`, `script`, `title`, `rp`, `rt`).

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_xpointer_resolver/XPointerResolverTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "GroundTruthCorpus.h"
#include "lib/BookOrbit/XPointer.h"
#include "lib/BookOrbit/XPointerResolver.h"

using bookorbit::emitXPointer;
using bookorbit::parseXPointer;
using bookorbit::resolveOffsetToXPointer;
using bookorbit::resolveXPointerToOffset;
using bookorbit::stripSyntheticSteps;
using bookorbit::visibleTextLength;
using bookorbit::XPointer;

namespace {

// Hand-countable document. Visible codepoints, in order:
//   h1        "ABCDE"  -> 0..4
//   div/p[1]  "12345"  -> 5..9
//   div/p[2]  "67890"  -> 10..14
//   p[1]      "XYZ"    -> 15..17
// <title> is inside <head> and contributes nothing.
constexpr char kDoc[] =
    "<html><head><title>IGNORED</title></head><body>"
    "<h1>ABCDE</h1>"
    "<div><p>12345</p><p>67890</p></div>"
    "<p>XYZ</p>"
    "</body></html>";

uint32_t offsetOf(const char* xpointer) {
  XPointer target;
  EXPECT_TRUE(parseXPointer(xpointer, target));
  uint32_t offset = 0xFFFFFFFFu;
  EXPECT_TRUE(resolveXPointerToOffset(kDoc, target, offset));
  return offset;
}

}  // namespace

TEST(XPointerResolver, CountsOnlyVisibleText) {
  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(kDoc, length));
  EXPECT_EQ(length, 18u);
}

TEST(XPointerResolver, ResolvesTopLevelElement) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/h1[1]"), 0u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]"), 5u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/p[1]"), 15u);
}

// Sibling counting is per parent, not per document: the div's two <p> children
// are p[1] and p[2] even though a third <p> follows at body level.
TEST(XPointerResolver, SiblingIndicesAreScopedToTheirParent) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[1]"), 5u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2]"), 10u);
}

TEST(XPointerResolver, UnindexedFormResolvesIdentically) {
  EXPECT_EQ(offsetOf("/body/DocFragment[1]/body/div/p[2]"), 10u);
}

TEST(XPointerResolver, CharacterOffsetIsAddedToTheTextNodeStart) {
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2]/text()[1].2"), 12u);
  EXPECT_EQ(offsetOf("/body[1]/DocFragment[1]/body[1]/div[1]/p[2].3"), 13u);
}

TEST(XPointerResolver, MissingElementFails) {
  XPointer target;
  ASSERT_TRUE(parseXPointer("/body[1]/DocFragment[1]/body[1]/div[9]/p[1]", target));
  uint32_t offset = 0;
  EXPECT_FALSE(resolveXPointerToOffset(kDoc, target, offset));
}

TEST(XPointerResolver, OffsetResolvesToCanonicalXPointer) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 12u, 0, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[1]/body[1]/div[1]/p[2]/text()[1].2");
}

TEST(XPointerResolver, OffsetZeroResolvesToTheFirstVisibleElement) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 0u, 0, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[1]/body[1]/h1[1]/text()[1].0");
}

TEST(XPointerResolver, SpineIndexBecomesTheDocFragmentIndex) {
  XPointer out;
  ASSERT_TRUE(resolveOffsetToXPointer(kDoc, 16u, 7, out));
  EXPECT_EQ(emitXPointer(out), "/body[1]/DocFragment[8]/body[1]/p[1]/text()[1].1");
}

TEST(XPointerResolver, OffsetPastTheEndFails) {
  XPointer out;
  EXPECT_FALSE(resolveOffsetToXPointer(kDoc, 999u, 0, out));
}

TEST(XPointerResolver, RoundTripsEveryVisibleOffsetExactly) {
  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(kDoc, length));
  for (uint32_t offset = 0; offset < length; offset++) {
    XPointer out;
    ASSERT_TRUE(resolveOffsetToXPointer(kDoc, offset, 0, out)) << offset;
    uint32_t back = 0xFFFFFFFFu;
    ASSERT_TRUE(resolveXPointerToOffset(kDoc, out, back)) << emitXPointer(out);
    EXPECT_EQ(back, offset) << emitXPointer(out);
  }
}

// ---- The corpus test: real KOReader xpointers, real spine XHTML ------------

// Every xpointer KOReader emitted for these books, resolved against the very
// spine item it names. Expect 100% resolution across both DOM versions.
TEST(XPointerResolverCorpus, ResolvesEveryGeneratedXPointer) {
  size_t rows = 0;
  size_t resolved = 0;

  for (const int dom : {corpus::kOldestDomVersion, corpus::kLatestDomVersion}) {
    for (const auto& name : corpus::epubNames()) {
      corpus::Fixture fixture;
      ASSERT_TRUE(corpus::loadFixture(corpus::fixturePath(name, dom), fixture)) << name;

      for (const auto& row : fixture.rows) {
        XPointer target;
        ASSERT_TRUE(parseXPointer(row.xpointer, target)) << row.xpointer;
        // Synthetic boxing elements exist only in crengine's DOM, never in the
        // source XHTML we stream (Task 3).
        stripSyntheticSteps(target);

        const std::string xhtml = corpus::readFile(corpus::spinePath(name, target.docFragment));
        ASSERT_FALSE(xhtml.empty()) << corpus::spinePath(name, target.docFragment);

        rows++;
        uint32_t offset = 0;
        if (resolveXPointerToOffset(xhtml, target, offset)) {
          resolved++;
        } else {
          ADD_FAILURE() << name << " dom " << dom << ": " << row.xpointer;
        }
      }
    }
  }

  EXPECT_GT(rows, 100u) << "corpus too small to prove anything";
  EXPECT_EQ(resolved, rows);
}

// Resolution must respect document order: a later page is never at a smaller
// offset than an earlier page in the same spine item.
TEST(XPointerResolverCorpus, OffsetsAdvanceWithinASpineItem) {
  for (const auto& name : corpus::epubNames()) {
    corpus::Fixture fixture;
    ASSERT_TRUE(corpus::loadFixture(corpus::fixturePath(name, corpus::kLatestDomVersion), fixture)) << name;

    int previousFragment = 0;
    uint32_t previousOffset = 0;
    for (const auto& row : fixture.rows) {
      XPointer target;
      ASSERT_TRUE(parseXPointer(row.xpointer, target)) << row.xpointer;
      stripSyntheticSteps(target);

      const std::string xhtml = corpus::readFile(corpus::spinePath(name, target.docFragment));
      ASSERT_FALSE(xhtml.empty());

      uint32_t offset = 0;
      ASSERT_TRUE(resolveXPointerToOffset(xhtml, target, offset)) << row.xpointer;

      if (target.docFragment == previousFragment) {
        EXPECT_GE(offset, previousOffset) << name << ": " << row.xpointer;
      }
      previousFragment = target.docFragment;
      previousOffset = offset;
    }
  }
}

// The fidelity measurement the spec asks for: offset -> xpointer -> offset over
// every visible position of a real spine item, exactly.
TEST(XPointerResolverCorpus, RoundTripsEveryVisibleOffsetInARealSpineItem) {
  const std::string xhtml = corpus::readFile(corpus::spinePath("test_reader_rendering_matrix", 2));
  ASSERT_FALSE(xhtml.empty());

  uint32_t length = 0;
  ASSERT_TRUE(visibleTextLength(xhtml, length));
  ASSERT_GT(length, 100u);

  for (uint32_t offset = 0; offset < length; offset++) {
    XPointer out;
    ASSERT_TRUE(resolveOffsetToXPointer(xhtml, offset, 1, out)) << offset;
    uint32_t back = 0xFFFFFFFFu;
    ASSERT_TRUE(resolveXPointerToOffset(xhtml, out, back)) << emitXPointer(out);
    EXPECT_EQ(back, offset) << emitXPointer(out);
  }
}
```

Create `test/bookorbit_xpointer_resolver/CMakeLists.txt`:

```cmake
add_executable(BookOrbitXPointerResolverTest
  XPointerResolverTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointerResolver.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BookOrbitXPointerResolverTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/bookorbit_xpointer_corpus
)

target_compile_definitions(BookOrbitXPointerResolverTest PRIVATE
  CORPUS_FIXTURE_DIR="${REPO_ROOT}/test/bookorbit_xpointer_corpus/fixtures"
)

target_link_libraries(BookOrbitXPointerResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(BookOrbitXPointerResolverTest)
```

Add `add_subdirectory(bookorbit_xpointer_resolver)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/XPointerResolver.cpp`, then `XPointerResolver.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/XPointerResolver.h`:

```cpp
#pragma once

#include <cstdint>
#include <string_view>

#include "XPointer.h"

namespace bookorbit {

// Streams a spine item's XHTML and resolves an xpointer's element ancestry to a
// zero-based visible-codepoint offset. Same-name siblings are counted per
// parent, so /div[1]/p[2] means "the second <p> child of the first <div>",
// exactly as crengine means it.
//
// The caller must have removed synthetic boxing steps first
// (stripSyntheticSteps): those elements exist in crengine's DOM but never in
// the source XHTML streamed here.
//
// Non-visible containers (head, style, script, title, rp, rt) contribute no
// codepoints, matching ChapterXPathResolver's existing rule.
bool resolveXPointerToOffset(std::string_view xhtml, const XPointer& target, uint32_t& visibleOffset);

// The inverse: the canonical xpointer for a visible-codepoint offset, with
// spineIndex (0-based) becoming the DocFragment index.
bool resolveOffsetToXPointer(std::string_view xhtml, uint32_t visibleOffset, int spineIndex, XPointer& out);

// Total visible codepoints in the document.
bool visibleTextLength(std::string_view xhtml, uint32_t& length);

}  // namespace bookorbit
```

Create `lib/BookOrbit/XPointerResolver.cpp`:

```cpp
#include "XPointerResolver.h"

#include <expat.h>

#include <cstring>
#include <string>
#include <vector>

#include "Utf8.h"

namespace bookorbit {
namespace {

std::string stripPrefix(const XML_Char* name) {
  if (!name) return {};
  const char* local = std::strrchr(name, ':');
  return local ? std::string(local + 1) : std::string(name);
}

bool isNonVisibleTag(const std::string& name) {
  return name == "head" || name == "style" || name == "script" || name == "title" || name == "rp" || name == "rt";
}

size_t countUtf8Codepoints(const char* data, const int len) {
  if (!data || len <= 0) return 0;
  size_t count = 0;
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data);
  const unsigned char* end = ptr + len;
  while (ptr < end) {
    utf8NextCodepoint(&ptr);
    count++;
  }
  return count;
}

struct NameCounter {
  std::string name;
  int count;
};

// Per-parent same-name sibling counters. One entry is pushed for every open
// element, so counting is always scoped to the immediate parent.
struct ParentState {
  std::vector<NameCounter> children;
  int textNodes = 0;

  int nextIndex(const std::string& name) {
    for (auto& child : children) {
      if (child.name == name) {
        child.count++;
        return child.count;
      }
    }
    children.push_back({name, 1});
    return 1;
  }
};

// One expat pass serving both directions. Which direction is active depends on
// whether target or wantOffset is set.
class AncestryWalker {
 public:
  AncestryWalker() {
    parser = XML_ParserCreate(nullptr);
    if (!parser) return;
    XML_SetUserData(parser, this);
    XML_SetElementHandler(parser, &AncestryWalker::startElement, &AncestryWalker::endElement);
    XML_SetCharacterDataHandler(parser, &AncestryWalker::characterData);
  }

  ~AncestryWalker() {
    if (parser) XML_ParserFree(parser);
  }

  bool run(const std::string_view xhtml) {
    if (!parser) return false;
    if (XML_Parse(parser, xhtml.data(), static_cast<int>(xhtml.size()), XML_TRUE) != XML_STATUS_OK) {
      const enum XML_Error error = XML_GetErrorCode(parser);
      if (error != XML_ERROR_ABORTED) return false;
    }
    return true;
  }

  // Direction 1: find this element ancestry.
  const XPointer* target = nullptr;
  bool targetFound = false;
  uint32_t targetOffset = 0;

  // Direction 2: find the element covering this offset.
  bool wantOffset = false;
  uint32_t wantedOffset = 0;
  bool offsetFound = false;
  std::vector<XPointerStep> offsetPath;
  int offsetTextNode = 0;
  long offsetChar = 0;

  uint32_t visibleChars = 0;

 private:
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char**) {
    static_cast<AncestryWalker*>(userData)->onStart(stripPrefix(name));
  }
  static void XMLCALL endElement(void* userData, const XML_Char* name) {
    static_cast<AncestryWalker*>(userData)->onEnd(stripPrefix(name));
  }
  static void XMLCALL characterData(void* userData, const XML_Char* data, const int len) {
    static_cast<AncestryWalker*>(userData)->onText(data, len);
  }

  void onStart(const std::string& name) {
    if (stopped) return;

    if (!insideBody) {
      if (name == "body") {
        insideBody = true;
        parents.emplace_back();
      } else if (isNonVisibleTag(name)) {
        nonVisibleDepth++;
      }
      return;
    }

    const int index = parents.back().nextIndex(name);
    path.push_back({name, index});
    parents.emplace_back();
    if (isNonVisibleTag(name)) nonVisibleDepth++;

    if (target && !targetFound && matchesTarget()) {
      // The element starts here. A text() step or a character offset is applied
      // by onText once the right text node is reached; with neither, the
      // element start is the answer.
      if (target->textNodeIndex <= 0 && target->charOffset <= 0) {
        targetFound = true;
        targetOffset = visibleChars;
        stop();
        return;
      }
      insideTarget = true;
    }
  }

  void onEnd(const std::string& name) {
    if (stopped) return;
    if (isNonVisibleTag(name) && nonVisibleDepth > 0) nonVisibleDepth--;
    if (!insideBody) return;

    if (path.empty()) {
      insideBody = false;
      if (!parents.empty()) parents.pop_back();
      return;
    }

    if (insideTarget && path.size() == target->steps.size()) {
      // The target element closed before its text node was found. Fall back to
      // the element start plus the raw offset, so a stale offset still lands
      // inside the right element rather than failing outright.
      targetFound = true;
      targetOffset = targetElementStart + static_cast<uint32_t>(target->charOffset > 0 ? target->charOffset : 0);
      stop();
      return;
    }

    path.pop_back();
    if (!parents.empty()) parents.pop_back();
  }

  void onText(const char* data, const int len) {
    if (stopped || !insideBody || nonVisibleDepth > 0) return;

    const size_t codepoints = countUtf8Codepoints(data, len);
    if (codepoints == 0) return;

    const int textNode = parents.empty() ? 1 : ++parents.back().textNodes;

    if (insideTarget && path.size() == target->steps.size()) {
      const int wanted = target->textNodeIndex > 0 ? target->textNodeIndex : 1;
      if (textNode == wanted) {
        targetFound = true;
        targetOffset = visibleChars + static_cast<uint32_t>(target->charOffset > 0 ? target->charOffset : 0);
        stop();
        return;
      }
    }

    if (wantOffset && !offsetFound && wantedOffset >= visibleChars &&
        wantedOffset < visibleChars + static_cast<uint32_t>(codepoints)) {
      offsetFound = true;
      offsetPath = path;
      offsetTextNode = textNode;
      offsetChar = static_cast<long>(wantedOffset - visibleChars);
      stop();
      return;
    }

    visibleChars += static_cast<uint32_t>(codepoints);
  }

  bool matchesTarget() {
    if (path.size() != target->steps.size()) return false;
    for (size_t i = 0; i < path.size(); i++) {
      if (path[i].name != target->steps[i].name || path[i].index != target->steps[i].index) return false;
    }
    targetElementStart = visibleChars;
    return true;
  }

  void stop() {
    stopped = true;
    XML_StopParser(parser, XML_FALSE);
  }

  XML_Parser parser = nullptr;
  std::vector<ParentState> parents;
  std::vector<XPointerStep> path;
  bool insideBody = false;
  bool insideTarget = false;
  bool stopped = false;
  int nonVisibleDepth = 0;
  uint32_t targetElementStart = 0;
};

}  // namespace

bool resolveXPointerToOffset(const std::string_view xhtml, const XPointer& target, uint32_t& visibleOffset) {
  if (!target.valid) return false;

  // An empty step list is the chapter start: offset 0, no parse needed.
  if (target.steps.empty()) {
    visibleOffset = static_cast<uint32_t>(target.charOffset > 0 ? target.charOffset : 0);
    return true;
  }

  AncestryWalker walker;
  walker.target = &target;
  if (!walker.run(xhtml) || !walker.targetFound) return false;
  visibleOffset = walker.targetOffset;
  return true;
}

bool resolveOffsetToXPointer(const std::string_view xhtml, const uint32_t visibleOffset, const int spineIndex,
                             XPointer& out) {
  out = XPointer{};
  if (spineIndex < 0) return false;

  AncestryWalker walker;
  walker.wantOffset = true;
  walker.wantedOffset = visibleOffset;
  if (!walker.run(xhtml) || !walker.offsetFound) return false;

  out.docFragment = spineIndex + 1;
  out.steps = walker.offsetPath;
  out.textNodeIndex = walker.offsetTextNode;
  out.charOffset = walker.offsetChar;
  out.valid = true;
  return true;
}

bool visibleTextLength(const std::string_view xhtml, uint32_t& length) {
  AncestryWalker walker;
  if (!walker.run(xhtml)) return false;
  length = walker.visibleChars;
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitXPointerResolver --output-on-failure
```

Expected: 14 tests PASS, with `XPointerResolverCorpus.ResolvesEveryGeneratedXPointer` resolving 100% of rows.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/XPointerResolver.h lib/BookOrbit/XPointerResolver.cpp test/bookorbit_xpointer_resolver test/CMakeLists.txt
git commit -m "feat: resolve xpointers by full element ancestry"
```

---

### Task 5: Route ProgressMapper and ChapterXPathResolver through the grammar

The five defect sites, all in `lib/KOReaderSync/`:

| Site | Current code | Replacement |
|---|---|---|
| `ProgressMapper.cpp:62` | `isChapterStartXPath` searching `"/body/DocFragment["` | `bookorbit::isChapterStartXPointer` |
| `ProgressMapper.cpp:134` | `parseXPathSteps` searching `"/body/DocFragment["` | `bookorbit::parseXPointer` |
| `ProgressMapper.cpp:192` | `rewriteDocFragment` searching `"/body/DocFragment["` | `bookorbit::rewriteDocFragmentIndex` |
| `ProgressMapper.cpp:257` | `parseIndex(xpath, "/body/DocFragment[")` | `bookorbit::xpointerDocFragmentIndex` |
| `ProgressMapper.cpp:1006` + `ChapterXPathResolver.cpp:52` | emit `"/body/DocFragment[N]/body"` | `bookorbit::buildCanonicalXPointer` |

**Files:**
- Create: `test/bookorbit_xpointer_bridge/CMakeLists.txt`, `test/bookorbit_xpointer_bridge/XPointerBridgeTest.cpp`
- Modify: `lib/KOReaderSync/ProgressMapper.cpp`, `lib/KOReaderSync/ChapterXPathResolver.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Tasks 2 and 3.
- Produces: `bool bookorbit::toXPathSteps(std::string_view raw, char (*tags)[12], int* siblingIndices, int capacity, int& count)` — the fixed-size adapter `ProgressMapper`'s existing `XPathStep steps[MAX_XPATH_DEPTH]` (`ProgressMapper.cpp:122-127`, `tag[12]`, `MAX_XPATH_DEPTH = 16`) needs, so the mapper keeps its stack-only step array and allocates nothing per call.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_xpointer_bridge/XPointerBridgeTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstring>

#include "lib/BookOrbit/XPointer.h"

using bookorbit::toXPathSteps;

namespace {

constexpr int kMaxDepth = 16;

// Thin wrapper so the tests below read as the call ProgressMapper will make.
bool toXPointerBridgeCall(char (&tags)[kMaxDepth][12], int (&indices)[kMaxDepth], int& count, const char* raw) {
  return toXPathSteps(raw, tags, indices, kMaxDepth, count);
}

}  // namespace

TEST(XPointerBridge, FillsFixedSizeStepArrayFromIndexedForm) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(toXPointerBridgeCall(tags, indices, count,
                                   "/body[1]/DocFragment[1]/body[1]/div[1]/ul[1]/li[4]/text()[1].51"));
  ASSERT_EQ(count, 3);
  EXPECT_STREQ(tags[0], "div");
  EXPECT_EQ(indices[0], 1);
  EXPECT_STREQ(tags[1], "ul");
  EXPECT_EQ(indices[1], 1);
  EXPECT_STREQ(tags[2], "li");
  EXPECT_EQ(indices[2], 4);
}

TEST(XPointerBridge, FillsFixedSizeStepArrayFromUnindexedForm) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(toXPointerBridgeCall(tags, indices, count, "/body/DocFragment[1]/body/div/ul/li[4]"));
  ASSERT_EQ(count, 3);
  EXPECT_STREQ(tags[0], "div");
  EXPECT_EQ(indices[0], 1);
  EXPECT_STREQ(tags[2], "li");
  EXPECT_EQ(indices[2], 4);
}

TEST(XPointerBridge, TruncatesOverlongTagNamesSafely) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 0;

  ASSERT_TRUE(toXPointerBridgeCall(tags, indices, count, "/body[1]/DocFragment[1]/body[1]/averyverylongtagname[2]"));
  ASSERT_EQ(count, 1);
  EXPECT_EQ(std::strlen(tags[0]), 11u);
  EXPECT_STREQ(tags[0], "averyverylo");
  EXPECT_EQ(indices[0], 2);
}

TEST(XPointerBridge, StopsAtCapacity) {
  char tags[2][12] = {};
  int indices[2] = {};
  int count = 0;

  EXPECT_TRUE(toXPathSteps("/body[1]/DocFragment[1]/body[1]/a[1]/b[1]/c[1]", tags, indices, 2, count));
  EXPECT_EQ(count, 2);
}

TEST(XPointerBridge, RejectsNonXPointers) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 7;

  EXPECT_FALSE(toXPathSteps("nonsense", tags, indices, kMaxDepth, count));
  EXPECT_EQ(count, 0);
}

TEST(XPointerBridge, ChapterStartYieldsZeroSteps) {
  char tags[kMaxDepth][12] = {};
  int indices[kMaxDepth] = {};
  int count = 7;

  ASSERT_TRUE(toXPathSteps("/body[1]/DocFragment[9]/body[1]", tags, indices, kMaxDepth, count));
  EXPECT_EQ(count, 0);
}
```

Create `test/bookorbit_xpointer_bridge/CMakeLists.txt`:

```cmake
add_executable(BookOrbitXPointerBridgeTest
  XPointerBridgeTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
)

target_include_directories(BookOrbitXPointerBridgeTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitXPointerBridgeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitXPointerBridgeTest)
```

Add `add_subdirectory(bookorbit_xpointer_bridge)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'toXPathSteps' is not a member of 'bookorbit'`.

- [ ] **Step 3: Write minimal implementation**

Append to `lib/BookOrbit/XPointer.h`, inside `namespace bookorbit`, before the closing brace:

```cpp
// Fixed-size adapter for ProgressMapper's stack-only XPathStep array
// (ProgressMapper.cpp:122-127: char tag[12], MAX_XPATH_DEPTH = 16). Tag names
// longer than 11 characters are truncated; no allocation happens per call
// beyond the parse itself.
bool toXPathSteps(std::string_view raw, char (*tags)[12], int* siblingIndices, int capacity, int& count);
```

Append to `lib/BookOrbit/XPointer.cpp`, inside `namespace bookorbit`, before the closing brace:

```cpp
bool toXPathSteps(const std::string_view raw, char (*tags)[12], int* siblingIndices, const int capacity, int& count) {
  count = 0;
  if (!tags || !siblingIndices || capacity <= 0) return false;

  XPointer parsed;
  if (!parseXPointer(raw, parsed)) return false;

  for (const auto& step : parsed.steps) {
    if (count >= capacity) break;
    const size_t len = step.name.size() < 11 ? step.name.size() : 11;
    std::memcpy(tags[count], step.name.data(), len);
    tags[count][len] = '\0';
    siblingIndices[count] = step.index > 0 ? step.index : 1;
    count++;
  }
  return true;
}
```

Add `#include <cstring>` to `lib/BookOrbit/XPointer.cpp`'s include block.

Now rewrite the five defect sites.

In `lib/KOReaderSync/ProgressMapper.cpp`, add to the include block:

```cpp
#include "BookOrbit/XPointer.h"
```

Replace `isChapterStartXPath` (`ProgressMapper.cpp:56` onward) in full with:

```cpp
// Delegates to the shared grammar so both indexed ("/body[1]/DocFragment[3]")
// and unindexed ("/body/DocFragment[3]") forms are recognised. The previous
// literal search for "/body/DocFragment[" missed every real KOReader xpointer.
bool isChapterStartXPath(const std::string& xpath) { return bookorbit::isChapterStartXPointer(xpath); }
```

Replace `parseXPathSteps` (`ProgressMapper.cpp:130` onward) in full with:

```cpp
int parseXPathSteps(const std::string& xpath, XPathStep steps[MAX_XPATH_DEPTH]) {
  char tags[MAX_XPATH_DEPTH][12] = {};
  int indices[MAX_XPATH_DEPTH] = {};
  int count = 0;
  if (!bookorbit::toXPathSteps(xpath, tags, indices, MAX_XPATH_DEPTH, count)) return 0;

  for (int i = 0; i < count; i++) {
    std::memcpy(steps[i].tag, tags[i], sizeof(steps[i].tag));
    steps[i].siblingIndex = indices[i];
  }
  return count;
}
```

Replace `rewriteDocFragment` (`ProgressMapper.cpp:190` onward) in full with:

```cpp
bool rewriteDocFragment(std::string& xpath, const int spineIndex) {
  if (spineIndex < 0) return false;
  return bookorbit::rewriteDocFragmentIndex(xpath, spineIndex + 1);
}
```

At `ProgressMapper.cpp:257`, replace:

```cpp
  const int sourceSpineIndex = parseIndex(xpath, "/body/DocFragment[") - 1;
```

with:

```cpp
  const int sourceSpineIndex = bookorbit::xpointerDocFragmentIndex(xpath) - 1;
```

At `ProgressMapper.cpp:906`, replace:

```cpp
  const int docFrag = parseIndex(mappedKoPos.xpath, "/body/DocFragment[");
```

with:

```cpp
  const int docFrag = bookorbit::xpointerDocFragmentIndex(mappedKoPos.xpath);
```

In `ProgressMapper::toCrossPoint`, normalize on ingest at the point `mappedKoPos`
is first populated from `koPos`, before any other use:

```cpp
  // Normalize on ingest. Real KOReader sidecars carry the fully-indexed form;
  // CrossInk historically wrote the unindexed one. Everything downstream sees
  // exactly one shape.
  const std::string normalized = bookorbit::normalizeXPointer(koPos.xpath);
  if (!normalized.empty()) {
    mappedKoPos.xpath = normalized;
  }
```

Replace the first line of `ProgressMapper::generateXPath` (`ProgressMapper.cpp:1006`):

```cpp
  const std::string base = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
```

with:

```cpp
  const std::string base = bookorbit::buildCanonicalXPointer(spineIndex, {}, 0, -1);
```

and its return line:

```cpp
  return (p > 0) ? base + "/p[" + std::to_string(p) + "]" : base;
```

with:

```cpp
  return (p > 0) ? bookorbit::buildCanonicalXPointer(spineIndex, {{"p", p}}, 0, -1) : base;
```

In `lib/KOReaderSync/ChapterXPathResolver.cpp`, add to the include block:

```cpp
#include "BookOrbit/XPointer.h"
```

and replace `buildParagraphXPath` (`ChapterXPathResolver.cpp:52-61`) in full with:

```cpp
std::string buildParagraphXPath(const int spineIndex, const std::vector<PathSegment>& path, const int textNodeIndex,
                                const size_t charOffset) {
  std::vector<bookorbit::XPointerStep> steps;
  steps.reserve(path.size());
  for (const auto& segment : path) {
    steps.push_back({segment.name, segment.index});
  }
  const long offset = (textNodeIndex > 0 && charOffset > 0) ? static_cast<long>(charOffset) : -1;
  return bookorbit::buildCanonicalXPointer(spineIndex, steps, textNodeIndex > 0 ? textNodeIndex : 0, offset);
}
```

Add `${REPO_ROOT}/lib` to the include path of any device build that needs it —
PlatformIO already places `lib/*` on the include path, so `#include
"BookOrbit/XPointer.h"` resolves with no `platformio.ini` change.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
```

Expected: 6 `BookOrbitXPointerBridge` tests PASS and the whole suite stays green.

```bash
pio run -e x4-pro && pio run -e default && pio run -e sticky && pio run -e simulator
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

Expected: all four targets link and `scripts/check_firmware_size.py` passes.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/XPointer.h lib/BookOrbit/XPointer.cpp lib/KOReaderSync/ProgressMapper.cpp lib/KOReaderSync/ChapterXPathResolver.cpp test/bookorbit_xpointer_bridge test/CMakeLists.txt
git commit -m "fix: emit and match canonical crengine xpointers in ProgressMapper"
```

---

### Task 6: Progress GET and PUT codecs

**Files:**
- Create: `lib/BookOrbit/BookOrbitProgress.h`, `lib/BookOrbit/BookOrbitProgress.cpp`
- Create: `test/bookorbit_progress/CMakeLists.txt`, `test/bookorbit_progress/BookOrbitProgressTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamingJsonParser`, `JsonCallbacks` (`lib/JsonParser/StreamingJsonParser.h`).
- Produces:
  `struct bookorbit::ProgressRecord { std::string document; float percentage; std::string progress; std::string device; std::string deviceId; uint32_t timestamp; }`;
  `std::string bookorbit::progressGetPath(std::string_view digest)`;
  `std::string bookorbit::encodePutProgress(const ProgressRecord& record)`;
  `bool bookorbit::decodeProgressResponse(std::string_view json, ProgressRecord& out)`.

Request body fields: `document`, `percentage` (float 0..1), `progress`
(xpointer string), `device`, `device_id`, `timestamp`. Response fields:
`percentage`, `progress`, `device`, `device_id`, `timestamp`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_progress/BookOrbitProgressTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/BookOrbitProgress.h"

using bookorbit::decodeProgressResponse;
using bookorbit::encodePutProgress;
using bookorbit::ProgressRecord;
using bookorbit::progressGetPath;

namespace {

ProgressRecord sample() {
  ProgressRecord record;
  record.document = "d18e399f0f79f24d68a8f70b76d59914";
  record.percentage = 0.001859f;
  record.progress = "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0";
  record.device = "CrossInk X4 Pro";
  record.deviceId = "A1B2C3D4E5F6";
  record.timestamp = 1787561453u;
  return record;
}

}  // namespace

TEST(BookOrbitProgress, GetPathCarriesTheDigest) {
  EXPECT_EQ(progressGetPath("d18e399f0f79f24d68a8f70b76d59914"),
            "/koreader/syncs/progress/d18e399f0f79f24d68a8f70b76d59914");
}

TEST(BookOrbitProgress, GetPathRejectsEmptyDigest) { EXPECT_EQ(progressGetPath(""), ""); }

TEST(BookOrbitProgress, EncodesEveryRequestField) {
  EXPECT_EQ(encodePutProgress(sample()),
            "{\"document\":\"d18e399f0f79f24d68a8f70b76d59914\","
            "\"percentage\":0.001859,"
            "\"progress\":\"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0\","
            "\"device\":\"CrossInk X4 Pro\","
            "\"device_id\":\"A1B2C3D4E5F6\","
            "\"timestamp\":1787561453}");
}

TEST(BookOrbitProgress, ClampsPercentageIntoRange) {
  ProgressRecord high = sample();
  high.percentage = 1.7f;
  EXPECT_NE(encodePutProgress(high).find("\"percentage\":1.000000"), std::string::npos);

  ProgressRecord low = sample();
  low.percentage = -0.4f;
  EXPECT_NE(encodePutProgress(low).find("\"percentage\":0.000000"), std::string::npos);
}

// Never degrade silently: an encode with no xpointer is a programming error
// upstream, and the encoder refuses rather than shipping a percentage-only
// record that a KOReader client would then read back as the whole truth.
TEST(BookOrbitProgress, RefusesToEncodeWithoutAnXpointer) {
  ProgressRecord noProgress = sample();
  noProgress.progress.clear();
  EXPECT_EQ(encodePutProgress(noProgress), "");
}

TEST(BookOrbitProgress, RefusesToEncodeWithoutADocument) {
  ProgressRecord noDocument = sample();
  noDocument.document.clear();
  EXPECT_EQ(encodePutProgress(noDocument), "");
}

TEST(BookOrbitProgress, EscapesQuotesAndBackslashesInDeviceNames) {
  ProgressRecord quoted = sample();
  quoted.device = "Jo\"s \\ Reader";
  EXPECT_NE(encodePutProgress(quoted).find("\"device\":\"Jo\\\"s \\\\ Reader\""), std::string::npos);
}

TEST(BookOrbitProgress, DecodesEveryResponseField) {
  const char* json =
      "{\"percentage\":0.2052,"
      "\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[4]/text()[1].96\","
      "\"device\":\"KOReader\","
      "\"device_id\":\"9F8E7D\","
      "\"timestamp\":1787407272}";

  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_FLOAT_EQ(out.percentage, 0.2052f);
  EXPECT_EQ(out.progress, "/body[1]/DocFragment[8]/body[1]/p[4]/text()[1].96");
  EXPECT_EQ(out.device, "KOReader");
  EXPECT_EQ(out.deviceId, "9F8E7D");
  EXPECT_EQ(out.timestamp, 1787407272u);
}

// The unindexed form still arrives from older CrossInk writes; decoding
// normalizes it so callers only ever compare one shape.
TEST(BookOrbitProgress, NormalizesTheProgressFieldOnDecode) {
  const char* json = "{\"percentage\":0.5,\"progress\":\"/body/DocFragment[2]/body/p[3]\",\"timestamp\":1}";
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_EQ(out.progress, "/body[1]/DocFragment[2]/body[1]/p[3]");
}

TEST(BookOrbitProgress, KeepsAnUnparseableProgressStringVerbatim) {
  const char* json = "{\"percentage\":0.5,\"progress\":\"something-else\",\"timestamp\":1}";
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_EQ(out.progress, "something-else");
}

TEST(BookOrbitProgress, EmptyResponseDecodesToAZeroedRecord) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse("{}", out));
  EXPECT_EQ(out.progress, "");
  EXPECT_FLOAT_EQ(out.percentage, 0.0f);
  EXPECT_EQ(out.timestamp, 0u);
}

TEST(BookOrbitProgress, MalformedJsonIsRejected) {
  ProgressRecord out;
  EXPECT_FALSE(decodeProgressResponse("{\"percentage\":", out));
}

TEST(BookOrbitProgress, EncodeDecodeRoundTrip) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(encodePutProgress(sample()), out));
  EXPECT_EQ(out.progress, sample().progress);
  EXPECT_EQ(out.device, sample().device);
  EXPECT_EQ(out.deviceId, sample().deviceId);
  EXPECT_EQ(out.timestamp, sample().timestamp);
}
```

Create `test/bookorbit_progress/CMakeLists.txt`:

```cmake
add_executable(BookOrbitProgressTest
  BookOrbitProgressTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitProgressTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitProgressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitProgressTest)
```

Add `add_subdirectory(bookorbit_progress)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/BookOrbitProgress.cpp`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitProgress.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bookorbit {

// One reading position, in the shape BookOrbit's KOSync-compatible progress
// endpoints use in both directions.
struct ProgressRecord {
  std::string document;        // partial MD5, request only
  float percentage = 0.0f;     // 0..1
  std::string progress;        // canonical crengine xpointer
  std::string device;          // human-readable device name
  std::string deviceId;        // stable device identifier
  uint32_t timestamp = 0;      // unix epoch
};

// GET /koreader/syncs/progress/{digest}. Returns "" for an empty digest.
std::string progressGetPath(std::string_view digest);

// PUT /koreader/syncs/progress body. Returns "" when the record is incomplete —
// a record with no xpointer or no document is never sent, because a
// percentage-only push is exactly the silent degradation this phase forbids.
std::string encodePutProgress(const ProgressRecord& record);

// Decodes either endpoint's response. The progress field is normalized to the
// canonical xpointer form when it parses, and kept verbatim when it does not.
bool decodeProgressResponse(std::string_view json, ProgressRecord& out);

// Shared JSON string escaping, also used by the bulk codec.
void appendJsonString(std::string& out, std::string_view value);

// Shared percentage formatting: clamped to 0..1, six decimal places.
void appendPercentage(std::string& out, float percentage);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitProgress.cpp`:

```cpp
#include "BookOrbitProgress.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct DecodeCtx {
  ProgressRecord* out = nullptr;
  std::string key;
  int depth = 0;
};

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->depth != 1) return;
  const std::string text(value, len);
  if (ctx->key == "progress") {
    const std::string normalized = normalizeXPointer(text);
    ctx->out->progress = normalized.empty() ? text : normalized;
  } else if (ctx->key == "device") {
    ctx->out->device = text;
  } else if (ctx->key == "device_id") {
    ctx->out->deviceId = text;
  } else if (ctx->key == "document") {
    ctx->out->document = text;
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->depth != 1) return;
  const std::string text(value, len);
  if (ctx->key == "percentage") {
    ctx->out->percentage = std::strtof(text.c_str(), nullptr);
  } else if (ctx->key == "timestamp") {
    ctx->out->timestamp = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
  }
}

void onObjectStart(void* raw) { static_cast<DecodeCtx*>(raw)->depth++; }

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->depth--;
  ctx->key.clear();
}

void onArrayStart(void*) {}
void onArrayEnd(void*) {}
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

void appendJsonString(std::string& out, const std::string_view value) {
  out += '"';
  for (const char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[7];
          std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(c));
          out += escape;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void appendPercentage(std::string& out, const float percentage) {
  float clamped = percentage;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > 1.0f) clamped = 1.0f;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(clamped));
  out += buffer;
}

std::string progressGetPath(const std::string_view digest) {
  if (digest.empty()) return {};
  std::string path = "/koreader/syncs/progress/";
  path.append(digest);
  return path;
}

std::string encodePutProgress(const ProgressRecord& record) {
  if (record.document.empty() || record.progress.empty()) return {};

  std::string body;
  body.reserve(256);
  body += "{\"document\":";
  appendJsonString(body, record.document);
  body += ",\"percentage\":";
  appendPercentage(body, record.percentage);
  body += ",\"progress\":";
  appendJsonString(body, record.progress);
  body += ",\"device\":";
  appendJsonString(body, record.device);
  body += ",\"device_id\":";
  appendJsonString(body, record.deviceId);
  body += ",\"timestamp\":";
  body += std::to_string(record.timestamp);
  body += '}';
  return body;
}

bool decodeProgressResponse(const std::string_view json, ProgressRecord& out) {
  out = ProgressRecord{};

  DecodeCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  // json.data() is not null-terminated, but feed() takes an explicit length.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out = ProgressRecord{};
    return false;
  }
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitProgress --output-on-failure
```

Expected: 13 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitProgress.h lib/BookOrbit/BookOrbitProgress.cpp test/bookorbit_progress test/CMakeLists.txt
git commit -m "feat: add BookOrbit progress GET and PUT codecs"
```

---

### Task 7: Bulk progress codec

**Files:**
- Create: `lib/BookOrbit/BookOrbitBulkProgress.h`, `lib/BookOrbit/BookOrbitBulkProgress.cpp`
- Create: `test/bookorbit_bulk_progress/CMakeLists.txt`, `test/bookorbit_bulk_progress/BookOrbitBulkProgressTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `appendJsonString`, `appendPercentage` (Task 6); `StreamingJsonParser`.
- Produces:
  `inline constexpr size_t bookorbit::kBulkProgressBatchSize = 100`;
  `struct bookorbit::BulkProgressItem { std::string hash; float percentage; std::string progress; uint32_t timestamp; }`;
  `std::string bookorbit::encodeBulkProgress(const std::vector<BulkProgressItem>& items, size_t offset, size_t& consumed)`;
  `bool bookorbit::decodeBulkProgressResponse(std::string_view json, std::vector<std::string>& unmatched)`.

Body is `{"items":[{hash, percentage, progress, timestamp}]}`; the four device
fields (`deviceId`, `deviceModel`, `pluginVersion`, `deviceTime`) are injected
by `BookOrbitClient`'s `withDevice` path, not here. Response is
`{"unmatched":[hash]}`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_bulk_progress/BookOrbitBulkProgressTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBulkProgress.h"

using bookorbit::BulkProgressItem;
using bookorbit::decodeBulkProgressResponse;
using bookorbit::encodeBulkProgress;
using bookorbit::kBulkProgressBatchSize;

namespace {

BulkProgressItem item(const char* hash, const float percentage, const char* progress, const uint32_t timestamp) {
  BulkProgressItem value;
  value.hash = hash;
  value.percentage = percentage;
  value.progress = progress;
  value.timestamp = timestamp;
  return value;
}

std::vector<BulkProgressItem> many(const size_t count) {
  std::vector<BulkProgressItem> items;
  items.reserve(count);
  for (size_t i = 0; i < count; i++) {
    items.push_back(item("d18e399f0f79f24d68a8f70b76d59914", 0.5f, "/body[1]/DocFragment[1]/body[1]/p[1]", 1u));
  }
  return items;
}

}  // namespace

TEST(BookOrbitBulkProgress, BatchSizeIsOneHundred) { EXPECT_EQ(kBulkProgressBatchSize, 100u); }

TEST(BookOrbitBulkProgress, EncodesOneItem) {
  const std::vector<BulkProgressItem> items = {
      item("d18e399f0f79f24d68a8f70b76d59914", 0.001859f, "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0",
           1787561453u)};
  size_t consumed = 0;
  EXPECT_EQ(encodeBulkProgress(items, 0, consumed),
            "{\"items\":[{\"hash\":\"d18e399f0f79f24d68a8f70b76d59914\","
            "\"percentage\":0.001859,"
            "\"progress\":\"/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0\","
            "\"timestamp\":1787561453}]}");
  EXPECT_EQ(consumed, 1u);
}

TEST(BookOrbitBulkProgress, SeparatesItemsWithCommas) {
  const std::vector<BulkProgressItem> items = {item("aa", 0.0f, "/body[1]/DocFragment[1]/body[1]", 1u),
                                               item("bb", 1.0f, "/body[1]/DocFragment[2]/body[1]", 2u)};
  size_t consumed = 0;
  const std::string body = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, 2u);
  EXPECT_NE(body.find("},{"), std::string::npos);
}

TEST(BookOrbitBulkProgress, StopsAtTheBatchLimit) {
  const auto items = many(250);
  size_t consumed = 0;
  const std::string first = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, kBulkProgressBatchSize);
  EXPECT_FALSE(first.empty());

  size_t second = 0;
  encodeBulkProgress(items, kBulkProgressBatchSize, second);
  EXPECT_EQ(second, kBulkProgressBatchSize);

  size_t third = 0;
  encodeBulkProgress(items, 2 * kBulkProgressBatchSize, third);
  EXPECT_EQ(third, 50u);
}

TEST(BookOrbitBulkProgress, OffsetPastTheEndEncodesNothing) {
  const auto items = many(3);
  size_t consumed = 7;
  EXPECT_EQ(encodeBulkProgress(items, 3, consumed), "");
  EXPECT_EQ(consumed, 0u);
}

// Never degrade silently: items missing an xpointer are dropped rather than
// uploaded percentage-only, and the caller learns via consumed vs emitted.
TEST(BookOrbitBulkProgress, SkipsItemsWithoutAnXpointer) {
  std::vector<BulkProgressItem> items = many(2);
  items[0].progress.clear();
  size_t consumed = 0;
  const std::string body = encodeBulkProgress(items, 0, consumed);
  EXPECT_EQ(consumed, 2u);
  EXPECT_EQ(body.find("},{"), std::string::npos);
}

TEST(BookOrbitBulkProgress, AllItemsSkippedEncodesNothing) {
  std::vector<BulkProgressItem> items = many(2);
  items[0].progress.clear();
  items[1].progress.clear();
  size_t consumed = 0;
  EXPECT_EQ(encodeBulkProgress(items, 0, consumed), "");
  EXPECT_EQ(consumed, 2u);
}

TEST(BookOrbitBulkProgress, NormalizesUnindexedXpointersOnEncode) {
  const std::vector<BulkProgressItem> items = {item("aa", 0.5f, "/body/DocFragment[2]/body/p[3]", 1u)};
  size_t consumed = 0;
  EXPECT_NE(encodeBulkProgress(items, 0, consumed).find("\"/body[1]/DocFragment[2]/body[1]/p[3]\""),
            std::string::npos);
}

TEST(BookOrbitBulkProgress, DecodesUnmatchedHashes) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"unmatched\":[\"aa\",\"bb\"]}", unmatched));
  ASSERT_EQ(unmatched.size(), 2u);
  EXPECT_EQ(unmatched[0], "aa");
  EXPECT_EQ(unmatched[1], "bb");
}

TEST(BookOrbitBulkProgress, DecodesAnEmptyUnmatchedArray) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"unmatched\":[]}", unmatched));
  EXPECT_TRUE(unmatched.empty());
}

TEST(BookOrbitBulkProgress, IgnoresUnrelatedArrays) {
  std::vector<std::string> unmatched;
  ASSERT_TRUE(decodeBulkProgressResponse("{\"accepted\":[\"cc\"],\"unmatched\":[\"aa\"]}", unmatched));
  ASSERT_EQ(unmatched.size(), 1u);
  EXPECT_EQ(unmatched[0], "aa");
}

TEST(BookOrbitBulkProgress, MalformedJsonIsRejected) {
  std::vector<std::string> unmatched;
  EXPECT_FALSE(decodeBulkProgressResponse("{\"unmatched\":[", unmatched));
  EXPECT_TRUE(unmatched.empty());
}
```

Create `test/bookorbit_bulk_progress/CMakeLists.txt`:

```cmake
add_executable(BookOrbitBulkProgressTest
  BookOrbitBulkProgressTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitBulkProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitBulkProgressTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitBulkProgressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitBulkProgressTest)
```

Add `add_subdirectory(bookorbit_bulk_progress)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/BookOrbitBulkProgress.cpp`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitBulkProgress.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// POST /koreader/plugin/progress accepts 100 items per request.
inline constexpr size_t kBulkProgressBatchSize = 100;

struct BulkProgressItem {
  std::string hash;         // partial MD5
  float percentage = 0.0f;  // 0..1
  std::string progress;     // canonical crengine xpointer
  uint32_t timestamp = 0;   // unix epoch
};

// Encodes one batch starting at offset, consuming at most kBulkProgressBatchSize
// items. consumed reports how many input items the batch covered, including any
// skipped for want of an xpointer, so the caller can advance without looping.
// Returns "" when nothing was emitted.
//
// The four device fields required on every /koreader/plugin/* POST are injected
// by BookOrbitClient's withDevice path, not here.
std::string encodeBulkProgress(const std::vector<BulkProgressItem>& items, size_t offset, size_t& consumed);

// Decodes {"unmatched":[hash]}. A hash listed here is not in the server's
// library, so its book must not advance any watermark.
bool decodeBulkProgressResponse(std::string_view json, std::vector<std::string>& unmatched);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitBulkProgress.cpp`:

```cpp
#include "BookOrbitBulkProgress.h"

#include "BookOrbitProgress.h"
#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct UnmatchedCtx {
  std::vector<std::string>* out = nullptr;
  std::string key;
  bool inUnmatched = false;
};

void onKey(void* raw, const char* key, const size_t len) {
  static_cast<UnmatchedCtx*>(raw)->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  if (ctx->inUnmatched) ctx->out->emplace_back(value, len);
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  if (ctx->key == "unmatched") ctx->inUnmatched = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  ctx->inUnmatched = false;
  ctx->key.clear();
}

void onNumber(void*, const char*, size_t) {}
void onObjectStart(void*) {}
void onObjectEnd(void* raw) { static_cast<UnmatchedCtx*>(raw)->key.clear(); }
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

std::string encodeBulkProgress(const std::vector<BulkProgressItem>& items, const size_t offset, size_t& consumed) {
  consumed = 0;
  if (offset >= items.size()) return {};

  const size_t end = (items.size() - offset < kBulkProgressBatchSize) ? items.size() : offset + kBulkProgressBatchSize;

  std::string body;
  body.reserve(128 * (end - offset));
  body += "{\"items\":[";

  size_t emitted = 0;
  for (size_t i = offset; i < end; i++) {
    consumed++;
    const auto& value = items[i];
    // An item with no xpointer would be a percentage-only upload. Skip it
    // rather than degrade the server's record silently.
    if (value.hash.empty() || value.progress.empty()) continue;

    if (emitted > 0) body += ',';
    body += "{\"hash\":";
    appendJsonString(body, value.hash);
    body += ",\"percentage\":";
    appendPercentage(body, value.percentage);
    body += ",\"progress\":";
    const std::string normalized = normalizeXPointer(value.progress);
    appendJsonString(body, normalized.empty() ? value.progress : normalized);
    body += ",\"timestamp\":";
    body += std::to_string(value.timestamp);
    body += '}';
    emitted++;
  }

  if (emitted == 0) return {};
  body += "]}";
  return body;
}

bool decodeBulkProgressResponse(const std::string_view json, std::vector<std::string>& unmatched) {
  unmatched.clear();

  UnmatchedCtx ctx;
  ctx.out = &unmatched;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    unmatched.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitBulkProgress --output-on-failure
```

Expected: 12 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitBulkProgress.h lib/BookOrbit/BookOrbitBulkProgress.cpp test/bookorbit_bulk_progress test/CMakeLists.txt
git commit -m "feat: add BookOrbit bulk progress codec"
```

---

### Task 8: The "never degrade silently" policy

**Files:**
- Create: `lib/BookOrbit/ProgressResolution.h`, `lib/BookOrbit/ProgressResolution.cpp`
- Create: `test/bookorbit_progress_resolution/CMakeLists.txt`, `test/bookorbit_progress_resolution/ProgressResolutionTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::ProgressRecord` (Task 6), `bookorbit::normalizeXPointer` (Task 2).
- Produces:
  `enum class bookorbit::ProgressSource : uint8_t { None, Xpointer, Percentage }`;
  `struct bookorbit::ResolvedProgress { ProgressSource source; bool degraded; bool jumpNeedsNotice; float percentage; std::string xpointer; }`;
  `inline constexpr float bookorbit::kDegradedJumpThreshold = 0.02f`;
  `bool bookorbit::isSendable(const ProgressRecord& record)`;
  `ResolvedProgress bookorbit::chooseRemoteProgress(const ProgressRecord& remote, bool xpointerResolved, float resolvedPercentage, float localPercentage)`.

**Threshold:** the spec says "surface that in the UI when the resulting jump
exceeds a threshold" without naming a number. `kDegradedJumpThreshold = 0.02f`
— 2% of the book — is this plan's choice: below it a percentage landing is
within a couple of pages and a banner would be noise; above it the reader is
being moved somewhere they did not choose and must be told.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_progress_resolution/ProgressResolutionTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitProgress.h"
#include "lib/BookOrbit/ProgressResolution.h"

using bookorbit::chooseRemoteProgress;
using bookorbit::isSendable;
using bookorbit::kDegradedJumpThreshold;
using bookorbit::ProgressRecord;
using bookorbit::ProgressSource;

namespace {

ProgressRecord remote(const char* progress, const float percentage) {
  ProgressRecord record;
  record.document = "d18e399f0f79f24d68a8f70b76d59914";
  record.progress = progress;
  record.percentage = percentage;
  record.device = "KOReader";
  record.deviceId = "9F8E7D";
  record.timestamp = 1787407272u;
  return record;
}

}  // namespace

TEST(ProgressResolution, ThresholdIsTwoPercent) { EXPECT_FLOAT_EQ(kDegradedJumpThreshold, 0.02f); }

// Outbound: both fields or nothing.
TEST(ProgressResolution, SendableRequiresBothProgressAndPercentage) {
  EXPECT_TRUE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", 0.5f)));
  EXPECT_FALSE(isSendable(remote("", 0.5f)));
}

TEST(ProgressResolution, SendableRejectsOutOfRangePercentage) {
  EXPECT_FALSE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", 1.5f)));
  EXPECT_FALSE(isSendable(remote("/body[1]/DocFragment[1]/body[1]/p[1]", -0.1f)));
}

TEST(ProgressResolution, SendableRejectsAnUnparseableXpointer) {
  EXPECT_FALSE(isSendable(remote("not-an-xpointer", 0.5f)));
}

TEST(ProgressResolution, SendableRejectsAMissingDocument) {
  ProgressRecord record = remote("/body[1]/DocFragment[1]/body[1]/p[1]", 0.5f);
  record.document.clear();
  EXPECT_FALSE(isSendable(record));
}

// Inbound: the xpointer wins whenever it resolved.
TEST(ProgressResolution, PrefersTheXpointerWhenItResolves) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.2052f), true, 0.2049f,
                                           0.1000f);
  EXPECT_EQ(chosen.source, ProgressSource::Xpointer);
  EXPECT_FALSE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
  EXPECT_FLOAT_EQ(chosen.percentage, 0.2049f);
  EXPECT_EQ(chosen.xpointer, "/body[1]/DocFragment[8]/body[1]/p[4]");
}

// A big jump from a *resolved* xpointer is a real position, not a degradation.
TEST(ProgressResolution, LargeJumpFromAResolvedXpointerNeedsNoNotice) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.90f), true, 0.90f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::Xpointer);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, FallsBackToPercentageWhenResolutionFails) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.2052f), false, 0.0f,
                                           0.2000f);
  EXPECT_EQ(chosen.source, ProgressSource::Percentage);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FLOAT_EQ(chosen.percentage, 0.2052f);
}

// The case the UI must surface: degraded AND the reader moves noticeably.
TEST(ProgressResolution, DegradedJumpBeyondThresholdNeedsNotice) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.60f), false, 0.0f, 0.10f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, DegradedJumpInsideThresholdIsSilent) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.205f), false, 0.0f,
                                           0.200f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, ThresholdIsExclusiveAtExactlyTwoPercent) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.22f), false, 0.0f, 0.20f);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, BackwardJumpsCountToo) {
  const auto chosen = chooseRemoteProgress(remote("/body[1]/DocFragment[8]/body[1]/p[4]", 0.10f), false, 0.0f, 0.60f);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

// No xpointer at all is not a degradation of ours — it is a percentage-only
// peer — but it still flags, because the landing is still approximate.
TEST(ProgressResolution, PercentageOnlyRemoteIsFlaggedDegraded) {
  const auto chosen = chooseRemoteProgress(remote("", 0.60f), false, 0.0f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::Percentage);
  EXPECT_TRUE(chosen.degraded);
  EXPECT_TRUE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, EmptyRemoteResolvesToNothing) {
  const auto chosen = chooseRemoteProgress(remote("", 0.0f), false, 0.0f, 0.10f);
  EXPECT_EQ(chosen.source, ProgressSource::None);
  EXPECT_FALSE(chosen.jumpNeedsNotice);
}

TEST(ProgressResolution, NormalizesTheChosenXpointer) {
  const auto chosen = chooseRemoteProgress(remote("/body/DocFragment[2]/body/p[3]", 0.3f), true, 0.3f, 0.3f);
  EXPECT_EQ(chosen.xpointer, "/body[1]/DocFragment[2]/body[1]/p[3]");
}
```

Create `test/bookorbit_progress_resolution/CMakeLists.txt`:

```cmake
add_executable(BookOrbitProgressResolutionTest
  ProgressResolutionTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/ProgressResolution.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitProgressResolutionTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitProgressResolutionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitProgressResolutionTest)
```

Add `add_subdirectory(bookorbit_progress_resolution)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/ProgressResolution.cpp`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/ProgressResolution.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>

#include "BookOrbitProgress.h"

namespace bookorbit {

// A percentage landing this far from where the reader was is a jump they did
// not choose, so it is surfaced. Below it the landing is within a page or two
// and a banner would be noise. The spec requires a threshold but names no
// number; 2% of the book is this plan's choice.
inline constexpr float kDegradedJumpThreshold = 0.02f;

enum class ProgressSource : uint8_t {
  None,        // nothing usable arrived
  Xpointer,    // exact position, resolved locally
  Percentage,  // approximate landing
};

struct ResolvedProgress {
  ProgressSource source = ProgressSource::None;
  bool degraded = false;         // an exact position was wanted but not obtained
  bool jumpNeedsNotice = false;  // degraded AND the reader moves more than the threshold
  float percentage = 0.0f;
  std::string xpointer;
};

// Outbound guard. A record is sendable only when it carries BOTH a parseable
// xpointer and an in-range percentage. Half a record is silent degradation.
bool isSendable(const ProgressRecord& record);

// Inbound choice. xpointerResolved says whether the local resolver turned
// remote.progress into a real position, and resolvedPercentage is where that
// position sits; localPercentage is where the reader currently is.
ResolvedProgress chooseRemoteProgress(const ProgressRecord& remote, bool xpointerResolved, float resolvedPercentage,
                                      float localPercentage);

}  // namespace bookorbit
```

Create `lib/BookOrbit/ProgressResolution.cpp`:

```cpp
#include "ProgressResolution.h"

#include <cmath>

#include "XPointer.h"

namespace bookorbit {

bool isSendable(const ProgressRecord& record) {
  if (record.document.empty()) return false;
  if (record.percentage < 0.0f || record.percentage > 1.0f) return false;
  return !normalizeXPointer(record.progress).empty();
}

ResolvedProgress chooseRemoteProgress(const ProgressRecord& remote, const bool xpointerResolved,
                                      const float resolvedPercentage, const float localPercentage) {
  ResolvedProgress result;

  const std::string normalized = normalizeXPointer(remote.progress);

  if (xpointerResolved && !normalized.empty()) {
    result.source = ProgressSource::Xpointer;
    result.xpointer = normalized;
    result.percentage = resolvedPercentage;
    // An exact position is never a degradation, however far it moves.
    return result;
  }

  if (remote.percentage <= 0.0f && remote.timestamp == 0u) {
    return result;  // nothing usable arrived
  }

  result.source = ProgressSource::Percentage;
  result.degraded = true;
  result.percentage = remote.percentage;
  result.xpointer = normalized;  // kept for logging even though it did not resolve
  result.jumpNeedsNotice = std::fabs(remote.percentage - localPercentage) > kDegradedJumpThreshold;
  return result;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitProgressResolution --output-on-failure
```

Expected: 15 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/ProgressResolution.h lib/BookOrbit/ProgressResolution.cpp test/bookorbit_progress_resolution test/CMakeLists.txt
git commit -m "feat: add never-degrade-silently progress resolution policy"
```

---

### Task 9: Device wiring and the approximate-position notice

This is the only task that touches `src/`. It has no host test — the policy it
enforces is fully covered by Task 8, and the resolver by Task 4. Verification is
on device and through the simulator smoke test.

**Files:**
- Create: `src/network/BookOrbitProgressPhase.h`, `src/network/BookOrbitProgressPhase.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `lib/I18n/translations/en.yaml` (then regenerate)
- Modify: `CHANGELOG.md`
- Reference: `lib/KOReaderSync/ProgressMapper.h` (`CrossPointPosition`, `KOReaderPosition`), `lib/Epub/Epub.h:165` (`readItemContentsToStream`), `lib/Epub/Epub.h:174` (`getSpineItem`)

**Interfaces:**
- Consumes: `bookorbit::ProgressRecord`, `encodePutProgress`, `decodeProgressResponse`, `progressGetPath` (Task 6); `encodeBulkProgress`, `decodeBulkProgressResponse` (Task 7); `isSendable`, `chooseRemoteProgress` (Task 8); `resolveXPointerToOffset`, `resolveOffsetToXPointer` (Task 4); `BookOrbitClient`, `SyncStateStore` (P0).
- Produces:
  `class BookOrbitProgressPhase` with
  `bool push(const std::string& md5, const CrossPointPosition& position, float percentage)`,
  `bool pull(const std::string& md5, float localPercentage, bookorbit::ResolvedProgress& out)`,
  `bool pushBulk(const std::vector<bookorbit::BulkProgressItem>& items, std::vector<std::string>& unmatched)`.

- [ ] **Step 1: Add the translation key**

Add to `lib/I18n/translations/en.yaml`:

```yaml
STR_BOOKORBIT_PROGRESS_APPROXIMATE: "Synced to an approximate position"
STR_BOOKORBIT_PROGRESS_APPROXIMATE_DETAIL: "The exact position from your other device could not be found in this copy of the book."
```

Regenerate:

```bash
python3 scripts/gen_i18n.py
```

Do not hand-edit `lib/I18n/I18nKeys.h` or `I18nStrings.{h,cpp}` — they are generated.

- [ ] **Step 2: Write the phase**

The push direction. Both fields, always:

```cpp
bool BookOrbitProgressPhase::push(const std::string& md5, const CrossPointPosition& position, const float percentage) {
  bookorbit::ProgressRecord record;
  record.document = md5;
  record.percentage = percentage;
  record.progress = ProgressMapper::toKOReader(epub, position).xpath;
  record.device = deviceName;
  record.deviceId = deviceId;
  record.timestamp = nowUnix;

  // Never degrade silently: refuse to push half a record rather than write a
  // percentage-only row a KOReader client would then treat as authoritative.
  if (!bookorbit::isSendable(record)) {
    LOG_ERR("BOP", "refusing incomplete progress push for %s", md5.c_str());
    return false;
  }

  const std::string body = bookorbit::encodePutProgress(record);
  if (body.empty() || body.size() > bookorbit::MAX_BODY_BYTES) {
    LOG_ERR("BOP", "progress body rejected (%u bytes)", static_cast<unsigned>(body.size()));
    return false;
  }
  return client.put("/koreader/syncs/progress", body).status == bookorbit::Status::Ok;
}
```

The pull direction. Resolve the xpointer against the real spine XHTML, and only
then decide:

```cpp
bool BookOrbitProgressPhase::pull(const std::string& md5, const float localPercentage,
                                  bookorbit::ResolvedProgress& out) {
  const auto response = client.get(bookorbit::progressGetPath(md5));
  if (response.error.status != bookorbit::Status::Ok) return false;

  bookorbit::ProgressRecord remote;
  if (!bookorbit::decodeProgressResponse(response.body, remote)) return false;

  bookorbit::XPointer target;
  bool resolved = false;
  float resolvedPercentage = 0.0f;
  if (bookorbit::parseXPointer(remote.progress, target)) {
    const int spineIndex = target.docFragment - 1;
    std::string xhtml;
    if (readSpineItem(spineIndex, xhtml)) {
      uint32_t offset = 0;
      uint32_t length = 0;
      if (bookorbit::resolveXPointerToOffset(xhtml, target, offset) &&
          bookorbit::visibleTextLength(xhtml, length) && length > 0) {
        resolved = true;
        resolvedPercentage = spinePercentage(spineIndex, static_cast<float>(offset) / static_cast<float>(length));
      }
    }
  }

  out = bookorbit::chooseRemoteProgress(remote, resolved, resolvedPercentage, localPercentage);
  return out.source != bookorbit::ProgressSource::None;
}
```

`readSpineItem` streams the item through `Epub::readItemContentsToStream` into a
`std::string` sink. Spine XHTML runs to tens of kilobytes, so it is a heap
buffer, not a stack one, and it is released as soon as resolution finishes.

- [ ] **Step 3: Surface the degraded jump in the reader**

In `EpubReaderActivity.cpp`, where an inbound sync position is applied, add:

```cpp
  if (resolved.jumpNeedsNotice) {
    // Approach A's whole point: a percentage landing is never applied silently.
    showToast(tr(STR_BOOKORBIT_PROGRESS_APPROXIMATE));
    LOG_INF("BOP", "degraded landing: remote %.4f local %.4f", static_cast<double>(resolved.percentage),
            static_cast<double>(localPercentage));
  }
```

All user-facing text goes through `tr(STR_*)`; the log line stays hardcoded.

- [ ] **Step 4: Verify it builds and runs**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
pio run -e x4-pro && pio run -e default && pio run -e sticky
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

Expected: whole suite green, all targets link, `scripts/check_firmware_size.py` passes, smoke test passes with no crash.

- [ ] **Step 5: Add the changelog entry and commit**

Under `### Fixed` in `CHANGELOG.md`:

```markdown
- KOReader reading positions now sync at exact-position fidelity. Positions written by KOReader use fully-indexed crengine paths, which the previous matcher never recognised, so every sync silently fell back to a percentage estimate.
```

Under `### Added`:

```markdown
- BookOrbit progress sync: two-way reading position, with a notice when an exact position cannot be found and the reader is moved to an approximate one.
```

```bash
git add src/network/BookOrbitProgressPhase.h src/network/BookOrbitProgressPhase.cpp src/activities/reader/EpubReaderActivity.cpp lib/I18n/translations/en.yaml lib/I18n CHANGELOG.md
git commit -m "feat: sync BookOrbit reading position at xpointer fidelity"
```

---

## FOLLOW-ON — Approach B: server-side native position

**These two tasks are blocked on a BookOrbit server change** and must not be
started until that change is deployed. The spec is explicit that Approach A
above is the *required* deliverable and Approach B is the follow-on; Approach A
is complete and useful without either task below.

The server change: accept and return an optional `position` object on the
progress record —

```json
"position": {"pctQ": 0, "spine": 0, "page": 0, "pages": 1, "para": 0, "xpath": "…"}
```

Strictly additive, no migration. KOReader clients ignore the field and keep
using the xpointer; CrossInk↔CrossInk becomes lossless. CrossInk already has the
exact struct for it: `KOReaderRichPosition` (`lib/KOReaderSync/KOReaderSyncClient.h:23-30`,
fields `pctQ`, `spineIndex`, `pageNumber`, `totalPages`, `paragraphIndex`, `xpath`),
consumed by `ProgressMapper::fromRichPosition` (`ProgressMapper.h:88`).

---

### FOLLOW-ON Task 10: Native position blob codec

**Files:**
- Create: `lib/BookOrbit/NativePosition.h`, `lib/BookOrbit/NativePosition.cpp`
- Create: `test/bookorbit_native_position/CMakeLists.txt`, `test/bookorbit_native_position/NativePositionTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `appendJsonString`, `appendPercentage` (Task 6); `StreamingJsonParser`; `normalizeXPointer` (Task 2).
- Produces:
  `struct bookorbit::NativePosition { uint32_t pctQ; uint16_t spine; uint16_t page; uint16_t pages; uint16_t para; std::string xpath; bool present; }`;
  `void bookorbit::appendNativePosition(std::string& out, const NativePosition& position)`;
  `bool bookorbit::decodeNativePosition(std::string_view json, NativePosition& out)`.

`pctQ` is the percentage quantized to 0..1,000,000, matching
`KOReaderRichPosition::pctQ`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_native_position/NativePositionTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>

#include "lib/BookOrbit/NativePosition.h"

using bookorbit::appendNativePosition;
using bookorbit::decodeNativePosition;
using bookorbit::NativePosition;

namespace {

NativePosition sample() {
  NativePosition position;
  position.pctQ = 205200u;
  position.spine = 7u;
  position.page = 3u;
  position.pages = 12u;
  position.para = 42u;
  position.xpath = "/body[1]/DocFragment[8]/body[1]/p[42]";
  position.present = true;
  return position;
}

}  // namespace

TEST(NativePosition, EncodesEveryField) {
  std::string out;
  appendNativePosition(out, sample());
  EXPECT_EQ(out,
            "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
            "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}");
}

TEST(NativePosition, AbsentPositionEncodesNothing) {
  NativePosition absent;
  std::string out;
  appendNativePosition(out, absent);
  EXPECT_EQ(out, "");
}

TEST(NativePosition, PagesNeverEncodesZero) {
  NativePosition zeroPages = sample();
  zeroPages.pages = 0u;
  std::string out;
  appendNativePosition(out, zeroPages);
  EXPECT_NE(out.find("\"pages\":1"), std::string::npos);
}

TEST(NativePosition, NormalizesTheXpathOnEncode) {
  NativePosition unindexed = sample();
  unindexed.xpath = "/body/DocFragment[8]/body/p[42]";
  std::string out;
  appendNativePosition(out, unindexed);
  EXPECT_NE(out.find("\"/body[1]/DocFragment[8]/body[1]/p[42]\""), std::string::npos);
}

TEST(NativePosition, DecodesEveryField) {
  const char* json =
      "{\"percentage\":0.2052,\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[42]\","
      "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
      "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}}";

  NativePosition out;
  ASSERT_TRUE(decodeNativePosition(json, out));
  EXPECT_TRUE(out.present);
  EXPECT_EQ(out.pctQ, 205200u);
  EXPECT_EQ(out.spine, 7u);
  EXPECT_EQ(out.page, 3u);
  EXPECT_EQ(out.pages, 12u);
  EXPECT_EQ(out.para, 42u);
  EXPECT_EQ(out.xpath, "/body[1]/DocFragment[8]/body[1]/p[42]");
}

// A server without the change simply omits the field. That is not an error.
TEST(NativePosition, MissingPositionDecodesAsAbsent) {
  NativePosition out;
  ASSERT_TRUE(decodeNativePosition("{\"percentage\":0.5,\"progress\":\"/body[1]/DocFragment[1]/body[1]\"}", out));
  EXPECT_FALSE(out.present);
}

TEST(NativePosition, TopLevelKeysNamedLikePositionKeysAreIgnored) {
  NativePosition out;
  ASSERT_TRUE(decodeNativePosition("{\"page\":99,\"spine\":99}", out));
  EXPECT_FALSE(out.present);
  EXPECT_EQ(out.page, 0u);
}

TEST(NativePosition, MalformedJsonIsRejected) {
  NativePosition out;
  EXPECT_FALSE(decodeNativePosition("{\"position\":{", out));
}

TEST(NativePosition, EncodeDecodeRoundTrip) {
  std::string body = "{\"percentage\":0.2052,";
  appendNativePosition(body, sample());
  body += '}';

  NativePosition out;
  ASSERT_TRUE(decodeNativePosition(body, out));
  EXPECT_EQ(out.pctQ, sample().pctQ);
  EXPECT_EQ(out.spine, sample().spine);
  EXPECT_EQ(out.para, sample().para);
  EXPECT_EQ(out.xpath, sample().xpath);
}
```

Create `test/bookorbit_native_position/CMakeLists.txt`:

```cmake
add_executable(BookOrbitNativePositionTest
  NativePositionTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/NativePosition.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitProgress.cpp
  ${REPO_ROOT}/lib/BookOrbit/XPointer.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitNativePositionTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitNativePositionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitNativePositionTest)
```

Add `add_subdirectory(bookorbit_native_position)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `Cannot find source file: .../lib/BookOrbit/NativePosition.cpp`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/NativePosition.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace bookorbit {

// Approach B's optional device-position blob. Mirrors KOReaderRichPosition
// (lib/KOReaderSync/KOReaderSyncClient.h:23-30) field for field, so
// ProgressMapper::fromRichPosition consumes it unchanged.
//
// KOReader clients ignore this object and use the xpointer, so adding it is
// strictly additive and needs no migration.
struct NativePosition {
  uint32_t pctQ = 0;    // percentage quantized to 0..1,000,000
  uint16_t spine = 0;
  uint16_t page = 0;
  uint16_t pages = 1;
  uint16_t para = 0;
  std::string xpath;
  bool present = false;
};

// Appends "position":{...} to a body already under construction. Appends
// nothing when the position is absent.
void appendNativePosition(std::string& out, const NativePosition& position);

// Reads the position object out of a progress response. A response without one
// decodes successfully with present == false.
bool decodeNativePosition(std::string_view json, NativePosition& out);

}  // namespace bookorbit
```

Create `lib/BookOrbit/NativePosition.cpp`:

```cpp
#include "NativePosition.h"

#include <cstdlib>

#include "BookOrbitProgress.h"
#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct PositionCtx {
  NativePosition* out = nullptr;
  std::string key;
  int depth = 0;
  int positionDepth = -1;
};

bool inPosition(const PositionCtx* ctx) { return ctx->positionDepth >= 0 && ctx->depth == ctx->positionDepth; }

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  ctx->key.assign(key, len);
  if (ctx->key == "position" && ctx->depth == 1) {
    ctx->positionDepth = 2;  // the object that opens next
  }
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (!inPosition(ctx) || ctx->key != "xpath") return;
  ctx->out->xpath.assign(value, len);
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (!inPosition(ctx)) return;
  const std::string text(value, len);
  const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10);
  if (ctx->key == "pctQ") ctx->out->pctQ = static_cast<uint32_t>(parsed);
  else if (ctx->key == "spine") ctx->out->spine = static_cast<uint16_t>(parsed);
  else if (ctx->key == "page") ctx->out->page = static_cast<uint16_t>(parsed);
  else if (ctx->key == "pages") ctx->out->pages = static_cast<uint16_t>(parsed);
  else if (ctx->key == "para") ctx->out->para = static_cast<uint16_t>(parsed);
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  ctx->depth++;
  if (ctx->depth == ctx->positionDepth) ctx->out->present = true;
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (ctx->depth == ctx->positionDepth) ctx->positionDepth = -1;
  ctx->depth--;
  ctx->key.clear();
}

void onArrayStart(void*) {}
void onArrayEnd(void*) {}
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

void appendNativePosition(std::string& out, const NativePosition& position) {
  if (!position.present) return;

  out += "\"position\":{\"pctQ\":";
  out += std::to_string(position.pctQ);
  out += ",\"spine\":";
  out += std::to_string(position.spine);
  out += ",\"page\":";
  out += std::to_string(position.page);
  out += ",\"pages\":";
  out += std::to_string(position.pages > 0 ? position.pages : 1);
  out += ",\"para\":";
  out += std::to_string(position.para);
  out += ",\"xpath\":";
  const std::string normalized = normalizeXPointer(position.xpath);
  appendJsonString(out, normalized.empty() ? position.xpath : normalized);
  out += '}';
}

bool decodeNativePosition(const std::string_view json, NativePosition& out) {
  out = NativePosition{};

  PositionCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out = NativePosition{};
    return false;
  }
  return true;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitNativePosition --output-on-failure
```

Expected: 9 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/NativePosition.h lib/BookOrbit/NativePosition.cpp test/bookorbit_native_position test/CMakeLists.txt
git commit -m "feat: add optional native position blob codec"
```

---

### FOLLOW-ON Task 11: Wire the native position through the progress phase

**Files:**
- Modify: `lib/BookOrbit/BookOrbitProgress.h`, `lib/BookOrbit/BookOrbitProgress.cpp`
- Modify: `src/network/BookOrbitProgressPhase.cpp`
- Modify: `test/bookorbit_progress/BookOrbitProgressTest.cpp`, `test/bookorbit_progress/CMakeLists.txt`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: `bookorbit::NativePosition`, `appendNativePosition`, `decodeNativePosition` (Task 10); `ProgressMapper::fromRichPosition` (`ProgressMapper.h:88`); `CapabilityCache` (P0 Task 3).
- Produces: `ProgressRecord::position` (a `NativePosition` member) carried through `encodePutProgress` and `decodeProgressResponse`.

Gated on the capability name `"nativePosition"`. Tri-state applies: only a
`Capability::Supported` answer turns the field on, so a server that has not
taken the change — or a blip that left the capability `Unknown` — simply keeps
sending Approach A's two fields.

- [ ] **Step 1: Write the failing test**

Append to `test/bookorbit_progress/BookOrbitProgressTest.cpp`:

```cpp
#include "lib/BookOrbit/NativePosition.h"

TEST(BookOrbitProgress, EncodesTheNativePositionWhenPresent) {
  ProgressRecord record = sample();
  record.position.pctQ = 205200u;
  record.position.spine = 7u;
  record.position.page = 3u;
  record.position.pages = 12u;
  record.position.para = 42u;
  record.position.xpath = "/body[1]/DocFragment[8]/body[1]/p[42]";
  record.position.present = true;

  const std::string body = encodePutProgress(record);
  EXPECT_NE(body.find("\"position\":{\"pctQ\":205200"), std::string::npos);
  // The two Approach A fields are still there. Approach B never replaces them.
  EXPECT_NE(body.find("\"progress\":"), std::string::npos);
  EXPECT_NE(body.find("\"percentage\":"), std::string::npos);
}

TEST(BookOrbitProgress, OmitsTheNativePositionWhenAbsent) {
  EXPECT_EQ(encodePutProgress(sample()).find("\"position\""), std::string::npos);
}

TEST(BookOrbitProgress, DecodesTheNativePositionFromAResponse) {
  const char* json =
      "{\"percentage\":0.2052,\"progress\":\"/body[1]/DocFragment[8]/body[1]/p[42]\",\"timestamp\":1,"
      "\"position\":{\"pctQ\":205200,\"spine\":7,\"page\":3,\"pages\":12,\"para\":42,"
      "\"xpath\":\"/body[1]/DocFragment[8]/body[1]/p[42]\"}}";

  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse(json, out));
  EXPECT_TRUE(out.position.present);
  EXPECT_EQ(out.position.spine, 7u);
  EXPECT_EQ(out.position.para, 42u);
}

TEST(BookOrbitProgress, ResponseWithoutAPositionStillDecodes) {
  ProgressRecord out;
  ASSERT_TRUE(decodeProgressResponse("{\"percentage\":0.5,\"progress\":\"/body[1]/DocFragment[1]/body[1]\"}", out));
  EXPECT_FALSE(out.position.present);
}
```

Add the new source to `test/bookorbit_progress/CMakeLists.txt`'s `add_executable` list:

```cmake
  ${REPO_ROOT}/lib/BookOrbit/NativePosition.cpp
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `error: 'struct bookorbit::ProgressRecord' has no member named 'position'`.

- [ ] **Step 3: Write minimal implementation**

In `lib/BookOrbit/BookOrbitProgress.h`, add the include and the member:

```cpp
#include "NativePosition.h"
```

```cpp
  uint32_t timestamp = 0;      // unix epoch
  NativePosition position;     // Approach B; absent unless the server supports it
```

In `lib/BookOrbit/BookOrbitProgress.cpp`, append the position inside
`encodePutProgress`, immediately before the closing brace is added:

```cpp
  body += ",\"timestamp\":";
  body += std::to_string(record.timestamp);
  if (record.position.present) {
    body += ',';
    appendNativePosition(body, record.position);
  }
  body += '}';
```

and decode it in `decodeProgressResponse` with a second pass, which keeps the
two decoders independent and costs one extra scan of a body that is well under
a kilobyte:

```cpp
  // Second pass for the optional Approach B blob. Nested-object state would
  // otherwise complicate the flat top-level decoder above for a field most
  // servers never send.
  decodeNativePosition(json, out.position);
  return true;
```

Add `#include "NativePosition.h"` to the `.cpp` include block.

In `src/network/BookOrbitProgressPhase.cpp`, populate it on push only when the
capability is confirmed, and prefer it on pull:

```cpp
  if (capabilities.get("nativePosition") == bookorbit::Capability::Supported) {
    record.position.pctQ = static_cast<uint32_t>(percentage * 1000000.0f + 0.5f);
    record.position.spine = static_cast<uint16_t>(position.spineIndex);
    record.position.page = static_cast<uint16_t>(position.pageNumber);
    record.position.pages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
    record.position.para = position.paragraphIndex;
    record.position.xpath = record.progress;
    record.position.present = true;
  }
```

```cpp
  // Device-to-device is lossless when the blob survived the round trip.
  if (remote.position.present) {
    KOReaderRichPosition rich;
    rich.pctQ = remote.position.pctQ;
    rich.spineIndex = remote.position.spine;
    rich.pageNumber = remote.position.page;
    rich.totalPages = remote.position.pages;
    rich.paragraphIndex = remote.position.para;
    rich.xpath = remote.position.xpath;
    if (ProgressMapper::fromRichPosition(epub, rich, renderer).has_value()) {
      out.source = bookorbit::ProgressSource::Xpointer;
      out.degraded = false;
      out.jumpNeedsNotice = false;
      out.percentage = static_cast<float>(remote.position.pctQ) / 1000000.0f;
      out.xpointer = remote.position.xpath;
      return true;
    }
  }
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests --output-on-failure
pio run -e x4-pro && pio run -e default && pio run -e sticky && pio run -e simulator
```

Expected: 17 `BookOrbitProgress` tests PASS, whole suite green, all four targets link.

- [ ] **Step 5: Commit**

```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add lib/BookOrbit/BookOrbitProgress.h lib/BookOrbit/BookOrbitProgress.cpp src/network/BookOrbitProgressPhase.cpp test/bookorbit_progress CHANGELOG.md
git commit -m "feat: carry the native position blob through progress sync"
```

---

## Hardware Verification

After Task 9, on an X4 Pro with an SD card and a BookOrbit server configured
(P0 Task 10's settings screen).

The book that matters is one KOReader has actually read, because the whole point
is a position CrossInk did not write itself. Copy an EPUB and its sidecar from
`/home/monish/repos/imprint-dev-books/` onto the card — for example
`We Solve Murders - Richard Osman.epub`, whose sidecar holds
`last_xpointer = "/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0"`,
`partial_md5_checksum = "d18e399f0f79f24d68a8f70b76d59914"` and
`cre_dom_version = 20260812` — the normalized DOM version, so its xpointers are
directly comparable with what the device emits.

1. Open the book, read to roughly 20%, close it. Expect a serial line from the
   progress phase showing a pushed body that contains **both**
   `"progress":"/body[1]/DocFragment[…"` and `"percentage":`. A body with only
   one of the two is the bug this phase exists to prevent.
2. Confirm the pushed xpointer is fully indexed — every step has `[N]`. The old
   emitter wrote `/body/DocFragment[N]/body`; seeing that form means Task 5 did
   not take effect.
3. Confirm the pushed xpointer contains none of `autoBoxing`, `tabularBox`,
   `rubyBox`, `mathBox`, `floatBox`, `inlineBox`, `pseudoElem`. The device
   streams source XHTML and must never invent crengine's synthetic elements.
4. On the server (or in KOReader against the same server), read the same book to
   a different chapter. Back on the X4 Pro, open the book and accept the sync.
   Expect to land in that chapter, with **no** "Synced to an approximate
   position" toast — the exact-position path.
5. Force the degraded path: with WiFi on, open a book whose spine differs from
   the sender's (a different EPUB build of the same title). Expect the toast,
   and a serial line reading `degraded landing: remote … local …`.
6. Check that a jump under 2% produces no toast: sync twice in a row without
   reading in between.
7. Legacy DOM version: in the KOReader emulator set the book's DOM version to
   20171225 (`requestDomVersion`), push from there, then pull on the device.
   Expect a landing in the right element and, where the position sat inside a
   crengine-boxed run of same-name siblings, tolerate landing on the first of
   them — the documented limit of `stripSyntheticSteps`, not a regression.
8. Pull the battery mid-sync. On the next boot expect the progress phase to
   retry from an unadvanced watermark, with no duplicate toast and no crash.
9. Watch internal heap across a sync with `ESP.getFreeHeap()` /
   `ESP.getMaxAllocHeap()`. The resolver holds one spine item's XHTML — tens of
   KB — for the duration of one resolution. On the C3 (`-e default`) confirm the
   largest allocatable block stays healthy; if the spine item is large the
   resolution should fail cleanly to the percentage path, not abort.
10. Clear `.crosspoint/epub_<hash>/` and repeat step 4 to confirm resolution does
    not depend on a warm cache.

## Self-Review Notes

- **Spec coverage.** P2's spec section has six requirements. "The defect to fix
  first" → Tasks 2 and 5 (all four literal-match sites at
  `ProgressMapper.cpp:62,134,192,257` and the emitter at `:1006`, each named and
  replaced). "parse / normalize / emit" → Task 2. "Extend the resolver from
  paragraph granularity to full element-ancestry granularity, tracking same-name
  sibling counts while streaming the spine XHTML" → Task 4. "Round-trip corpus…
  written first and must fail before the fix" → Tasks 1 and 2, now over 404
  generated xpointers rather than a hand-written list, with Task 4's exhaustive
  offset round trip quantifying fidelity. Endpoints → Tasks 6 and 7. "Never
  degrade silently" → Task 8, enforced in both directions and surfaced in
  Task 9. Approach B → FOLLOW-ON Tasks 10 and 11, explicitly blocked on the
  server change the spec names.
- **Beyond the spec: DOM versions.** The spec treats "the crengine xpointer" as
  one format. It is two, and which one you get depends on the document's DOM
  version (`getDomVersionWithNormalizedXPointers()` = 20200223). Generating the
  corpus at both 20171225 and 20260812 surfaced this, and Task 3 handles it
  explicitly rather than leaving it to be discovered in the field.
- **Beyond the spec: synthetic boxing elements.** crengine inserts `autoBoxing`,
  `tabularBox` and their siblings into the DOM, and they appear as xpointer
  steps that do not exist in the source XHTML the resolver streams. Measured: 16
  of 404 corpus rows. Task 3 strips them and records the one case where
  stripping is lossy (`autoBoxing[2]/img[1]` under the old DOM version is
  `img[2]` under the new one), which is the concrete reason the Global
  Constraints say xpointers are only comparable within a DOM version.
- **Ground truth is generated, not synthesized.** An earlier draft of this plan
  carried a hand-written corpus built from the two real sidecar strings plus
  invented variants, because both sidecars on this machine hold the *same*
  `last_xpointer` and empty `annotations`. That limitation is real but no longer
  binding: Task 1 drives the KOReader emulator to produce 404 genuine xpointers
  over 13 books and two DOM versions. The two sidecar strings are kept in
  `GroundTruthCorpus.h` as an independent regression case — they are the only
  ground truth that came off a real device rather than an emulator.
- **No unpacked EPUBs are needed for the corpus.** crengine reads the committed
  `test/epubs/*.epub` zips directly. The spine XHTML the C++ resolver test needs
  is extracted by the generator wrapper into `fixtures/<epub>_frag<N>.xhtml`,
  indexed by DocFragment, so no test depends on `test/epubs-src/`.
- **Placeholder scan.** No "TBD", no "similar to Task N", no "add error
  handling". Every implementation step carries complete code; every test step
  carries a complete test file and a complete `CMakeLists.txt`; every commit
  step carries the exact `git` command and a `<type>: <summary>` message.
  Task 5, Task 9 and FOLLOW-ON Task 11 show real code for each edit site rather
  than full files, because they modify existing files whose surrounding code is
  unchanged — the anchors (`ProgressMapper.cpp:62`, `:134`, `:192`, `:257`,
  `:906`, `:1006`, `ChapterXPathResolver.cpp:52-61`) are given with line numbers
  and the exact text being replaced.
- **Type consistency.** `bookorbit::XPointer` and `XPointerStep` (Task 2) are
  consumed unchanged by Tasks 3, 4, 5, 6, 7 and 10. Offsets are `uint32_t`
  zero-based visible codepoints everywhere, matching
  `CrossPointPosition::visibleTextOffset` (`ProgressMapper.h:22`) and
  `ChapterXPathResolver::findXPathForVisibleTextOffset`. `charOffset` is `long`
  with `-1` meaning absent, never `0` — `.0` is a real KOReader offset (it is
  what both production sidecars carry) and the two must not collide. DOM
  versions are plain `int` in both `corpus::` and `bookorbit::`, and
  `XPointerDom.NormalizationBoundaryMatchesCrengine` asserts the two agree.
  Percentages are `float` in 0..1 throughout, matching
  `KOReaderPosition::percentage` and `BookSyncState::progressPushedPct` (P0
  Task 5). `ProgressRecord` (Task 6) is the single record type used by Tasks 7,
  8, 9 and 11. `NativePosition` (Task 10) mirrors `KOReaderRichPosition`
  (`KOReaderSyncClient.h:23-30`) field for field so `fromRichPosition` needs no
  change.
- **Reused rather than rewritten.** `ChapterXPathResolver`'s existing
  `ParentState::nextIndex` sibling counting (`:31-45`), its non-visible tag set
  (`:77-79`), and its codepoint counting (`:63-75`) are the model for
  `XPointerResolver`; the resolver is a separate file only because
  `ChapterXPathResolver` takes a `std::shared_ptr<Epub>` and therefore cannot
  run under the native suite. `KOReaderDocumentId::calculate()` is the document
  digest for every endpoint here and is not touched.
- **Known gap.** `ProgressMapper` itself has no native test target — it depends
  on `Epub`, `GfxRenderer` and `Logging`. Task 5's coverage is therefore the
  pure grammar plus device builds and the simulator smoke test. Giving
  `ProgressMapper` a host target is worth doing but is a larger stub exercise
  than this phase should carry.
- **Fixture drift.** The corpus is committed output. If `test/epubs/` gains or
  loses an EPUB, `corpus::epubNames()` and the row-count assertions in
  `CorpusFixtureTest` must be updated in the same commit as a regenerated
  `fixtures/` directory — the tests fail loudly rather than silently shrinking
  the corpus, which is the point of the distinctness guard.
