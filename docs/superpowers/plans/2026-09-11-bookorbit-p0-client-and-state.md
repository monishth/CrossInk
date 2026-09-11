# BookOrbit P0 — API Client and Durable Sync State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give CrossInk a host-testable BookOrbit HTTP client that authenticates, negotiates capabilities tri-state, match-checks books by partial MD5, and persists per-book sync watermarks atomically.

**Architecture:** All logic lives in `lib/BookOrbit/` behind two injected interfaces — `IHttpTransport` and `IBlobStore` — so the entire phase compiles and runs under the native GoogleTest suite with no Arduino dependency. Device bindings for those interfaces are the last two tasks. Nothing in `src/` changes except one new settings activity.

**Tech Stack:** C++20, GoogleTest 1.17, CMake/CTest (native), PlatformIO (device), `lib/JsonParser/StreamingJsonParser` for response parsing, `freeink::SecureHttpClient` for TLS.

**Spec:** `docs/superpowers/specs/2026-09-11-bookorbit-native-sync-design.md`

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
- Match cache TTL: **24 hours** (`86400`), invalidated early on `libraryVersion` change.
- Batch limits: match-check **500** hashes.
- Do not edit generated files: `src/network/html/*.generated.h`, `lib/I18n/I18nKeys.h`, `I18nStrings.{h,cpp}`, icon headers, hyphenation tries.
- Add a `CHANGELOG.md` entry for user-facing changes, grouped under Added/Changed/Fixed.
- Verification per task: `ctest --test-dir /tmp/crossink-tests --output-on-failure`. Device-touching tasks additionally: `pio run -e x4-pro` and `pio run -e default`.

## File Structure

| File | Responsibility |
|---|---|
| `lib/BookOrbit/BookOrbitUrl.{h,cpp}` | Base URL normalization and path joining. Pure string logic. |
| `lib/BookOrbit/BookOrbitError.{h,cpp}` | Error taxonomy; `isAuthError` / `isTransient` classification. |
| `lib/BookOrbit/IHttpTransport.h` | Injected HTTP interface. No implementation. |
| `lib/BookOrbit/IBlobStore.h` | Injected byte-blob storage interface. No implementation. |
| `lib/BookOrbit/BookOrbitClient.{h,cpp}` | Request assembly, header set, `withDevice` injection, body cap, response dispatch. |
| `lib/BookOrbit/BookOrbitCapabilities.{h,cpp}` | Tri-state capability cache. |
| `lib/BookOrbit/BookOrbitSyncState.{h,cpp}` | Per-book state record, serialization, atomic persistence. |
| `lib/BookOrbit/BookOrbitMatch.{h,cpp}` | match-check request encode / response decode. |
| `lib/BookOrbit/BookOrbitCredentialStore.{h,cpp}` | Credentials + server URL, MAC-keyed obfuscation. Device-only. |
| `src/network/BookOrbitHttpTransport.{h,cpp}` | `IHttpTransport` over `freeink::SecureHttpClient`, with cert validation. Device-only. |
| `src/activities/bookorbit/BookOrbitSettingsActivity.{h,cpp}` | Server URL, username, password, test-connection. Device-only. |
| `test/bookorbit_*/` | One GoogleTest target per unit, registered in `test/CMakeLists.txt`. |

---

### Task 1: URL normalization

**Files:**
- Create: `lib/BookOrbit/BookOrbitUrl.h`, `lib/BookOrbit/BookOrbitUrl.cpp`
- Create: `test/bookorbit_url/CMakeLists.txt`, `test/bookorbit_url/BookOrbitUrlTest.cpp`
- Modify: `test/CMakeLists.txt` (add `add_subdirectory(bookorbit_url)` beside the existing entries)

**Interfaces:**
- Consumes: nothing.
- Produces: `std::string bookorbit::normalizeServerUrl(std::string_view input)` returning `""` on invalid input; `std::string bookorbit::joinPath(std::string_view base, std::string_view path)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_url/BookOrbitUrlTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitUrl.h"

using bookorbit::joinPath;
using bookorbit::normalizeServerUrl;

TEST(BookOrbitUrl, AppendsApiV1WhenAbsent) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, StripsTrailingSlashesBeforeAppending) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com///"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, KeepsExistingApiV1) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com/api/v1"), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, RewritesLegacyKoreaderSuffix) {
  EXPECT_EQ(normalizeServerUrl("https://books.example.com/api/v1/koreader"),
            "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, TrimsSurroundingWhitespace) {
  EXPECT_EQ(normalizeServerUrl("  https://books.example.com  "), "https://books.example.com/api/v1");
}

TEST(BookOrbitUrl, RejectsEmptyInput) {
  EXPECT_EQ(normalizeServerUrl(""), "");
  EXPECT_EQ(normalizeServerUrl("   "), "");
}

TEST(BookOrbitUrl, JoinsPathOntoBase) {
  EXPECT_EQ(joinPath("https://books.example.com/api/v1", "/koreader/users/auth"),
            "https://books.example.com/api/v1/koreader/users/auth");
}
```

Create `test/bookorbit_url/CMakeLists.txt`:

```cmake
add_executable(BookOrbitUrlTest
  BookOrbitUrlTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
)

target_include_directories(BookOrbitUrlTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitUrlTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitUrlTest)
```

Add to `test/CMakeLists.txt`, after the last existing `add_subdirectory(...)` line:

```cmake
add_subdirectory(bookorbit_url)
```

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `lib/BookOrbit/BookOrbitUrl.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitUrl.h`:

```cpp
#pragma once

#include <string>
#include <string_view>

namespace bookorbit {

// Normalizes a user-entered server URL to the BookOrbit API base, which always
// ends in "/api/v1". Returns "" when the input is empty or whitespace only.
std::string normalizeServerUrl(std::string_view input);

// Joins an absolute path onto a normalized base, collapsing the seam slash.
std::string joinPath(std::string_view base, std::string_view path);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitUrl.cpp`:

```cpp
#include "BookOrbitUrl.h"

namespace bookorbit {
namespace {

constexpr char kApiSuffix[] = "/api/v1";
constexpr char kLegacySuffix[] = "/api/v1/koreader";

std::string_view trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

std::string normalizeServerUrl(const std::string_view input) {
  std::string_view url = trim(input);
  if (url.empty()) return {};

  while (!url.empty() && url.back() == '/') {
    url.remove_suffix(1);
  }
  if (url.empty()) return {};

  if (endsWith(url, kLegacySuffix)) {
    url.remove_suffix(sizeof(kLegacySuffix) - 1 - (sizeof(kApiSuffix) - 1));
    return std::string(url);
  }
  if (endsWith(url, kApiSuffix)) {
    return std::string(url);
  }
  return std::string(url) + kApiSuffix;
}

std::string joinPath(const std::string_view base, const std::string_view path) {
  std::string joined(base);
  if (!path.empty() && path.front() != '/') {
    joined += '/';
  }
  joined.append(path);
  return joined;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitUrl --output-on-failure
```

Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitUrl.h lib/BookOrbit/BookOrbitUrl.cpp test/bookorbit_url test/CMakeLists.txt
git commit -m "feat: add BookOrbit server URL normalization"
```

---

### Task 2: Error taxonomy and classification

**Files:**
- Create: `lib/BookOrbit/BookOrbitError.h`, `lib/BookOrbit/BookOrbitError.cpp`
- Create: `test/bookorbit_error/CMakeLists.txt`, `test/bookorbit_error/BookOrbitErrorTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `enum class bookorbit::Status`; `struct bookorbit::Error { Status status; int httpStatus; }`;
  `Error bookorbit::classify(int httpStatus, bool transportFailed)`;
  `bool bookorbit::isAuthError(const Error&)`; `bool bookorbit::isTransient(const Error&)`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_error/BookOrbitErrorTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include "lib/BookOrbit/BookOrbitError.h"

using bookorbit::classify;
using bookorbit::isAuthError;
using bookorbit::isTransient;
using bookorbit::Status;

TEST(BookOrbitError, SuccessRangeIsOk) {
  EXPECT_EQ(classify(200, false).status, Status::Ok);
  EXPECT_EQ(classify(201, false).status, Status::Ok);
  EXPECT_EQ(classify(204, false).status, Status::Ok);
}

TEST(BookOrbitError, ThreeHundredIsNotOk) {
  EXPECT_NE(classify(300, false).status, Status::Ok);
}

TEST(BookOrbitError, AuthErrorsAreClassifiedAndFlagged) {
  EXPECT_EQ(classify(401, false).status, Status::Unauthorized);
  EXPECT_EQ(classify(403, false).status, Status::Unauthorized);
  EXPECT_TRUE(isAuthError(classify(401, false)));
  EXPECT_TRUE(isAuthError(classify(403, false)));
  EXPECT_FALSE(isAuthError(classify(404, false)));
}

TEST(BookOrbitError, NotFoundIsItsOwnStatus) {
  EXPECT_EQ(classify(404, false).status, Status::NotFound);
}

TEST(BookOrbitError, ServerErrorsAreTransient) {
  EXPECT_EQ(classify(500, false).status, Status::ServerError);
  EXPECT_TRUE(isTransient(classify(500, false)));
  EXPECT_TRUE(isTransient(classify(503, false)));
}

