# BookOrbit Native Sync — Assumptions Register

Every decision made while writing the spec and the six phase plans that the
source material did **not** settle. Each entry says what was assumed, why, and
what breaks if the assumption is wrong.

Reviewing this list is cheaper than discovering these during implementation.

**Legend — Risk:** how expensive it is to reverse after code exists.
🔴 hard to reverse · 🟡 contained · 🟢 trivial

---

## 1. Project-level

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 1.1 | Fork base is `uxjulia/CrossInk` v1.5.1, not upstream `crosspoint-reader` | User said "crossink"; this fork already has X4 Pro support, reading stats, and a kosync client | 🟡 | Rebase onto upstream; most `lib/BookOrbit/` code is unaffected since it is additive |
| 1.2 | CrossPoint sync is **kept**, BookOrbit added alongside | X4 Pro has 16 MB flash / 8 MB PSRAM; the "save space" rationale does not hold on this target | 🟢 | Delete the KOReaderSync menu entry; no BookOrbit code changes |
| 1.3 | ESP32-C3 targets (X3/X4/Sticky) stay buildable | Keeps upstream merges clean | 🟡 | Could relax C3 memory discipline and simplify P5's streaming |
| 1.4 | All new code in new files under `lib/BookOrbit/` | Mergeability with upstream | 🟢 | — |
| 1.5 | Interfaces (`IHttpTransport`, `IBlobStore`) exist so the core is host-testable | Native CTest suite already exists and is the only fast feedback loop | 🟡 | Testing collapses to on-device only |

## 2. Protocol — **VERIFIED against the server source**

Server cloned to `/home/monish/repos/bookorbit-server` (`github.com/bookorbit/bookorbit`,
public; the deployed image is `ghcr.io/bookorbit/bookorbit:latest`). It ships
`koreader-plugin/` alongside the server, confirming the client the protocol was
read from is authoritative. These are no longer assumptions.

| # | Finding | Evidence | Status |
|---|---|---|---|
| 2.1 | The Lua client accurately describes the server | Server source read directly | ✅ Confirmed |
| 2.2 | `x-auth-key` is `md5(password)`, lowercase hex | `koreader-auth.guard.ts:64-70` accepts a 32-char hex md5 directly, else md5s the incoming value | ✅ Confirmed |
| 2.3 | Server does not gate on device type | No device-type check in the guard or controller | ✅ Confirmed |
| 2.4 | **Re-sent page-stat events are true no-ops** | `koreader-plugin.repository.ts:72-80` — `onConflictDoNothing` on `(userId, bookFileId, deviceId, page, startTime)`. `duplicates = events.length - accepted` is counted and discarded | ✅ **Confirmed — P1's watermark back-off is safe** |
| 2.5 | Body cap 900 KiB client-side | Client-side constant; no server body limit found in `main.ts` | ✅ Safe |
| 2.6 | ~~Batch sizes are client choices~~ **WRONG — they are enforced server limits** | `koreader-stats.service.ts:18` `MAX_EVENTS_PER_REQUEST = 500`, and `:50-52` throws `BadRequestException` above it. Also `MAX_ANNOTATIONS_PER_REQUEST = 50`, `MAX_CHANGES_PER_REQUEST = 50` | ⚠️ **Corrected** — exceeding a batch size is a hard 400, not a soft preference. Never raise them |
| 2.7 | **`deviceId` must be stable and persisted** | The dedup key includes `deviceId`, so a device that regenerates its ID re-inserts its entire history as new rows. The Lua plugin reads a persisted `G_reader_settings` `device_id` (`main.lua:138`) | 🔴 **New requirement** — CrossInk must generate a device ID once, persist it to SD, and never regenerate it on reboot or reflash |
| 2.8 | `durationSeconds` is server-validated at `@Max(86400)` | `dto/koreader-plugin.dto.ts:104-105` | ✅ P1's 120 s clamp is well inside the limit |
| 2.9 | The watermark is the batch's max `startTime`, duplicates included | `koreader-stats.service.ts:94-97` — deliberate, so "a plugin that lost its local state still advances past history the server already has" | ✅ Consistent with P1's back-off |

