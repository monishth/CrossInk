# BookOrbit Native Sync for CrossInk — Design

**Date:** 2026-09-11
**Target device:** Xteink X4 Pro (`x4-pro` env — ESP32-S3 `n16r8`, dual-core 240 MHz, 16 MB flash, 8 MB PSRAM, SDMMC storage)
**Base:** fork of [`uxjulia/CrossInk`](https://github.com/uxjulia/CrossInk) v1.5.1

## Goal

Make the X4 Pro a first-class BookOrbit client at the same fidelity as the
BookOrbit KOReader plugin — not the percentage-only fidelity of stock KOSync —
and record reading statistics equivalent to KOReader's `statistics.koplugin`.

## Non-goals

- Imprint integration. Explicitly out of scope.
- Removing CrossPoint sync. The X4 Pro has ample flash and PSRAM; removing it
  would buy nothing and would create permanent merge conflicts with upstream.
- Dropping the ESP32-C3 targets (X3 / X4 / Sticky). They stay buildable. Where a
  feature cannot fit the C3's ~380 KB of internal RAM, it is compiled out by
  target rather than removed.

## Strategy: additive and mergeable

All new code lives in new files. Shared upstream code is touched only where a
hook is unavoidable, and those touch points are enumerated per phase. The fork
must be able to merge upstream CrossInk indefinitely.

Three deliberate reuses rather than rewrites:

| Reuse | Why |
|---|---|
| `KOReaderDocumentId::calculate()` | Already implements KOReader's partial MD5 exactly (12 offsets at `1024 << 2i`, 1024 bytes each). This hash is BookOrbit's only book key. |
| `ProgressMapper` / `ChapterXPathResolver` | Existing xpointer↔`(spine,page)` machinery. Extended in P2, not replaced. |
| `obfuscation::obfuscateToBase64()` | MAC-keyed credential obfuscation, matching `KOReaderCredentialStore`. |

---

## Protocol summary

Base URL is normalized to end in `/api/v1` (`https://books.example.com` →
`https://books.example.com/api/v1`). Every request carries:

```
accept: application/json
x-auth-user: <username>
x-auth-key: <lowercase hex md5(password)>
```

Every `/koreader/plugin/*` POST body additionally carries `deviceId`,
`deviceModel`, `pluginVersion`, and `deviceTime` (local `%Y-%m-%d %H:%M:%S`).

Request bodies are capped client-side at 900 KiB (the server's limit is 1 MiB).
Error convention: non-2xx yields `(status, decodedBody)`; transport failure
yields a string reason. `401`/`403` and transport errors abort the whole sync;
other numeric errors mark the phase failed, leave its watermark unadvanced, and
move on.

### Endpoints by phase

| Phase | Method & path | Batch |
|---|---|---|
| P0 | `GET /koreader/users/auth` | — |
| P0 | `GET /koreader/plugin/version` → `{capabilities[], serverVersion}` | — |
| P0 | `POST /koreader/plugin/match-check` | 500 hashes |
| P0 | `POST /koreader/plugin/sweeps` → `{libraryVersion}` | — |
| P1 | `POST /koreader/plugin/page-stats` | 500 events |
| P2 | `GET /koreader/syncs/progress/{digest}` | — |
| P2 | `PUT /koreader/syncs/progress` | — |
| P2 | `POST /koreader/plugin/progress` (bulk) | 100 items |
| P3 | `POST /koreader/plugin/book-states` | 200 books |
| P4 | `POST /koreader/plugin/annotations/exchange` + `/exchange-ack` | 50 changes |
| P4 | `POST /koreader/plugin/bookmarks/exchange` + `/exchange-ack` | 50 changes |
| P5 | `GET /koreader/plugin/catalog/{root,dashboard,sections/…,books,manifest}` | cursor / page |
| P5 | `GET /koreader/plugin/catalog/files/{id}/download`, `…/books/{id}/thumbnail` | streamed |
| P5 | `PUT /koreader/plugin/catalog/books/{id}/{read-status,rating}` | — |

---

## Module layout

```
lib/BookOrbit/                       # core: host-testable, no Arduino deps
  BookOrbitClient.{h,cpp}            HTTP, auth, error taxonomy
  BookOrbitCapabilities.{h,cpp}      tri-state capability cache
  BookOrbitSyncState.{h,cpp}         durable per-book watermarks, atomic write
  BookOrbitCredentialStore.{h,cpp}   credentials + server URL
  BookOrbitOutbox.{h,cpp}            durable queue + phase-ack state machine
  ReadingEventLog.{h,cpp}            append-only page-stat event log
  ReadingStatsQuery.{h,cpp}          KOReader's statistics formulas over the log
  XPointer.{h,cpp}                   crengine xpointer parse / normalize / emit
src/activities/bookorbit/            settings, sync progress, stats screens
```

The `lib/BookOrbit/` core is written against narrow storage and HTTP interfaces
so the whole of it compiles and runs under the native CTest suite.

---

## P0 — API client and durable sync state

**Delivers:** the device authenticates, negotiates capabilities, and matches its
library against the server.

### Sync state

Per-book state keyed by partial MD5, ported faithfully from the Lua plugin's
`bookorbit_sync_state.lua`:

```c
struct BookSyncState {
  uint32_t statsWatermark;       // max uploaded event startTime
  uint32_t matchVerifiedAt;      // unix; 24h TTL
  char     matchVerifiedVersion[24];  // server libraryVersion at match time
  uint32_t bookId, fileId;
  float    progressPushedPct;
  char     annSignature[48], bmSignature[48];   // "count:maxDt:h1:h2"
  uint32_t annExchangedAt, bmExchangedAt;
  char     statusSyncedModified[11];            // YYYY-MM-DD
};
```

Persisted atomically — temp file → `flush()`/`sync()` → size verify → rotate old
to `.bak` → `rename()` — the same pattern `GlobalReadingStats.cpp:197-267`
already uses, and the same durability guarantee the Lua plugin's `flush()` gives.

### Phase chain

Per book: `match → stats → progress → state → annotations → bookmarks`.

Each phase is durably acknowledged to disk **before** advancing. A crash or
battery pull between phases must never skip a watermark. Phases run one step at
a time off the UI cadence so the reader stays responsive.

### Match caching

A book is syncable only once its hash appears in a `match-check` response.
Matches cache for 24 hours, invalidated early when the server's opaque
`libraryVersion` token changes. `match-check` sends candidate metadata
(`title`, `authors`, `lastOpen`, `metadataAmbiguous`) as hints; the server
decides. The client never does fuzzy matching itself.

### Capability negotiation is tri-state

`true` (supported) / `false` (confirmed absent) / `unknown`. Any 5xx or
transport error yields *unknown* and is never cached as a negative — otherwise a
single blip permanently disables bookmark sync. Only a definitive 4xx caches a
negative. A confirmed 404 on a feature's own route downgrades that capability
immediately.

### No retry loops

Deliberate, matching the Lua client. A failed phase leaves its watermark
unadvanced and retries on the next sync trigger (book close, suspend, manual
sync, periodic push, outbox drain). On a battery device, timed retry loops are
the wrong default.

### Two corrections to existing behaviour

1. **Book matching must use content hashing.** CrossInk defaults to
   `DocumentMatchMethod::FILENAME`. BookOrbit forbids it: *"BookOrbit matches on
   the scanner-computed partial MD5 of the file, so the filename checksum method
   does not exist here."* The BookOrbit path forces partial MD5 regardless of the
   KOSync setting.

2. **TLS must verify certificates.** `KOReaderSyncClient.cpp` calls
   `setInsecure()` before every request, so credentials are sent with no
   certificate validation. The BookOrbit client validates against a CA bundle,
   with an optional pinned SHA-256 fingerprint for self-signed deployments.

Large responses are read through the existing `lib/JsonParser/StreamingJsonParser`
(512-byte token buffer, 32 nesting levels, constant memory) rather than
ArduinoJson's whole-body-in-memory path, which does not scale to catalog
responses near the 900 KiB cap.

---

## P1 — Reading event log and page-stats

**Delivers:** KOReader-equivalent statistics, recorded on-device and uploaded.

### Why this maps cleanly

BookOrbit's upload is a verbatim dump of KOReader's `page_stat_data` rows:

```json
{"books": [{"hash": "<partial-md5>",
            "events": [{"page": 42, "startTime": 1787561453,
                        "durationSeconds": 37, "totalPages": 310}]}]}
```

No translation layer and no SQLite are needed — the on-device log *is* this
record.

### Stable page numbers

CrossInk already has layout-independent reference pages:
`Epub::resolveReferencePage(spineIndex, spineRead, &currentPage, &pageCount)`,
gated by `hasStablePageNumbers()` (`totalWords`, `wordsPerReferencePage`,
`totalReferencePages`). These do not move when font size or margins change.

This is the property KOReader's `page_stat` rescaling VIEW exists to
reconstruct, so we get comparability natively.

When a book has no x-locations (`hasStablePageNumbers()` false), fall back to
byte-based progress via `calculateSizeProgress()` and define nominal pages as
`totalPages = ceil(getBookSize() / 2048)`, with
`page = floor(progress * totalPages) + 1`. `getBookSize()` is fixed for a given
file, so this total is stable across sessions and font changes — the property
that matters. The 2048-byte divisor is arbitrary but must never change once
events exist; `totalPages` is recorded on every event, so the server can rescale
exactly as KOReader's view does even if a later firmware picks a different
divisor.

### Record format

```c
struct ReadingEvent {   // 16 bytes, little-endian
  uint32_t page;
  uint32_t startTime;          // unix epoch
  uint16_t durationSeconds;
  uint16_t totalPages;
  uint32_t reserved;
};
```

Append-only, one file per book alongside the existing `stats_v5.bin`. At roughly
200 events per reading hour this is ~3 KB/hour — negligible on SD.

### Clamping: KOReader's rules exactly

| Condition | KOReader | Adopted |
|---|---|---|
| dwell < `min_sec` (default **5 s**) | discard | discard |
| `min_sec` ≤ dwell ≤ `max_sec` | credit in full | credit in full |
| dwell > `max_sec` (default **120 s**) | **clamp to `max_sec`** | clamp to `max_sec` |

Both configurable. This differs from CrossInk's current behaviour (2 s minimum;
*discard* entirely above a 300 s idle threshold) and is the change that makes
on-device numbers comparable to KOReader's. The hook already exists —
`recordCurrentPageReadingTime()`, `EpubReaderActivity.cpp:1481` — and gains an
append beside its existing counter updates.

### Upload

Watermark-incremental: events where `startTime > statsWatermark`, ordered by
`(startTime, page)`, 500 per POST.

**The one-second back-off is mandatory.** When a batch comes back full, the
watermark is set to `lastEvent.startTime - 1` rather than `lastEvent.startTime`,
because a 500-row cut can land inside a group of events sharing one timestamp.
Without it those events are dropped permanently. Re-sent events are idempotent
server-side. On a short batch, trust the server-reported `watermark`.

A book reported `unmatched` does not advance its watermark.

### Statistics computed on-device

From the event log, following KOReader's formulas:

- total time (uncapped) and capped time — `min(sum(duration), max_sec)` grouped
  **per page**, not per event
- pages read, distinct
- average time per page = capped time / capped pages
- average time per day = book time / distinct active days
- estimated time left = `(totalPages - currentPage) * avgTimePerPage`
- estimated days to finish and finish date
- per-day / per-week / per-month buckets, local time
- calendar heatmap: day × hour, `sum(duration)/3600`

Plus two that are *not* from `statistics.koplugin` and are noted as such:
reading streak (current and longest) exists in CrossInk today and in BookOrbit's
own `readingSummary()`, but not in KOReader's statistics plugin; time-of-day and
day-of-week buckets are CrossInk's. Both are recomputed from the event log so
they agree with everything else, and both are retained.

### Two honest limitations

**Events require the RTC.** `HalClock::isAvailable()` gates wall-clock time.
Events recorded without valid time are buffered with a monotonic marker and
backfilled on the next NTP sync; if none arrives within a bounded window they
are discarded rather than uploaded with a fabricated `startTime`.

**Historical totals will not match.** The log starts empty, while
`global_stats.bin` holds totals accumulated under the old 2 s/300 s rules. Both
are kept: log-derived statistics for the covered period, and the legacy totals
preserved and labelled as a pre-event-logging baseline. The two clamping regimes
are never silently summed.

---

## P2 — High-fidelity progress sync

**Delivers:** two-way reading position at xpointer fidelity.

### The defect to fix first

Real KOReader sidecars contain fully-indexed crengine xpointers:

```
/body[1]/DocFragment[1]/body[1]/div[1]/svg[1].0
```

`ProgressMapper` matches on the literal `"/body/DocFragment["`
(`ProgressMapper.cpp:62,134,192,257`) and emits `"/body/DocFragment[N]/body"`
(`:1006`) — without the `[1]` indices. No normalization exists in
`ProgressMapper.cpp` or `ChapterXPathResolver.cpp`. The `find()` therefore misses
on any genuine KOReader xpointer and both directions silently fall through to
percentage heuristics.

This is consistent with the code's own admission that CrossInk "discards HTML
structure during parsing," and implies the existing KOSync path has only ever
round-tripped positions CrossInk itself wrote. A normalization layer is cheap and
is the highest-value single fix in this project.

*Verification status:* confirmed by code inspection, absence of normalization,
and a real sidecar sample. The round-trip corpus below is what proves it
end-to-end; that test is written first and must fail before the fix.

### Approach A — client-side xpointer fidelity (required)

New `lib/BookOrbit/XPointer.{h,cpp}`:

- parse a crengine xpointer into `(docFragment, steps[{name,index}], charOffset)`
- normalize on ingest, tolerating both indexed and unindexed forms
- emit canonical form: explicit index on every step, `.N` offset suffix

Then extend the resolver from paragraph granularity to full element-ancestry
granularity, tracking same-name sibling counts while streaming the spine XHTML.
`ChapterXPathResolver` already streams the source; it gains a sibling-counting
ancestry stack.

### Approach B — server-side native position (follow-on)

Add an optional device-position blob to BookOrbit's progress record, mirroring
what CrossPoint's own sync server accepts:

```json
"position": {"pctQ": 0, "spine": 0, "page": 0, "pages": 1, "para": 0, "xpath": "…"}
```

CrossInk↔CrossInk becomes lossless; KOReader clients ignore the field and use
the xpointer. Strictly additive, no migration. Requires a small server change,
which is available since the server is self-hosted.

### Never degrade silently

Always send both `progress` (xpointer) and `percentage`. On receive, prefer the
xpointer; fall back to percentage only when resolution fails, and surface that in
the UI when the resulting jump exceeds a threshold. A silent fallback to
percentage is exactly the low-fidelity behaviour this project exists to avoid.

---

## P3 — Book states

**Delivers:** reading status, rating, and review synced two-way.

`POST /koreader/plugin/book-states`, batched at 200:

```json
{"hash": "…", "status": "reading|complete|abandoned",
 "statusModified": "2026-08-21", "rating": 4,
 "reviewNote": "…", "reviewModified": "2026-08-21"}
```

Response carries `ratingSet`, `rating`, `ratingUpdatedAt`, `reviewNoteSet`,
`reviewNote`, `reviewUpdatedAt` for the pull direction.

Maps onto CrossInk's existing `isCompleted` flag and "mark as finished" flow.
New: a 1–5 rating UI, and date-only (`YYYY-MM-DD`) modification tracking for
conflict resolution. Clearing is explicit (`ratingCleared` / `reviewCleared`),
not an absent field.

---

## P4 — Bookmarks and highlights

**Delivers:** two-way annotation sync. **Depends on P2** — BookOrbit identifies
annotations by `pos0`/`pos1` xpointers, the same machinery.

### Three-legged exchange

1. `POST …/exchange` with `{hash, keys, keysComplete, changes}`
   - `keys[i] = {k: md5(datetime + "|" + pos0), dt: datetime}` — the local
     identity set, enabling server-side deletion detection
   - `keysComplete` false when the set exceeds the cap; deletion detection then
     degrades gracefully rather than deleting wrongly
2. Response `{unmatched, results: [{hash, toApply: {add, delete}, more}]}`
3. `POST …/exchange-ack` with per-item `applied` / `failed` status

Chunks of 50 changes; max 10 pull rounds; key cap 5000 (annotations) / 500
(bookmarks).

### Change-detection signature

`"count:maxDatetime:hash1:hash2"` — stored per book, compared before exchanging.
When unchanged, the exchange is skipped entirely. This is what keeps routine
syncs cheap.

### Field mapping

Annotations normalize to `{datetime, datetimeUpdated, drawer, color, text, note,
chapter, pageno, posFormat, pos0, pos1}` with `drawer ∈ {lighten, underscore,
strikeout, invert}`, `posFormat = "xpointer"`, and truncation limits (`text`
≤ 10000, `note` ≤ 5000, `chapter` ≤ 500, `pos0`/`pos1` ≤ 4000, `color` ≤ 30).
Bookmarks normalize to `{datetime, datetimeUpdated, pos, pageno, chapter, note}`.

CrossInk's `BookmarkStore` and `ClippingStore` are mapped onto these shapes.

`bookmarkSync` is capability-gated; the tri-state rule matters most here.

---

## P5 — Catalog and downloads

**Delivers:** browse the BookOrbit library on-device and download over WiFi.

Reuses CrossInk's existing OPDS browse and download UI patterns
(`OpdsParser`, `OpdsServerStore`).

- Catalog browsing: page-number pagination (`page`, `size`, `q`, filters), with
  query keys URL-encoded and sorted.
- Bulk manifest: **cursor** pagination (`cursor` → `{hasNext, nextCursor}`),
  restarting enumeration from scratch on a rejected cursor.
- Downloads: streamed to a `.part` temp file then atomically renamed;
  same-origin redirects capped at 5; byte cap enforced; partial MD5 computed on
  the downloaded file.

That last step closes the loop: the hash it produces is the sync key for every
other phase, so a freshly downloaded book is immediately syncable.

Catalog responses are parsed through `StreamingJsonParser`, never buffered whole.

---

## Testing

The native CMake/CTest suite makes nearly all of this host-testable.

```
cmake -S test -B /tmp/crossink-tests -G 'Unix Makefiles'
cmake --build /tmp/crossink-tests -j 6
ctest --test-dir /tmp/crossink-tests --output-on-failure
```

| Area | Test |
|---|---|
| Stats formulas | Run KOReader's own SQL against the real `statistics.sqlite3` (**2027 events, 11 books**) with `sqlite3`; assert `ReadingStatsQuery` produces identical numbers. Verification against the real implementation, not against a reading of it. |
| Protocol | Golden encode/decode fixtures per endpoint, including `withDevice` field injection and the 900 KiB body guard. |
| State machine | Crash injection between phase acks; assert no watermark ever advances past unacknowledged data. |
| Event log | Clamping boundaries (4/5/119/120/121 s); the one-second watermark back-off on a full batch. |
| XPointer | Round-trip corpus from real sidecars (`imprint-dev-books/*.sdr`) and the 12 EPUB fixtures in `test/epubs/`; assert resolution lands within a character tolerance. |
| Capabilities | 5xx and transport errors yield *unknown* and never cache a negative. |
| App flow | `./scripts/run_simulator_smoke_test.py` regression tripwire. |

Per-target builds must stay green: `pio run -e x4-pro`, `-e default`, `-e sticky`,
`-e simulator`, plus `pio check -e default --fail-on-defect low`.

---

## Risks

| Risk | Mitigation |
|---|---|
| Xpointer fidelity is not achievable to KOReader's precision | Round-trip corpus quantifies it early, before P4 depends on it. Approach B makes device↔device lossless regardless. |
| RTC drift or absence corrupts `startTime` | Events without valid time are buffered and backfilled, never fabricated. |
| Catalog responses exceed memory | Streaming parser throughout; PSRAM headroom on the X4 Pro. |
| Upstream merge conflicts | New files only; shared-code touch points enumerated per phase. |
| Binary exceeds the 6,553,600-byte app partition | `scripts/check_firmware_size.py` fails the build; phases are independently revertable. |