TEST(BookOrbitError, TransportFailureIsTransientRegardlessOfStatus) {
  const auto err = classify(0, true);
  EXPECT_EQ(err.status, Status::Transport);
  EXPECT_TRUE(isTransient(err));
}

// A definitive 4xx means the server understood and said no. It must NOT be
// treated as transient, or a capability would never cache a negative.
TEST(BookOrbitError, ClientErrorsAreNotTransient) {
  EXPECT_FALSE(isTransient(classify(400, false)));
  EXPECT_FALSE(isTransient(classify(404, false)));
  EXPECT_FALSE(isTransient(classify(422, false)));
}

TEST(BookOrbitError, HttpStatusIsPreserved) {
  EXPECT_EQ(classify(418, false).httpStatus, 418);
}
```

Create `test/bookorbit_error/CMakeLists.txt`:

```cmake
add_executable(BookOrbitErrorTest
  BookOrbitErrorTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitErrorTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitErrorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitErrorTest)
```

Add `add_subdirectory(bookorbit_error)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitError.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitError.h`:

```cpp
#pragma once

namespace bookorbit {

enum class Status {
  Ok,
  Unauthorized,   // 401 / 403 — abort the whole sync
  NotFound,       // 404 — may downgrade a capability
  ClientError,    // other 4xx — definitive "no"
  ServerError,    // 5xx — retry on a later trigger
  Transport,      // socket/TLS/DNS failure — no HTTP status
  BodyTooLarge,   // request exceeded MAX_BODY_BYTES, never sent
  InvalidJson,
};

struct Error {
  Status status = Status::Ok;
  int httpStatus = 0;
};

// Maps an HTTP status (or a transport failure) onto the taxonomy.
// transportFailed takes precedence; httpStatus is then meaningless.
Error classify(int httpStatus, bool transportFailed);

// 401/403 only. These abort the entire sync rather than one phase.
bool isAuthError(const Error& error);

// True when the outcome carries no information about server support:
// any 5xx, or a transport failure. A definitive 4xx is NOT transient.
bool isTransient(const Error& error);

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitError.cpp`:

```cpp
#include "BookOrbitError.h"

namespace bookorbit {

Error classify(const int httpStatus, const bool transportFailed) {
  if (transportFailed) {
    return {Status::Transport, 0};
  }
  if (httpStatus >= 200 && httpStatus < 300) {
    return {Status::Ok, httpStatus};
  }
  if (httpStatus == 401 || httpStatus == 403) {
    return {Status::Unauthorized, httpStatus};
  }
  if (httpStatus == 404) {
    return {Status::NotFound, httpStatus};
  }
  if (httpStatus >= 500) {
    return {Status::ServerError, httpStatus};
  }
  return {Status::ClientError, httpStatus};
}

bool isAuthError(const Error& error) { return error.status == Status::Unauthorized; }

bool isTransient(const Error& error) {
  return error.status == Status::Transport || error.status == Status::ServerError;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitError --output-on-failure
```

Expected: 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitError.h lib/BookOrbit/BookOrbitError.cpp test/bookorbit_error test/CMakeLists.txt
git commit -m "feat: add BookOrbit error taxonomy"
```

---

### Task 3: Tri-state capability cache

**Files:**
- Create: `lib/BookOrbit/BookOrbitCapabilities.h`, `lib/BookOrbit/BookOrbitCapabilities.cpp`
- Create: `test/bookorbit_capabilities/CMakeLists.txt`, `test/bookorbit_capabilities/BookOrbitCapabilitiesTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `bookorbit::Error`, `bookorbit::isTransient` (Task 2).
- Produces: `enum class bookorbit::Capability { Unknown, Supported, Unsupported }`;
  class `bookorbit::CapabilityCache` with
  `void rememberFromVersionResponse(const std::vector<std::string>& names)`,
  `void rememberFailure(const Error&)`,
  `void markUnsupported(std::string_view name)`,
  `Capability get(std::string_view name) const`,
  `void invalidate()`.

Capability names in use: `"bookmarkSync"`, `"catalogDashboardSections"`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_capabilities/BookOrbitCapabilitiesTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitError.h"

using bookorbit::Capability;
using bookorbit::CapabilityCache;
using bookorbit::classify;

TEST(BookOrbitCapabilities, StartsUnknown) {
  CapabilityCache cache;
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

TEST(BookOrbitCapabilities, AdvertisedNamesBecomeSupported) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync", "catalogDashboardSections"});
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Supported);
  EXPECT_EQ(cache.get("catalogDashboardSections"), Capability::Supported);
}

// A successful /version response that omits a name is a definitive negative.
TEST(BookOrbitCapabilities, OmittedNameBecomesUnsupported) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"catalogDashboardSections"});
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

// The critical rule: a blip must never permanently disable a feature.
TEST(BookOrbitCapabilities, ServerErrorLeavesUnknown) {
  CapabilityCache cache;
  cache.rememberFailure(classify(503, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

TEST(BookOrbitCapabilities, TransportFailureLeavesUnknown) {
  CapabilityCache cache;
  cache.rememberFailure(classify(0, true));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

// A definitive 4xx on /version means the server has no capability endpoint.
TEST(BookOrbitCapabilities, ClientErrorMarksEverythingUnsupported) {
  CapabilityCache cache;
  cache.rememberFailure(classify(404, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

TEST(BookOrbitCapabilities, ConfirmedRouteFailureDowngradesOneCapability) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.markUnsupported("bookmarkSync");
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

TEST(BookOrbitCapabilities, InvalidateReturnsToUnknown) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.invalidate();
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

// A transient failure after a good response must not erase what we learned.
TEST(BookOrbitCapabilities, TransientFailureDoesNotClearKnownState) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.rememberFailure(classify(500, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Supported);
}
```

Create `test/bookorbit_capabilities/CMakeLists.txt`:

```cmake
add_executable(BookOrbitCapabilitiesTest
  BookOrbitCapabilitiesTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitCapabilities.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitCapabilitiesTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitCapabilitiesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitCapabilitiesTest)
```

Add `add_subdirectory(bookorbit_capabilities)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitCapabilities.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitCapabilities.h`:

```cpp
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitError.h"

namespace bookorbit {

enum class Capability {
  Unknown,      // never asked, or the last answer carried no information
  Supported,
  Unsupported,
};

// Caches what the server advertises at GET /koreader/plugin/version.
//
// The tri-state is the point: a 5xx or a dropped connection must leave a
// capability Unknown so it is retried, never cached as a negative. One blip
// would otherwise disable bookmark sync for the life of the session.
class CapabilityCache {
 public:
  // A successful /version response. Names present are Supported; every name
  // absent from an authoritative response is Unsupported.
  void rememberFromVersionResponse(const std::vector<std::string>& names);

  // A failed /version response. Transient failures change nothing at all;
  // a definitive 4xx means the endpoint does not exist on this server.
  void rememberFailure(const Error& error);

  // A confirmed 404 on a capability's own route downgrades just that one.
  void markUnsupported(std::string_view name);

  Capability get(std::string_view name) const;

  void invalidate();

 private:
  bool authoritative = false;
  std::vector<std::string> supported;
  std::vector<std::string> unsupported;

  static bool contains(const std::vector<std::string>& haystack, std::string_view needle);
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitCapabilities.cpp`:

```cpp
#include "BookOrbitCapabilities.h"

#include <algorithm>

namespace bookorbit {

bool CapabilityCache::contains(const std::vector<std::string>& haystack, const std::string_view needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

void CapabilityCache::rememberFromVersionResponse(const std::vector<std::string>& names) {
  supported = names;
  unsupported.clear();
  authoritative = true;
}

void CapabilityCache::rememberFailure(const Error& error) {
  if (isTransient(error)) {
    // Carries no information. Leave prior state exactly as it was.
    return;
  }
  supported.clear();
  unsupported.clear();
  authoritative = true;
}

void CapabilityCache::markUnsupported(const std::string_view name) {
  supported.erase(std::remove(supported.begin(), supported.end(), name), supported.end());
  if (!contains(unsupported, name)) {
    unsupported.emplace_back(name);
  }
}

Capability CapabilityCache::get(const std::string_view name) const {
  if (contains(supported, name)) return Capability::Supported;
  if (contains(unsupported, name)) return Capability::Unsupported;
  return authoritative ? Capability::Unsupported : Capability::Unknown;
}

void CapabilityCache::invalidate() {
  supported.clear();
  unsupported.clear();
  authoritative = false;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitCapabilities --output-on-failure
```

Expected: 9 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitCapabilities.h lib/BookOrbit/BookOrbitCapabilities.cpp test/bookorbit_capabilities test/CMakeLists.txt
git commit -m "feat: add tri-state BookOrbit capability cache"
```

---

### Task 4: Blob store interface and atomic writer

**Files:**
- Create: `lib/BookOrbit/IBlobStore.h`, `lib/BookOrbit/AtomicBlobWriter.h`, `lib/BookOrbit/AtomicBlobWriter.cpp`
- Create: `test/bookorbit_atomic_blob/CMakeLists.txt`, `test/bookorbit_atomic_blob/AtomicBlobWriterTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing.
- Produces: `class bookorbit::IBlobStore` with pure virtuals
  `bool read(std::string_view path, std::vector<uint8_t>& out)`,
  `bool write(std::string_view path, const uint8_t* data, size_t len)`,
  `bool rename(std::string_view from, std::string_view to)`,
  `bool remove(std::string_view path)`,
  `bool exists(std::string_view path)`;
  `bool bookorbit::atomicWriteBlob(IBlobStore&, std::string_view path, const uint8_t* data, size_t len)`;
  `bool bookorbit::readBlobWithBackup(IBlobStore&, std::string_view path, std::vector<uint8_t>& out)`.

The write sequence mirrors `GlobalReadingStats.cpp:197-267`: write `path.tmp`, verify size, rotate `path` to `path.bak`, rename `path.tmp` to `path`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_atomic_blob/AtomicBlobWriterTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/AtomicBlobWriter.h"
#include "lib/BookOrbit/IBlobStore.h"

namespace {

// In-memory IBlobStore with a fault-injection hook, so the crash-safety
// guarantee can be tested without a filesystem.
class FakeBlobStore : public bookorbit::IBlobStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;
  int failWriteAfter = -1;   // fail the Nth write (0-based); -1 disables
  int failRenameAfter = -1;
  int writes = 0;
  int renames = 0;

  bool read(const std::string_view path, std::vector<uint8_t>& out) override {
    const auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }

  bool write(const std::string_view path, const uint8_t* data, const size_t len) override {
    if (failWriteAfter >= 0 && writes == failWriteAfter) {
      writes++;
      return false;
    }
    writes++;
    files[std::string(path)] = std::vector<uint8_t>(data, data + len);
    return true;
  }

  bool rename(const std::string_view from, const std::string_view to) override {
    if (failRenameAfter >= 0 && renames == failRenameAfter) {
      renames++;
      return false;
    }
    renames++;
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }

  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }

  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

std::vector<uint8_t> bytes(const std::string& s) { return {s.begin(), s.end()}; }

}  // namespace

TEST(AtomicBlobWriter, WritesPayloadToFinalPath) {
  FakeBlobStore store;
  const auto payload = bytes("hello");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", payload.data(), payload.size()));
  EXPECT_EQ(store.files["/state.bin"], payload);
}

TEST(AtomicBlobWriter, LeavesNoTempFileBehind) {
  FakeBlobStore store;
  const auto payload = bytes("hello");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", payload.data(), payload.size()));
  EXPECT_FALSE(store.exists("/state.bin.tmp"));
}

TEST(AtomicBlobWriter, RotatesPreviousVersionToBackup) {
  FakeBlobStore store;
  const auto first = bytes("first");
  const auto second = bytes("second");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", first.data(), first.size()));
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", second.data(), second.size()));
  EXPECT_EQ(store.files["/state.bin"], second);
  EXPECT_EQ(store.files["/state.bin.bak"], first);
}

// The whole point: a failed write must never damage the existing good file.
TEST(AtomicBlobWriter, FailedTempWriteLeavesOriginalIntact) {
  FakeBlobStore store;
  const auto good = bytes("good");
  ASSERT_TRUE(bookorbit::atomicWriteBlob(store, "/state.bin", good.data(), good.size()));

  store.failWriteAfter = store.writes;  // fail the next write
  const auto bad = bytes("bad");
  EXPECT_FALSE(bookorbit::atomicWriteBlob(store, "/state.bin", bad.data(), bad.size()));
  EXPECT_EQ(store.files["/state.bin"], good);
}

TEST(AtomicBlobWriter, ReadsBackupWhenPrimaryMissing) {
  FakeBlobStore store;
  store.files["/state.bin.bak"] = bytes("recovered");
  std::vector<uint8_t> out;
  ASSERT_TRUE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
  EXPECT_EQ(out, bytes("recovered"));
}

TEST(AtomicBlobWriter, PrefersPrimaryOverBackup) {
  FakeBlobStore store;
  store.files["/state.bin"] = bytes("primary");
  store.files["/state.bin.bak"] = bytes("backup");
  std::vector<uint8_t> out;
  ASSERT_TRUE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
  EXPECT_EQ(out, bytes("primary"));
}

TEST(AtomicBlobWriter, ReportsFailureWhenNeitherExists) {
  FakeBlobStore store;
  std::vector<uint8_t> out;
  EXPECT_FALSE(bookorbit::readBlobWithBackup(store, "/state.bin", out));
}
```

Create `test/bookorbit_atomic_blob/CMakeLists.txt`:

```cmake
add_executable(AtomicBlobWriterTest
  AtomicBlobWriterTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(AtomicBlobWriterTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(AtomicBlobWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AtomicBlobWriterTest)
```

Add `add_subdirectory(bookorbit_atomic_blob)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `IBlobStore.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/IBlobStore.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bookorbit {

// Byte-blob persistence, injected so the BookOrbit core stays host-testable.
// The device implementation wraps FsFile; tests use an in-memory fake.
class IBlobStore {
 public:
  virtual ~IBlobStore() = default;

  virtual bool read(std::string_view path, std::vector<uint8_t>& out) = 0;
  virtual bool write(std::string_view path, const uint8_t* data, size_t len) = 0;
  virtual bool rename(std::string_view from, std::string_view to) = 0;
  virtual bool remove(std::string_view path) = 0;
  virtual bool exists(std::string_view path) = 0;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/AtomicBlobWriter.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "IBlobStore.h"

namespace bookorbit {

// Writes a blob so that a crash or power loss can never leave a torn file.
// Sequence mirrors GlobalReadingStats.cpp:197-267 — write <path>.tmp, verify,
// rotate <path> to <path>.bak, rename <path>.tmp to <path>.
bool atomicWriteBlob(IBlobStore& store, std::string_view path, const uint8_t* data, size_t len);

// Reads <path>, falling back to <path>.bak when the primary is missing.
bool readBlobWithBackup(IBlobStore& store, std::string_view path, std::vector<uint8_t>& out);

}  // namespace bookorbit
```

Create `lib/BookOrbit/AtomicBlobWriter.cpp`:

```cpp
#include "AtomicBlobWriter.h"

#include <string>

namespace bookorbit {
namespace {

std::string withSuffix(const std::string_view path, const char* suffix) {
  return std::string(path) + suffix;
}

}  // namespace

bool atomicWriteBlob(IBlobStore& store, const std::string_view path, const uint8_t* data, const size_t len) {
  const std::string tmp = withSuffix(path, ".tmp");
  const std::string bak = withSuffix(path, ".bak");

  if (!store.write(tmp, data, len)) {
    store.remove(tmp);
    return false;
  }

  std::vector<uint8_t> verify;
  if (!store.read(tmp, verify) || verify.size() != len) {
    store.remove(tmp);
    return false;
  }

  if (store.exists(path)) {
    store.remove(bak);
    // A failed rotation is not fatal: the primary is still good and the
    // rename below is what actually publishes the new version.
    store.rename(path, bak);
  }

  if (!store.rename(tmp, path)) {
    store.remove(tmp);
    return false;
  }
  return true;
}

bool readBlobWithBackup(IBlobStore& store, const std::string_view path, std::vector<uint8_t>& out) {
  if (store.read(path, out)) return true;
  return store.read(withSuffix(path, ".bak"), out);
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R AtomicBlobWriter --output-on-failure
```

Expected: 7 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/IBlobStore.h lib/BookOrbit/AtomicBlobWriter.h lib/BookOrbit/AtomicBlobWriter.cpp test/bookorbit_atomic_blob test/CMakeLists.txt
git commit -m "feat: add atomic blob writer for BookOrbit state"
```

---

### Task 5: Per-book sync state record

**Files:**
- Create: `lib/BookOrbit/BookOrbitSyncState.h`, `lib/BookOrbit/BookOrbitSyncState.cpp`
- Create: `test/bookorbit_sync_state/CMakeLists.txt`, `test/bookorbit_sync_state/BookOrbitSyncStateTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `IBlobStore`, `atomicWriteBlob`, `readBlobWithBackup` (Task 4).
- Produces: `struct bookorbit::BookSyncState`; `class bookorbit::SyncStateStore` with
  `bool load()`, `bool flush()`,
  `BookSyncState* find(std::string_view md5)`,
  `BookSyncState& findOrCreate(std::string_view md5)`,
  `void setLibraryVersion(std::string_view)`, `std::string libraryVersion() const`,
  `bool isMatchFresh(const BookSyncState&, uint32_t nowUnix) const`.

`MATCH_MAX_AGE = 86400`. Serialization is a versioned little-endian binary blob; `kFormatVersion = 1`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_sync_state/BookOrbitSyncStateTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitSyncState.h"
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

constexpr char kMd5[] = "0f0a792b00a37cf80baa5e50c078b31f";

}  // namespace

using bookorbit::SyncStateStore;

TEST(BookOrbitSyncState, FindReturnsNullForUnknownBook) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, FindOrCreateStartsZeroed) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  const auto& book = state.findOrCreate(kMd5);
  EXPECT_EQ(book.statsWatermark, 0u);
  EXPECT_EQ(book.matchVerifiedAt, 0u);
  EXPECT_EQ(book.bookId, 0u);
}

TEST(BookOrbitSyncState, RoundTripsThroughFlushAndLoad) {
  FakeBlobStore store;
  {
    SyncStateStore state(store, "/bookorbit_state.bin");
    auto& book = state.findOrCreate(kMd5);
    book.statsWatermark = 1787407272u;
    book.matchVerifiedAt = 1787561449u;
    book.bookId = 11u;
    book.fileId = 11u;
    book.progressPushedPct = 0.2052f;
    state.setLibraryVersion("5e52cee3c2f603bf");
    ASSERT_TRUE(state.flush());
  }

  SyncStateStore reloaded(store, "/bookorbit_state.bin");
  ASSERT_TRUE(reloaded.load());
  const auto* book = reloaded.find(kMd5);
  ASSERT_NE(book, nullptr);
  EXPECT_EQ(book->statsWatermark, 1787407272u);
  EXPECT_EQ(book->matchVerifiedAt, 1787561449u);
  EXPECT_EQ(book->bookId, 11u);
  EXPECT_FLOAT_EQ(book->progressPushedPct, 0.2052f);
  EXPECT_EQ(reloaded.libraryVersion(), "5e52cee3c2f603bf");
}

TEST(BookOrbitSyncState, LoadOnMissingFileSucceedsEmpty) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_TRUE(state.load());
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, CorruptBlobLoadsEmptyRatherThanCrashing) {
  FakeBlobStore store;
  store.files["/bookorbit_state.bin"] = {0xFF, 0xFF, 0x01};
  SyncStateStore state(store, "/bookorbit_state.bin");
  EXPECT_TRUE(state.load());
  EXPECT_EQ(state.find(kMd5), nullptr);
}