## 3. Statistics (P1)

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 3.1 | KOReader's clamp defaults (**min 5 s, max 120 s**) replace CrossInk's (2 s / 300 s) | User asked for KOReader-comparable statistics | 🟡 | Settings-level change; historical events keep their recorded durations |
| 3.2 | Long dwells are **clamped to 120 s**, not discarded | KOReader's actual behaviour (`main.lua:2600-2620`) — CrossInk currently discards | 🔴 | Every derived figure shifts. This is the single biggest behavioural change in the project |
| 3.3 | Legacy `global_stats.bin` totals are shown as a separate labelled baseline, never summed with log-derived stats | The two clamping regimes are incompatible | 🟢 | Reset to zero instead |
| 3.4 | `Epub::resolveReferencePage()` is stable across font and margin changes | Implied by `hasStablePageNumbers()` gating on word counts | 🔴 | `page`/`totalPages` would drift between sessions and events stop being comparable. **Verify empirically in P1 hardware check step 5** |
| 3.5 | **2048-byte nominal page** for books without x-locations | Arbitrary; only stability matters, and `getBookSize()` is constant | 🟡 | Change before any events exist for such a book |
| 3.6 | Events without a valid RTC are buffered then discarded after 200, never uploaded with a fabricated timestamp | A wrong `startTime` corrupts every derived statistic irreversibly | 🟡 | Could buffer more, or drop the feature on RTC-less devices |
| 3.7 | 16-byte record is enough (`uint16` duration, `uint16` totalPages) | Duration is clamped to 120 s; 65535 pages is beyond any EPUB | 🟡 | Format bump + migration |
| 3.8 | Flush every 50 events matches KOReader and satisfies `AGENTS.md` rule 8 | KOReader's `MAX_PAGETURNS_BEFORE_FLUSH` | 🟢 | Tune |

## 4. Position fidelity (P2)

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 4.1 | `ProgressMapper` genuinely cannot parse real KOReader xpointers | ✅ **PROVEN against the generated corpus.** The literal `"/body/DocFragment["` is used with `find()` (substring, `ProgressMapper.cpp:15-28,62,257`), so `/body[1]/DocFragment[1]` cannot match: `parseIndex` returns -1, `sourceSpineIndex` = -2, `mapSourceXPathToCurrent` returns false. Match rate **202/202 on legacy DOM 20171225, 0/202 on modern DOM 20260812**. Both real sidecars carry `cre_dom_version = 20260812` | 🟡 | Not wrong. The existing KOSync path interoperates only with pre-2020 crengine; every modern KOReader position silently falls through to percentage |
| 4.2 | **Ground truth is generated by the KOReader emulator**, not taken from sidecars | ✅ **DONE — the generator has been run and the corpus exists.** 404 rows / 353 distinct xpointers across 13 EPUBs × 2 DOM versions, plus 86 extracted spine XHTML files (572 KB) in `test/bookorbit_xpointer_corpus/fixtures/`. Headless drive works with `SDL_VIDEODRIVER=dummy` + the fake `CanvasContext`; byte-identical across repeated runs | 🟢 | — |
| 4.3 | Xpointers are only comparable **within a crengine DOM version** | `CreDocument:getDomVersionWithNormalizedXPointers()` exists; oldest is `20171225` | 🔴 | Positions could resolve to the wrong place across KOReader versions. Fixtures record their DOM version |
| 4.7 | crengine injects **synthetic boxing elements** that appear as xpointer steps but have no counterpart in source XHTML | ✅ **CONFIRMED, and scoped.** Real output contains `autoBoxing` (15 rows) and `tabularBox` (1 row) across 6 of 13 EPUBs — but **only under legacy DOM 20171225**. Modern DOM 20260812 emits **zero** synthetic steps: `/body/DocFragment[11]/body/autoBoxing/img.0` becomes `/body[1]/DocFragment[11]/body[1]/img[1].0`. `rubyBox`/`mathBox`/`floatBox`/`inlineBox`/`pseudoElem` never appeared | 🟢 | Real, but the stripping path is exercised only by legacy-DOM xpointers, not by anything a current KOReader writes |
| 4.4 | Degraded-jump threshold is **2 % of the book** | Spec said "a threshold" with no number | 🟢 | Tune |
| 4.5 | `XPointerResolver` is a new host-testable file; `ChapterXPathResolver` delegates to it | The latter needs `shared_ptr<Epub>` and cannot run natively | 🟡 | Known gap: `ProgressMapper` itself still has no host test |
| 4.6 | Approach B (server-side `position` blob) is optional and reuses `KOReaderRichPosition` | Needs a server change the user can make; field shapes already match | 🟢 | Skip it; Approach A still gives KOReader interop |