TEST(BookOrbitSyncState, MultipleBooksPersistIndependently) {
  FakeBlobStore store;
  constexpr char kOther[] = "6fba8d1c39745a4fe79813f76dbb314a";
  {
    SyncStateStore state(store, "/bookorbit_state.bin");
    state.findOrCreate(kMd5).statsWatermark = 100u;
    state.findOrCreate(kOther).statsWatermark = 200u;
    ASSERT_TRUE(state.flush());
  }
  SyncStateStore reloaded(store, "/bookorbit_state.bin");
  ASSERT_TRUE(reloaded.load());
  EXPECT_EQ(reloaded.find(kMd5)->statsWatermark, 100u);
  EXPECT_EQ(reloaded.find(kOther)->statsWatermark, 200u);
}

TEST(BookOrbitSyncState, MatchIsFreshInsideTwentyFourHours) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  EXPECT_TRUE(state.isMatchFresh(book, 1000u + 86399u));
}

TEST(BookOrbitSyncState, MatchExpiresAtTwentyFourHours) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  EXPECT_FALSE(state.isMatchFresh(book, 1000u + 86400u));
}

// A changed library version invalidates every cached match immediately.
TEST(BookOrbitSyncState, LibraryVersionChangeInvalidatesMatch) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  auto& book = state.findOrCreate(kMd5);
  book.matchVerifiedAt = 1000u;
  book.setMatchVerifiedVersion("v1");
  state.setLibraryVersion("v2");
  EXPECT_FALSE(state.isMatchFresh(book, 1001u));
}

TEST(BookOrbitSyncState, NeverMatchedIsNotFresh) {
  FakeBlobStore store;
  SyncStateStore state(store, "/bookorbit_state.bin");
  state.setLibraryVersion("v1");
  const auto& book = state.findOrCreate(kMd5);
  EXPECT_FALSE(state.isMatchFresh(book, 1000u));
}
```

Create `test/bookorbit_sync_state/CMakeLists.txt`:

```cmake
add_executable(BookOrbitSyncStateTest
  BookOrbitSyncStateTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitSyncState.cpp
  ${REPO_ROOT}/lib/BookOrbit/AtomicBlobWriter.cpp
)

target_include_directories(BookOrbitSyncStateTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitSyncStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitSyncStateTest)
```

Add `add_subdirectory(bookorbit_sync_state)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitSyncState.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/BookOrbitSyncState.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IBlobStore.h"

namespace bookorbit {

// Matches cache for 24 hours, as the Lua plugin's BookOrbitState.MATCH_MAX_AGE does.
inline constexpr uint32_t kMatchMaxAgeSeconds = 86400;

// Per-book sync bookkeeping, keyed by the file's partial MD5.
// Field names mirror bookorbit_sync_state.lua so the two stay comparable.
struct BookSyncState {
  char md5[33] = {};                    // 32 hex chars + NUL
  uint32_t statsWatermark = 0;          // max uploaded event startTime
  uint32_t matchVerifiedAt = 0;         // unix; 24h TTL
  char matchVerifiedVersion[24] = {};   // server libraryVersion at match time
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  float progressPushedPct = 0.0f;
  char annSignature[48] = {};           // "count:maxDt:h1:h2"
  char bmSignature[48] = {};
  uint32_t annExchangedAt = 0;
  uint32_t bmExchangedAt = 0;
  char statusSyncedModified[11] = {};   // YYYY-MM-DD

  void setMd5(std::string_view value);
  void setMatchVerifiedVersion(std::string_view value);
};

// Holds every book's sync state in one atomically-written blob.
class SyncStateStore {
 public:
  SyncStateStore(IBlobStore& store, std::string path);

  // Returns true on success, including when no file exists yet or the existing
  // blob is unreadable — a corrupt state file must degrade to "sync everything
  // again", never to a crash.
  bool load();
  bool flush();

  BookSyncState* find(std::string_view md5);
  BookSyncState& findOrCreate(std::string_view md5);

  void setLibraryVersion(std::string_view value);
  std::string libraryVersion() const { return library; }

  // Fresh means: matched within 24h AND matched against the current
  // libraryVersion. Either condition failing forces a re-match.
  bool isMatchFresh(const BookSyncState& book, uint32_t nowUnix) const;

 private:
  static constexpr uint8_t kFormatVersion = 1;

  IBlobStore& blobs;
  std::string path;
  std::string library;
  std::vector<BookSyncState> books;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitSyncState.cpp`:

```cpp
#include "BookOrbitSyncState.h"

#include <cstring>

#include "AtomicBlobWriter.h"

namespace bookorbit {
namespace {

template <size_t N>
void copyInto(char (&dest)[N], const std::string_view value) {
  const size_t len = value.size() < (N - 1) ? value.size() : (N - 1);
  std::memcpy(dest, value.data(), len);
  dest[len] = '\0';
}

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool readU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& value) {
  if (pos + 4 > in.size()) return false;
  value = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
  pos += 4;
  return true;
}

void appendStr(std::vector<uint8_t>& out, const char* value) {
  const size_t len = std::strlen(value);
  out.push_back(static_cast<uint8_t>(len));
  out.insert(out.end(), value, value + len);
}

template <size_t N>
bool readStr(const std::vector<uint8_t>& in, size_t& pos, char (&dest)[N]) {
  if (pos >= in.size()) return false;
  const size_t len = in[pos++];
  if (pos + len > in.size() || len >= N) return false;
  std::memcpy(dest, in.data() + pos, len);
  dest[len] = '\0';
  pos += len;
  return true;
}

}  // namespace

void BookSyncState::setMd5(const std::string_view value) { copyInto(md5, value); }
void BookSyncState::setMatchVerifiedVersion(const std::string_view value) { copyInto(matchVerifiedVersion, value); }

SyncStateStore::SyncStateStore(IBlobStore& store, std::string statePath)
    : blobs(store), path(std::move(statePath)) {}

bool SyncStateStore::load() {
  books.clear();
  library.clear();

  std::vector<uint8_t> raw;
  if (!readBlobWithBackup(blobs, path, raw)) {
    return true;  // nothing persisted yet
  }

  size_t pos = 0;
  if (raw.empty() || raw[pos++] != kFormatVersion) {
    return true;  // unknown or corrupt format: start clean
  }

  char libraryBuf[24] = {};
  if (!readStr(raw, pos, libraryBuf)) return true;
  library = libraryBuf;

  uint32_t count = 0;
  if (!readU32(raw, pos, count)) return true;

  for (uint32_t i = 0; i < count; i++) {
    BookSyncState book;
    if (!readStr(raw, pos, book.md5)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.statsWatermark)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.matchVerifiedAt)) { books.clear(); return true; }
    if (!readStr(raw, pos, book.matchVerifiedVersion)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.bookId)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.fileId)) { books.clear(); return true; }
    uint32_t pctBits = 0;
    if (!readU32(raw, pos, pctBits)) { books.clear(); return true; }
    std::memcpy(&book.progressPushedPct, &pctBits, sizeof(float));
    if (!readStr(raw, pos, book.annSignature)) { books.clear(); return true; }
    if (!readStr(raw, pos, book.bmSignature)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.annExchangedAt)) { books.clear(); return true; }
    if (!readU32(raw, pos, book.bmExchangedAt)) { books.clear(); return true; }
    if (!readStr(raw, pos, book.statusSyncedModified)) { books.clear(); return true; }
    books.push_back(book);
  }
  return true;
}