## 5. Book states (P3)

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 5.1 | Server-set `abandoned` is **sticky** against CrossInk's two-state finished toggle | CrossInk has no "abandon" gesture; rewriting it to `reading` would silently undo a deliberate choice made elsewhere | 🟡 | Add an abandon gesture |
| 5.2 | Rating/review live in a new `bookorbit_states.bin`, not a `BookReadingStats` format bump | Avoids a cache-version migration and a `docs/file-formats.md` change | 🟢 | — |
| 5.3 | Review notes sync two-way but have **no on-device editor** | Spec asked only for a rating UI | 🟢 | Add an editor later; the store already carries the field |
| 5.4 | Edits made with no valid RTC date are **refused**, not stamped | Conflict resolution is entirely date-driven | 🟡 | Would need a different conflict scheme |
| 5.5 | Ties and both-dates-missing resolve **to the server** | Arbitrary but must be deterministic | 🟢 | Flip it |

## 6. Annotations (P4)

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 6.1 | **No `annWatermark`** — the full normalized set uploads in 50-entry chunks | P0's `BookSyncState` has no datetime watermark; the Lua *bookmark* client does the same | 🟡 | If P0 gains `annWatermark`, P4 Task 8 needs a delta filter |
| 6.2 | `EXCHANGE_MAX_AGE` is **6 hours** | Taken from the Lua plugin; the spec omits it | 🟢 | Tune |
| 6.3 | MD5 must be **reimplemented portably** | `MD5Builder` is Arduino-only; no portable MD5 exists in the repo | 🟢 | — |
| 6.4 | `toApply.edit` is **out of scope** | CrossInk has no highlight-edit UI | 🟡 | Server-side edits would be ignored, not lost |
| 6.5 | `drawer` is always `"lighten"` | CrossInk renders one highlight style | 🟢 | — |
| 6.6 | Bookmark `note` cap is 500 bytes, annotations 5000 | Lua client values | 🟢 | — |

## 7. Catalog (P5)

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 7.1 | Existing OPDS browse/download UI patterns are reusable | `OpdsParser`, `OpdsServerStore` and the network activities already do this | 🟡 | More UI work |
| 7.2 | Downloads stream via `SecureHttpClient`'s `DataCallback` GET overload | Buffering whole EPUBs would blow the C3 heap | 🟢 | — |
| 7.3 | A downloaded file's partial MD5 immediately makes it syncable | The hash is the sync key for every phase | 🟢 | — |

## 8. Toolchain and verification

| # | Assumption | Why | Risk | If wrong |
|---|---|---|---|---|
| 8.1 | TLS pins a **single PEM root**; there is no fingerprint API | `SecureHttpClient::setCACert()` → `wolfSSL_CTX_load_verify_buffer` (`SecureClient.cpp:88`). Verified | 🟢 | — |
| 8.2 | The stale `SecureHttpClient.h:67` comment ("no CA bundle wired up") is wrong | The call path demonstrably reaches wolfSSL | 🟢 | — |
| 8.3 | I18n source is `lib/I18n/translations/english.yaml` | Verified on disk; the P0/P1/P4 drafts initially said `en.yaml` and were corrected | 🟢 | — |
| 8.4 | ~~**The simulator does not currently build on this machine.**~~ **RESOLVED — it builds and runs under stock GCC 16.** Four real defects, all in build flags, none needing clang | See the note below | 🟢 | — |
| 8.5 | `.claude/CONTEXT.md:15` is **stale** — the simulator is configured with image decoders | `lib_ignore = hal, WebSockets` only; `PNGdec`/`JPEGDEC` are in `lib_deps` and were cloned during the build, with `-DCROSSPOINT_SIM_USE_NATIVE_DECODERS` set | 🟢 | Confirmed — both decoders compile and archive into the working binary. P5 thumbnails are simulator-verifiable |
| 8.6 | `wtype` + `grim` is sufficient to drive and capture the simulator | Every screen is keyboard-navigable; Hyprland has no click dispatcher and no pointer tool is installed | 🟡 | Install `wlrctl` for touch-specific paths |
| 8.7 | Ground-truth statistics come from the user's real `statistics.sqlite3` (2027 events, 11 books) | Verifying against KOReader's SQL *executing* beats verifying against a reading of it | 🟢 | — |