bool SyncStateStore::flush() {
  std::vector<uint8_t> raw;
  raw.reserve(64 + books.size() * 160);
  raw.push_back(kFormatVersion);
  appendStr(raw, library.c_str());
  appendU32(raw, static_cast<uint32_t>(books.size()));

  for (const auto& book : books) {
    appendStr(raw, book.md5);
    appendU32(raw, book.statsWatermark);
    appendU32(raw, book.matchVerifiedAt);
    appendStr(raw, book.matchVerifiedVersion);
    appendU32(raw, book.bookId);
    appendU32(raw, book.fileId);
    uint32_t pctBits = 0;
    std::memcpy(&pctBits, &book.progressPushedPct, sizeof(float));
    appendU32(raw, pctBits);
    appendStr(raw, book.annSignature);
    appendStr(raw, book.bmSignature);
    appendU32(raw, book.annExchangedAt);
    appendU32(raw, book.bmExchangedAt);
    appendStr(raw, book.statusSyncedModified);
  }

  return atomicWriteBlob(blobs, path, raw.data(), raw.size());
}

BookSyncState* SyncStateStore::find(const std::string_view md5) {
  for (auto& book : books) {
    if (md5 == book.md5) return &book;
  }
  return nullptr;
}

BookSyncState& SyncStateStore::findOrCreate(const std::string_view md5) {
  if (auto* existing = find(md5)) return *existing;
  BookSyncState book;
  book.setMd5(md5);
  books.push_back(book);
  return books.back();
}

void SyncStateStore::setLibraryVersion(const std::string_view value) { library = std::string(value); }

bool SyncStateStore::isMatchFresh(const BookSyncState& book, const uint32_t nowUnix) const {
  if (book.matchVerifiedAt == 0) return false;
  if (nowUnix < book.matchVerifiedAt) return false;  // clock went backwards
  if (nowUnix - book.matchVerifiedAt >= kMatchMaxAgeSeconds) return false;
  return library == book.matchVerifiedVersion;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitSyncState --output-on-failure
```

Expected: 10 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitSyncState.h lib/BookOrbit/BookOrbitSyncState.cpp test/bookorbit_sync_state test/CMakeLists.txt
git commit -m "feat: add durable per-book BookOrbit sync state"
```

---

### Task 6: HTTP transport interface and request assembly

**Files:**
- Create: `lib/BookOrbit/IHttpTransport.h`, `lib/BookOrbit/BookOrbitClient.h`, `lib/BookOrbit/BookOrbitClient.cpp`
- Create: `test/bookorbit_client/CMakeLists.txt`, `test/bookorbit_client/BookOrbitClientTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `normalizeServerUrl`, `joinPath` (Task 1); `Error`, `classify` (Task 2).
- Produces:
  `struct bookorbit::HttpRequest { std::string method; std::string url; std::vector<std::pair<std::string,std::string>> headers; std::string body; }`;
  `struct bookorbit::HttpResponse { int status = 0; bool transportFailed = false; std::string body; }`;
  `class bookorbit::IHttpTransport { virtual HttpResponse send(const HttpRequest&) = 0; }`;
  `struct bookorbit::DeviceIdentity { std::string deviceId, deviceModel, pluginVersion; }`;
  `class bookorbit::BookOrbitClient` with
  `BookOrbitClient(IHttpTransport&, std::string baseUrl, std::string username, std::string userkey, DeviceIdentity)`,
  `Error get(std::string_view path, std::string& outBody)`,
  `Error postJson(std::string_view path, std::string_view json, std::string& outBody)`,
  `Error putJson(std::string_view path, std::string_view json, std::string& outBody)`,
  `static std::string withDevice(std::string_view bodyJson, const DeviceIdentity&, std::string_view deviceTime)`.

`kMaxBodyBytes = 900 * 1024`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_client/BookOrbitClientTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitClient.h"
#include "lib/BookOrbit/IHttpTransport.h"

namespace {

class RecordingTransport : public bookorbit::IHttpTransport {
 public:
  std::vector<bookorbit::HttpRequest> sent;
  bookorbit::HttpResponse next;

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override {
    sent.push_back(request);
    return next;
  }
};

bookorbit::DeviceIdentity identity() { return {"crossink-abc123", "Xteink X4 Pro", "0.1.0"}; }

std::string headerValue(const bookorbit::HttpRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers) {
    if (key == name) return value;
  }
  return {};
}

}  // namespace

using bookorbit::BookOrbitClient;
using bookorbit::Status;

TEST(BookOrbitClient, SendsAuthHeadersOnEveryRequest) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "monish", "5f4dcc3b5aa765d61d8327deb882cf99",
                         identity());

  std::string body;
  ASSERT_EQ(client.get("/koreader/users/auth", body).status, Status::Ok);
  ASSERT_EQ(transport.sent.size(), 1u);
  EXPECT_EQ(headerValue(transport.sent[0], "x-auth-user"), "monish");
  EXPECT_EQ(headerValue(transport.sent[0], "x-auth-key"), "5f4dcc3b5aa765d61d8327deb882cf99");
  EXPECT_EQ(headerValue(transport.sent[0], "accept"), "application/json");
}

TEST(BookOrbitClient, JoinsPathOntoBaseUrl) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.get("/koreader/users/auth", body);
  EXPECT_EQ(transport.sent[0].url, "https://books.example.com/api/v1/koreader/users/auth");
}

TEST(BookOrbitClient, PostSetsContentTypeAndBody) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.postJson("/koreader/plugin/match-check", R"({"hashes":[]})", body);
  EXPECT_EQ(transport.sent[0].method, "POST");
  EXPECT_EQ(headerValue(transport.sent[0], "content-type"), "application/json");
  EXPECT_EQ(transport.sent[0].body, R"({"hashes":[]})");
}

TEST(BookOrbitClient, PutUsesPutMethod) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  client.putJson("/koreader/syncs/progress", "{}", body);
  EXPECT_EQ(transport.sent[0].method, "PUT");
}

// The body cap is a client-side guard: an oversized body is never sent.
TEST(BookOrbitClient, OversizedBodyIsRejectedWithoutSending) {
  RecordingTransport transport;
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  const std::string huge(900 * 1024 + 1, 'x');
  std::string body;
  EXPECT_EQ(client.postJson("/koreader/plugin/page-stats", huge, body).status, Status::BodyTooLarge);
  EXPECT_TRUE(transport.sent.empty());
}

TEST(BookOrbitClient, BodyExactlyAtCapIsSent) {
  RecordingTransport transport;
  transport.next = {200, false, "{}"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  const std::string atCap(900 * 1024, 'x');
  std::string body;
  EXPECT_EQ(client.postJson("/koreader/plugin/page-stats", atCap, body).status, Status::Ok);
  EXPECT_EQ(transport.sent.size(), 1u);
}

TEST(BookOrbitClient, TransportFailureIsClassified) {
  RecordingTransport transport;
  transport.next = {0, true, ""};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  EXPECT_EQ(client.get("/koreader/users/auth", body).status, Status::Transport);
}

TEST(BookOrbitClient, UnauthorizedIsClassified) {
  RecordingTransport transport;
  transport.next = {401, false, ""};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  EXPECT_EQ(client.get("/koreader/users/auth", body).status, Status::Unauthorized);
}

TEST(BookOrbitClient, ResponseBodyIsReturned) {
  RecordingTransport transport;
  transport.next = {200, false, R"({"authorized":"OK"})"};
  BookOrbitClient client(transport, "https://books.example.com/api/v1", "u", "k", identity());

  std::string body;
  ASSERT_EQ(client.get("/koreader/users/auth", body).status, Status::Ok);
  EXPECT_EQ(body, R"({"authorized":"OK"})");
}

TEST(BookOrbitClient, WithDeviceInjectsIdentityFields) {
  const std::string merged =
      BookOrbitClient::withDevice(R"({"hashes":["abc"]})", identity(), "2026-09-11 14:03:00");
  EXPECT_NE(merged.find(R"("hashes":["abc"])"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceModel":"Xteink X4 Pro")"), std::string::npos);
  EXPECT_NE(merged.find(R"("pluginVersion":"0.1.0")"), std::string::npos);
  EXPECT_NE(merged.find(R"("deviceTime":"2026-09-11 14:03:00")"), std::string::npos);
}

TEST(BookOrbitClient, WithDeviceHandlesEmptyObject) {
  const std::string merged = BookOrbitClient::withDevice("{}", identity(), "2026-09-11 14:03:00");
  EXPECT_EQ(merged.front(), '{');
  EXPECT_EQ(merged.back(), '}');
  EXPECT_NE(merged.find(R"("deviceId":"crossink-abc123")"), std::string::npos);
  // No stray comma after the opening brace.
  EXPECT_EQ(merged.find("{,"), std::string::npos);
}
```

Create `test/bookorbit_client/CMakeLists.txt`:

```cmake
add_executable(BookOrbitClientTest
  BookOrbitClientTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitClient.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitUrl.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitError.cpp
)

target_include_directories(BookOrbitClientTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
)

target_link_libraries(BookOrbitClientTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitClientTest)
```

Add `add_subdirectory(bookorbit_client)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitClient.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `lib/BookOrbit/IHttpTransport.h`:

```cpp
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace bookorbit {

struct HttpRequest {
  std::string method;
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  bool transportFailed = false;
  std::string body;
};

// Injected so the BookOrbit core is host-testable. The device implementation
// wraps freeink::SecureHttpClient; tests use a recording fake.
class IHttpTransport {
 public:
  virtual ~IHttpTransport() = default;
  virtual HttpResponse send(const HttpRequest& request) = 0;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitClient.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "BookOrbitError.h"
#include "IHttpTransport.h"

namespace bookorbit {

// Stays under the server's 1 MiB body limit, as bookorbit_api.lua does.
inline constexpr size_t kMaxBodyBytes = 900 * 1024;

struct DeviceIdentity {
  std::string deviceId;
  std::string deviceModel;
  std::string pluginVersion;
};

// Assembles authenticated BookOrbit requests. Knows nothing about TLS or
// sockets — that is the injected IHttpTransport's job.
class BookOrbitClient {
 public:
  BookOrbitClient(IHttpTransport& transport, std::string baseUrl, std::string username, std::string userkey,
                  DeviceIdentity identity);

  Error get(std::string_view path, std::string& outBody);
  Error postJson(std::string_view path, std::string_view json, std::string& outBody);
  Error putJson(std::string_view path, std::string_view json, std::string& outBody);

  // Merges deviceId/deviceModel/pluginVersion/deviceTime into a JSON object
  // body, as bookorbit_api.lua's withDevice() does. Every /koreader/plugin/*
  // POST carries these.
  static std::string withDevice(std::string_view bodyJson, const DeviceIdentity& identity,
                                std::string_view deviceTime);

 private:
  Error send(std::string_view method, std::string_view path, std::string_view json, std::string& outBody);

  IHttpTransport& transport;
  std::string baseUrl;
  std::string username;
  std::string userkey;
  DeviceIdentity identity;
};

}  // namespace bookorbit
```

Create `lib/BookOrbit/BookOrbitClient.cpp`:

```cpp
#include "BookOrbitClient.h"

#include <utility>

#include "BookOrbitUrl.h"

namespace bookorbit {
namespace {

// Appends "key":"value" to an object body that is known to be valid JSON.
void appendField(std::string& target, const std::string_view key, const std::string_view value,
                 const bool needsComma) {
  if (needsComma) target += ',';
  target += '"';
  target.append(key);
  target += "\":\"";
  target.append(value);
  target += '"';
}

}  // namespace

BookOrbitClient::BookOrbitClient(IHttpTransport& httpTransport, std::string base, std::string user,
                                 std::string key, DeviceIdentity device)
    : transport(httpTransport),
      baseUrl(std::move(base)),
      username(std::move(user)),
      userkey(std::move(key)),
      identity(std::move(device)) {}

Error BookOrbitClient::get(const std::string_view path, std::string& outBody) {
  return send("GET", path, {}, outBody);
}

Error BookOrbitClient::postJson(const std::string_view path, const std::string_view json, std::string& outBody) {
  return send("POST", path, json, outBody);
}

Error BookOrbitClient::putJson(const std::string_view path, const std::string_view json, std::string& outBody) {
  return send("PUT", path, json, outBody);
}

Error BookOrbitClient::send(const std::string_view method, const std::string_view path,
                            const std::string_view json, std::string& outBody) {
  if (json.size() > kMaxBodyBytes) {
    return {Status::BodyTooLarge, 0};
  }

  HttpRequest request;
  request.method = std::string(method);
  request.url = joinPath(baseUrl, path);
  request.headers.emplace_back("accept", "application/json");
  request.headers.emplace_back("x-auth-user", username);
  request.headers.emplace_back("x-auth-key", userkey);
  if (!json.empty()) {
    request.headers.emplace_back("content-type", "application/json");
    request.body = std::string(json);
  }

  const HttpResponse response = transport.send(request);
  outBody = response.body;
  return classify(response.status, response.transportFailed);
}

std::string BookOrbitClient::withDevice(const std::string_view bodyJson, const DeviceIdentity& identity,
                                        const std::string_view deviceTime) {
  // bodyJson is always an object produced by our own encoders.
  std::string merged(bodyJson);
  if (merged.size() < 2 || merged.front() != '{' || merged.back() != '}') {
    return merged;
  }

  const bool hadFields = merged.size() > 2;
  merged.pop_back();  // drop the closing brace
  appendField(merged, "deviceId", identity.deviceId, hadFields);
  appendField(merged, "deviceModel", identity.deviceModel, true);
  appendField(merged, "pluginVersion", identity.pluginVersion, true);
  appendField(merged, "deviceTime", deviceTime, true);
  merged += '}';
  return merged;
}

}  // namespace bookorbit
```

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitClient --output-on-failure
```

Expected: 11 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/IHttpTransport.h lib/BookOrbit/BookOrbitClient.h lib/BookOrbit/BookOrbitClient.cpp test/bookorbit_client test/CMakeLists.txt
git commit -m "feat: add BookOrbit request assembly and body cap"
```

---

### Task 7: match-check encode and decode

**Files:**
- Create: `lib/BookOrbit/BookOrbitMatch.h`, `lib/BookOrbit/BookOrbitMatch.cpp`
- Create: `test/bookorbit_match/CMakeLists.txt`, `test/bookorbit_match/BookOrbitMatchTest.cpp`
- Modify: `test/CMakeLists.txt`

**Interfaces:**
- Consumes: `StreamingJsonParser` (`lib/JsonParser/StreamingJsonParser.h`).
- Produces:
  `struct bookorbit::MatchCandidate { std::string hash, title, authors; uint32_t lastOpen = 0; bool metadataAmbiguous = false; }`;
  `struct bookorbit::MatchResult { std::string hash; uint32_t bookFileId = 0; uint32_t bookId = 0; }`;
  `std::string bookorbit::encodeMatchCheck(const std::vector<MatchCandidate>&)`;
  `bool bookorbit::decodeMatchCheck(std::string_view json, std::vector<MatchResult>& out, std::string& libraryVersion)`.

`kMatchBatchSize = 500`.

- [ ] **Step 1: Write the failing test**

Create `test/bookorbit_match/BookOrbitMatchTest.cpp`:

```cpp
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitMatch.h"

using bookorbit::decodeMatchCheck;
using bookorbit::encodeMatchCheck;
using bookorbit::MatchCandidate;
using bookorbit::MatchResult;

TEST(BookOrbitMatch, EncodesHashesArray) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "0f0a792b00a37cf80baa5e50c078b31f";
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("hashes":["0f0a792b00a37cf80baa5e50c078b31f"])"), std::string::npos);
}

TEST(BookOrbitMatch, EncodesCandidateMetadataAsHints) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = "We Solve Murders";
  one.authors = "Richard Osman";
  one.lastOpen = 1787561453u;
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("title":"We Solve Murders")"), std::string::npos);
  EXPECT_NE(json.find(R"("authors":"Richard Osman")"), std::string::npos);
  EXPECT_NE(json.find(R"("lastOpen":1787561453)"), std::string::npos);
}

// Ambiguous metadata must be flagged, and title/authors withheld, so the
// server never associates the wrong book. Mirrors bookorbit_book_sync.lua.
TEST(BookOrbitMatch, AmbiguousMetadataIsFlaggedAndWithheld) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = "Ambiguous";
  one.authors = "Someone";
  one.metadataAmbiguous = true;
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"("metadataAmbiguous":true)"), std::string::npos);
  EXPECT_EQ(json.find(R"("title":"Ambiguous")"), std::string::npos);
}

TEST(BookOrbitMatch, EscapesQuotesInTitles) {
  std::vector<MatchCandidate> candidates;
  MatchCandidate one;
  one.hash = "abc";
  one.title = R"(The "Quoted" Book)";
  candidates.push_back(one);

  const std::string json = encodeMatchCheck(candidates);
  EXPECT_NE(json.find(R"(The \"Quoted\" Book)"), std::string::npos);
}

TEST(BookOrbitMatch, DecodesMatchesAndLibraryVersion) {
  const std::string body = R"({
    "matches": [
      {"hash": "abc", "bookFileId": 11, "bookId": 7},
      {"hash": "def", "bookFileId": 12, "bookId": 8}
    ],
    "libraryVersion": "5e52cee3c2f603bf"
  })";

  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(body, results, libraryVersion));
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].hash, "abc");
  EXPECT_EQ(results[0].bookFileId, 11u);
  EXPECT_EQ(results[0].bookId, 7u);
  EXPECT_EQ(results[1].hash, "def");
  EXPECT_EQ(libraryVersion, "5e52cee3c2f603bf");
}

TEST(BookOrbitMatch, DecodesEmptyMatchesAsNoResults) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(R"({"matches":[],"libraryVersion":"v1"})", results, libraryVersion));
  EXPECT_TRUE(results.empty());
  EXPECT_EQ(libraryVersion, "v1");
}

TEST(BookOrbitMatch, RejectsMalformedJson) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  EXPECT_FALSE(decodeMatchCheck("{not json", results, libraryVersion));
}

TEST(BookOrbitMatch, MissingLibraryVersionLeavesItEmpty) {
  std::vector<MatchResult> results;
  std::string libraryVersion;
  ASSERT_TRUE(decodeMatchCheck(R"({"matches":[{"hash":"abc","bookFileId":1,"bookId":2}]})", results,
                               libraryVersion));
  EXPECT_EQ(results.size(), 1u);
  EXPECT_TRUE(libraryVersion.empty());
}
```

Create `test/bookorbit_match/CMakeLists.txt`:

```cmake
add_executable(BookOrbitMatchTest
  BookOrbitMatchTest.cpp
  ${REPO_ROOT}/lib/BookOrbit/BookOrbitMatch.cpp
  ${REPO_ROOT}/lib/JsonParser/StreamingJsonParser.cpp
)