---

### Note on 8.4 — the simulator build (resolved 2026-09-11)

The earlier diagnosis was wrong on two of three counts, and missed a fourth
defect entirely. Nothing here required clang, a pinned toolchain, or an
upstream change. All four are build-flag defects in `[simulator-base]`:

1. **Narrowing flag spelling.** Correct as reported. `-Wno-c++11-narrowing`
   is clang-only; GCC reports it as an unrecognized option (note-level) and
   still errors on `src/network/html/*.generated.h`. **`-Wno-narrowing` is
   accepted by both compilers**, so one spelling serves — no dual flags and
   no toolchain pin.
2. **~~libstdc++ 16 `static_assert` in `CssParser`~~ — did not reproduce.**
   All 239 translation units compile cleanly under GCC 16 / libstdc++ 16 once
   defects 1 and 3 are fixed. The register already noted "the actual trigger
   was not isolated"; there was no trigger. This was almost certainly the
   narrowing failure being misattributed.
3. **`QRCode/src/qrcode.h:37` — right file, wrong reason.** The `typedef
   unsigned char bool` sits behind `#ifndef __cplusplus`, so it is only ever
   seen by the **C** compiler. It breaks because GCC 15+ defaults to
   **C23**, where `bool`/`true`/`false` are keywords. Clang 22 still defaults
   to C17, which is the only reason clang appeared to help. Fixed with
   `-std=gnu17`; C++ TUs ignore a C-only `-std`, so it coexists with
   `-std=gnu++2a`.
4. **NEW — missing `-lcrypto`.** The simulator package's
   `MD5Builder_linux.h` calls OpenSSL `MD5_Init`/`MD5_Update`/`MD5_Final`,
   but nothing links libcrypto, so `libKOReaderSync.a` fails at link. This
   was never reached before because compilation failed first. **Relevant to
   BookOrbit:** `MD5Builder` is what P0's `x-auth-key` and the partial-MD5
   book key run through on the simulator.

Applied as three lines in `[simulator-base]` (`platformio.ini`).
`pio run -e simulator`, `-e x4-pro-simulator`, both binaries run under
SDL/Wayland, and `scripts/run_simulator_smoke_test.py` passes.

**On clang specifically.** Clang 22 also builds and runs the simulator, but
it is strictly worse as a default and is not needed:

- Clang here has no libc++ installed and resolves headers against
  **libstdc++ 16** anyway, so it never dodged a libstdc++ issue.
- PlatformIO's native builder (`main.py`) deletes `CC`/`CXX` and hardcodes
  SCons' `gcc`/`g++` tools, so `CC=clang` is silently ignored. Selecting
  clang needs a PATH shim or an extra script.
- PlatformIO only emits `-Wl,--start-group` when
  `GetCompilerType() == "gcc"` (`piobuild.py:70-76`). This project's
  archives are cyclically dependent (`EpdFont` → `InflateReader`,
  `Epub`/`DictHtmlRenderer` → `expat`), so a clang link fails with
  undefined `XML_*` and `InflateReader::*` unless the group is re-added by
  hand.

Verified on Arch: GCC 16.2.1, GCC 15.3.0, Clang 22.1.8, SDL 2.32.70.

## The five worth resolving before writing code

*(2.1 and 2.4 were resolved by reading the server source — see section 2.)*

1. **2.7 — device ID stability.** The server dedups page-stat events on a key
   that *includes* `deviceId`. If CrossInk regenerates its ID on reboot or
   reflash, every re-sent event inserts as new and reading time silently
   doubles. Generate once, persist to SD, never regenerate. This replaced the
   old "read the server source" item, which is now done.
2. **3.2 — the clamp change.** It alters numbers the user already sees. Worth a
   conscious yes.
3. **3.4 — reference-page stability.** If they drift, P1's whole comparability
   argument collapses. Cheap to verify: read a book, change the font, compare.
4. **4.3 — crengine DOM versions.** Still the subtlest failure mode, but now
   measured: the legacy (20171225) and modern (20260812) forms differ on
   *every single row* of the corpus — indices on every step, and synthetic
   boxing steps present only in the legacy form. The parser must accept both;
   the corpus covers both.
5. ~~**8.4 — the Clang requirement.**~~ **Resolved.** There was no clang
   requirement. Four build-flag defects, fixed in `[simulator-base]`;
   simulator verification is unblocked under stock GCC.