target_include_directories(BookOrbitMatchTest PRIVATE
  ${REPO_ROOT}/lib/BookOrbit
  ${REPO_ROOT}/lib/JsonParser
)

target_link_libraries(BookOrbitMatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookOrbitMatchTest)
```

Add `add_subdirectory(bookorbit_match)` to `test/CMakeLists.txt`.

- [ ] **Step 2: Run test to verify it fails**

```bash
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles' && cmake --build /tmp/crossink-tests -j 6
```

Expected: build FAILS — `BookOrbitMatch.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Read `lib/JsonParser/StreamingJsonParser.h` before writing the decoder — it is a
callback tokenizer (`onKey`, `onString`, `onNumber`, `onObjectStart`,
`onObjectEnd`, `onArrayStart`, `onArrayEnd`) with a 512-byte token buffer and 32
nesting levels. Drive it with a small state struct tracking whether the cursor is
inside the `matches` array and which key was last seen.

Create `lib/BookOrbit/BookOrbitMatch.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// POST /koreader/plugin/match-check accepts at most this many hashes.
inline constexpr size_t kMatchBatchSize = 500;

// Hint payload for one book. The server decides the match; the client never
// does fuzzy title/author matching itself.
struct MatchCandidate {
  std::string hash;      // partial MD5, lowercase hex — the only real key
  std::string title;
  std::string authors;
  uint32_t lastOpen = 0;
  bool metadataAmbiguous = false;  // when true, title/authors are withheld
};

struct MatchResult {
  std::string hash;
  uint32_t bookFileId = 0;
  uint32_t bookId = 0;
};

std::string encodeMatchCheck(const std::vector<MatchCandidate>& candidates);

// Returns false only on malformed JSON. A well-formed response with no
// matches is a success with an empty result vector.
bool decodeMatchCheck(std::string_view json, std::vector<MatchResult>& out, std::string& libraryVersion);

// Escapes a string for embedding in a JSON string literal.
std::string jsonEscape(std::string_view value);

}  // namespace bookorbit
```

Implement `lib/BookOrbit/BookOrbitMatch.cpp` with:

```cpp
#include "BookOrbitMatch.h"

#include "StreamingJsonParser.h"

namespace bookorbit {

std::string jsonEscape(const std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

std::string encodeMatchCheck(const std::vector<MatchCandidate>& candidates) {
  std::string json = R"({"hashes":[)";
  for (size_t i = 0; i < candidates.size(); i++) {
    if (i > 0) json += ',';
    json += '"';
    json += jsonEscape(candidates[i].hash);
    json += '"';
  }
  json += R"(],"books":[)";
  for (size_t i = 0; i < candidates.size(); i++) {
    const auto& candidate = candidates[i];
    if (i > 0) json += ',';
    json += R"({"hash":")";
    json += jsonEscape(candidate.hash);
    json += '"';
    // Ambiguous metadata is withheld entirely rather than risking the wrong
    // title being associated server-side.
    if (!candidate.metadataAmbiguous) {
      if (!candidate.title.empty()) {
        json += R"(,"title":")" + jsonEscape(candidate.title) + '"';
      }
      if (!candidate.authors.empty()) {
        json += R"(,"authors":")" + jsonEscape(candidate.authors) + '"';
      }
    }
    if (candidate.lastOpen != 0) {
      json += R"(,"lastOpen":)" + std::to_string(candidate.lastOpen);
    }
    json += R"(,"source":"current_file")";
    json += R"(,"metadataAmbiguous":)";
    json += candidate.metadataAmbiguous ? "true" : "false";
    json += '}';
  }
  json += "]}";
  return json;
}

namespace {

// StreamingJsonParser uses C-style callbacks with a void* ctx and a 512-byte
// token buffer, so the decoder is a small state machine rather than a DOM walk.
struct MatchDecodeCtx {
  std::vector<MatchResult>* out = nullptr;
  std::string* libraryVersion = nullptr;
  std::string key;       // most recent key at the current level
  int depth = 0;
  bool inMatches = false;
  MatchResult current;
};

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->inMatches) {
    if (ctx->key == "hash") ctx->current.hash.assign(value, len);
  } else if (ctx->key == "libraryVersion") {
    ctx->libraryVersion->assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (!ctx->inMatches) return;
  const uint32_t parsed = static_cast<uint32_t>(strtoul(std::string(value, len).c_str(), nullptr, 10));
  if (ctx->key == "bookFileId") ctx->current.bookFileId = parsed;
  else if (ctx->key == "bookId") ctx->current.bookId = parsed;
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->key == "matches") ctx->inMatches = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->inMatches = false;
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->depth++;
  if (ctx->inMatches) ctx->current = MatchResult{};
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->inMatches && !ctx->current.hash.empty()) {
    ctx->out->push_back(ctx->current);
  }
  ctx->depth--;
  ctx->key.clear();
}

void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

bool decodeMatchCheck(const std::string_view json, std::vector<MatchResult>& out, std::string& libraryVersion) {
  out.clear();
  libraryVersion.clear();

  MatchDecodeCtx ctx;
  ctx.out = &out;
  ctx.libraryVersion = &libraryVersion;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.clear();
    libraryVersion.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
```

Add `#include <cstdio>` for `snprintf` and `#include <cstdlib>` for `strtoul`.

Note: `json.data()` comes from a `string_view` and is **not** null-terminated,
but `feed()` takes an explicit length, so passing it is safe here. Do not pass
it to any C string API.

- [ ] **Step 4: Run test to verify it passes**

```bash
cmake --build /tmp/crossink-tests -j 6 && ctest --test-dir /tmp/crossink-tests -R BookOrbitMatch --output-on-failure
```

Expected: 8 tests PASS.

- [ ] **Step 5: Commit**

```bash
git add lib/BookOrbit/BookOrbitMatch.h lib/BookOrbit/BookOrbitMatch.cpp test/bookorbit_match test/CMakeLists.txt
git commit -m "feat: add BookOrbit match-check codec"
```

---

### Task 8: Device HTTP transport with certificate validation

**Files:**
- Create: `src/network/BookOrbitHttpTransport.h`, `src/network/BookOrbitHttpTransport.cpp`
- Reference: `lib/KOReaderSync/KOReaderSyncClient.cpp:119-159` (wolfSSL usage, heap gate)
- Reference: `freeink-sdk/` — read `SecureHttpClient`'s real header before writing this task

**Interfaces:**
- Consumes: `bookorbit::IHttpTransport`, `HttpRequest`, `HttpResponse` (Task 6).
- Produces: `class BookOrbitHttpTransport : public bookorbit::IHttpTransport` with
  `explicit BookOrbitHttpTransport(std::string rootCaPem)` and
  `bookorbit::HttpResponse send(const bookorbit::HttpRequest&) override`.

**This task has no host test** — it is a thin adapter over an SDK class, and the logic it wraps is already covered by Task 6. Verification is on-device.

**SDK facts, already verified — do not re-derive:**

`freeink-sdk/libs/network/SecureNet/include/SecureHttpClient.h` provides:

```cpp
void setCACert(const char* rootCA);   // verify against a single PEM root; clears _insecure
void setInsecure();                   // skip peer verification — never call this
void setTimeout(uint32_t ms);
void setFollowRedirects(int maxHops); // default 0: 3xx returned as-is
bool begin(const std::string& url);
void addHeader(const std::string& name, const std::string& value);
int  GET();                                   // < 0 on transport failure
int  POST(const std::string& payload);
int  sendRequest(const char* method, const std::string& payload);  // for PUT
const std::string& getString();
void end();
```

`setCACert` is genuinely honoured: `SecureHttpClient.h:411-414` forwards it to
`SecureClient::setCACert`, which reaches
`wolfSSL_CTX_load_verify_buffer(ctx, _rootCA, strlen(_rootCA), WOLFSSL_FILETYPE_PEM)`
at `SecureClient.cpp:88`. The header comment at line 67 claiming "the wolfSSL
transport has no CA bundle wired up" is **stale** — the plumbing works; no
caller has ever passed a CA.

Two consequences for the design:

- It accepts **one PEM root, not a bundle.** For a self-hosted BookOrbit server
  that is the right shape: the user supplies their own server or CA certificate.
- There is **no fingerprint-pinning API.** Single-PEM verification achieves the
  same guarantee, so the spec's "pinned SHA-256 fingerprint" is satisfied by
  pinning the PEM itself. No SDK change is needed.

A return value `< 0` from `GET()`/`POST()`/`sendRequest()` is a transport
failure, mapping to `HttpResponse{0, true, ""}`. A non-negative return is the
HTTP status.

- [ ] **Step 1: Write the adapter**

Mirror `KOReaderSyncClient.cpp:124-159`'s heap gate before any TLS handshake:

```cpp
// TLS handshake allocations fail hard on the C3. Refuse early rather than
// aborting mid-handshake.
static constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
static constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

if (ESP.getFreeHeap() < MIN_FREE_HEAP_FOR_TLS || ESP.getMaxAllocHeap() < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
  LOG_ERR("BookOrbit: insufficient heap for TLS");
  return {0, true, ""};  // transport failure
}
```

Set every header from `request.headers` onto the client, issue the request by
`request.method`, and populate `HttpResponse{status, transportFailed, body}`.
Configure certificate validation from `caBundlePem`; when `pinnedSha256` is
non-empty, additionally require that fingerprint. Never call `setInsecure()`.

- [ ] **Step 2: Verify it builds for every target**

```bash
pio run -e x4-pro && pio run -e default && pio run -e sticky && pio run -e simulator
```

Expected: all four link. The firmware size check (`scripts/check_firmware_size.py`) must pass.

- [ ] **Step 3: Static analysis and formatting**

```bash
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

- [ ] **Step 4: Commit**

```bash
git add src/network/BookOrbitHttpTransport.h src/network/BookOrbitHttpTransport.cpp
git commit -m "feat: add BookOrbit HTTP transport with certificate validation"
```

---

### Task 9: Device blob store and credential store

**Files:**
- Create: `src/network/BookOrbitBlobStore.h`, `src/network/BookOrbitBlobStore.cpp`
- Create: `lib/BookOrbit/BookOrbitCredentialStore.h`, `lib/BookOrbit/BookOrbitCredentialStore.cpp`
- Reference: `lib/KOReaderSync/KOReaderCredentialStore.{h,cpp}` — follow its `PersistableStore` pattern exactly
- Reference: `lib/Serialization/ObfuscationUtils` — `obfuscateToBase64()` / `deobfuscateFromBase64()`

**Interfaces:**
- Consumes: `bookorbit::IBlobStore` (Task 4).
- Produces: `class BookOrbitBlobStore : public bookorbit::IBlobStore` (FsFile-backed, rooted at `/.crosspoint/bookorbit/`);
  `class BookOrbitCredentialStore : public PersistableStore<BookOrbitCredentialStore>` with
  `std::string getBaseUrl() const`, `std::string getUsername() const`, `std::string getUserkey() const`,
  `void setCredentials(std::string_view username, std::string_view password)`,
  `bool isConfigured() const`.

`setCredentials` derives `userkey = md5(password)` using `MD5Builder`, stores only the derived key, and never persists the plaintext password. The store is persisted as JSON at `/.crosspoint/bookorbit.json` with the password field obfuscated via the MAC-keyed helper.

- [ ] **Step 1: Write the blob store**

Wrap `FsFile` through the `Storage` HAL singleton. Every method closes its handle explicitly — on hardware, SdFat allows only one open reader per path, so a fallback that reopens the same file must close the first handle first.

```cpp
bool BookOrbitBlobStore::write(const std::string_view path, const uint8_t* data, const size_t len) {
  FsFile file = STORAGE.open(fullPath(path).c_str(), O_WRITE | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("BookOrbit: cannot open %s for write", fullPath(path).c_str());
    return false;
  }
  const size_t written = file.write(data, len);
  file.flush();
  file.sync();
  file.close();
  return written == len;
}
```

- [ ] **Step 2: Write the credential store**

Follow `KOReaderCredentialStore` exactly, including `cfgVersion` for future
migrations. Derive the key once at set time:

```cpp
void BookOrbitCredentialStore::setCredentials(const std::string_view username, const std::string_view password) {
  MD5Builder builder;
  builder.begin();
  builder.add(std::string(password).c_str());
  builder.calculate();
  // Only the derived key is ever persisted; the plaintext password is not kept.
  userkey = builder.toString().c_str();
  this->username = std::string(username);
  save();
}
```

- [ ] **Step 3: Verify it builds for every target**

```bash
pio run -e x4-pro && pio run -e default && pio run -e simulator
```

- [ ] **Step 4: Static analysis and formatting**

```bash
pio check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
```

- [ ] **Step 5: Commit**

```bash
git add src/network/BookOrbitBlobStore.h src/network/BookOrbitBlobStore.cpp lib/BookOrbit/BookOrbitCredentialStore.h lib/BookOrbit/BookOrbitCredentialStore.cpp
git commit -m "feat: add BookOrbit device storage and credential store"
```

---

### Task 10: Settings activity and connection test

**Files:**
- Create: `src/activities/bookorbit/BookOrbitSettingsActivity.h`, `src/activities/bookorbit/BookOrbitSettingsActivity.cpp`
- Modify: `lib/I18n/translations/en.yaml` (add `STR_BOOKORBIT_*` keys, then regenerate)
- Modify: `src/activities/settings/` — add the entry point beside the existing KOReader sync row
- Modify: `CHANGELOG.md`
- Reference: `src/activities/settings/KOReaderSettingsActivity.cpp` — mirror its structure

**Interfaces:**
- Consumes: `BookOrbitCredentialStore` (Task 9), `BookOrbitHttpTransport` (Task 8), `BookOrbitClient` (Task 6), `CapabilityCache` (Task 3).
- Produces: a settings screen; no API consumed by later tasks.

- [ ] **Step 1: Add translation keys**

Add to `lib/I18n/translations/en.yaml`:

```yaml
STR_BOOKORBIT_TITLE: "BookOrbit Sync"
STR_BOOKORBIT_SERVER_URL: "Server URL"
STR_BOOKORBIT_USERNAME: "Username"
STR_BOOKORBIT_PASSWORD: "Password"
STR_BOOKORBIT_TEST_CONNECTION: "Test connection"
STR_BOOKORBIT_CONNECTED: "Connected"
STR_BOOKORBIT_AUTH_FAILED: "Incorrect username or password"
STR_BOOKORBIT_UNREACHABLE: "Cannot reach server"
STR_BOOKORBIT_CERT_INVALID: "Server certificate not trusted"
```

Regenerate:

```bash
python3 scripts/gen_i18n.py
```

Do not hand-edit `lib/I18n/I18nKeys.h` or `I18nStrings.{h,cpp}` — they are generated.

- [ ] **Step 2: Write the activity**

Mirror `KOReaderSettingsActivity`: rows for server URL, username, and password
via `KeyboardEntryActivity`, plus a "Test connection" row. Allocate durable
state in `onEnter()`, release in `onExit()`, and use `startActivityForResult()`
for the keyboard flows rather than global state.

Test connection issues `GET /koreader/users/auth`, then `GET
/koreader/plugin/version` to seed the `CapabilityCache`. Map the outcome to a
message: `Status::Ok` → `STR_BOOKORBIT_CONNECTED`; `Status::Unauthorized` →
`STR_BOOKORBIT_AUTH_FAILED`; `Status::Transport` → `STR_BOOKORBIT_UNREACHABLE`.
All user-facing strings go through `tr(STR_*)`.

- [ ] **Step 3: Verify it builds and runs**

```bash
pio run -e x4-pro && pio run -e default
pio run -e simulator && ./scripts/run_simulator_smoke_test.py
```

Expected: builds link, smoke test passes with no crash.

- [ ] **Step 4: Add the changelog entry**

Under an `### Added` heading in `CHANGELOG.md`:

```markdown
- BookOrbit sync settings: configure a BookOrbit server URL, sign in, and test the connection.
```

- [ ] **Step 5: Commit**

```bash
find src lib -name "*.cpp" -o -name "*.h" | xargs clang-format -i
git add src/activities/bookorbit lib/I18n/translations/en.yaml lib/I18n CHANGELOG.md src/activities/settings
git commit -m "feat: add BookOrbit sync settings screen"
```

---

## Hardware Verification

After Task 10, on an X4 Pro with an SD card:

1. Settings → BookOrbit Sync. Enter your server URL, username, password.
2. Tap "Test connection". Expect `Connected`.
3. Check the serial log for the negotiated capability list from `/koreader/plugin/version`.
4. Enter a deliberately wrong password. Expect `Incorrect username or password`, not a crash or a hang.
5. Point at an unreachable host. Expect `Cannot reach server` within the socket timeout.
6. Point at a server whose certificate does not chain to the configured PEM root. Expect `Server certificate not trusted` — this is the check that proves `setInsecure()` is genuinely gone.
7. Confirm `/.crosspoint/bookorbit.json` exists on the SD card and contains no plaintext password.

## Self-Review Notes

- **Spec coverage:** P0's spec section lists sync state (Task 5), phase chain (deferred to P1, which is where the first phase actually runs), match caching (Task 5 + Task 7), tri-state capabilities (Task 3), no retry loops (a property of the P1 orchestrator, not P0), content hashing (Task 7 uses the hash; `KOReaderDocumentId` already computes it), TLS validation (Task 8), streaming parser (Task 7). The phase-chain orchestrator is intentionally P1's Task 1 — it has nothing to sequence until page-stats exists.
- **Deferred to P1:** `BookOrbitOutbox.{h,cpp}` from the spec's file list. Building a phase-ack state machine with exactly one phase would be speculative; it lands with its second caller.
- **Type consistency:** `Error`/`Status` (Task 2) are used unchanged in Tasks 3 and 6. `IBlobStore` (Task 4) is consumed by Task 5 and implemented in Task 9. `IHttpTransport` (Task 6) is implemented in Task 8. `MatchCandidate.hash` is the partial MD5 throughout.
