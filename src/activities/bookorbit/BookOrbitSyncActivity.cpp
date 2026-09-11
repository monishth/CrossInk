#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <ctime>
#include <utility>

#include "BookOrbitClient.h"
#include "BookOrbitError.h"
#include "BookOrbitMatch.h"
#include "BookOrbitSyncState.h"
#include "BookStatePhase.h"
#include "BookStateStore.h"
#include "MappedInputManager.h"
#include "PageStatsCodec.h"
#include "ReadingEventLog.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "network/BookOrbitBlobStore.h"
#include "network/BookOrbitCredentialStore.h"
#include "network/BookOrbitDocumentHasher.h"

#ifndef SIMULATOR
#include "network/BookOrbitHttpTransport.h"
#endif

namespace {
constexpr char kModule[] = "BORB";
constexpr char kStatePath[] = "/.crosspoint/bookorbit_state.bin";
constexpr char kBookStatePath[] = "/.crosspoint/bookorbit_book_states.bin";
}  // namespace

BookOrbitSyncActivity::BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity(NAME, renderer, mappedInput), epubPath(std::move(path)) {}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  finished = false;
  started = false;
  statusMessage = tr(STR_BOOKORBIT_SYNCING);
  BOOKORBIT_STORE.ensureLoaded();
  requestUpdate();
}

void BookOrbitSyncActivity::onExit() {
  statusMessage.clear();
  bookHash.clear();
  Activity::onExit();
}

const char* BookOrbitSyncActivity::phaseLabel(const bookorbit::SyncPhase phase) const {
  switch (phase) {
    case bookorbit::SyncPhase::Match:
      return "match";
    case bookorbit::SyncPhase::Stats:
      return "stats";
    case bookorbit::SyncPhase::Progress:
      return "progress";
    case bookorbit::SyncPhase::State:
      return "state";
    case bookorbit::SyncPhase::Annotations:
      return "annotations";
    case bookorbit::SyncPhase::Bookmarks:
      return "bookmarks";
    case bookorbit::SyncPhase::Done:
      return "done";
  }
  return "?";
}

void BookOrbitSyncActivity::stepOnePhase() {
  if (finished) return;

  if (!started) {
    started = true;
    // The partial MD5 is BookOrbit's only book key, and it is forced regardless
    // of the KOSync filename-matching setting: BookOrbit has no filename mode.
    BookOrbitDocumentHasher hasher;
    bookHash = hasher.partialMd5(epubPath);
    if (bookHash.empty()) {
      LOG_ERR(kModule, "could not hash %s", epubPath.c_str());
      statusMessage = tr(STR_BOOKORBIT_HASH_FAILED);
      finished = true;
      requestUpdate();
      return;
    }
    LOG_INF(kModule, "syncing %s (%s)", epubPath.c_str(), bookHash.c_str());
  }

  const uint32_t nowUnix = static_cast<uint32_t>(time(nullptr));
  const bookorbit::SyncPhase phase = outbox.currentPhase();
  if (phase == bookorbit::SyncPhase::Done) {
    if (outbox.hadErrors()) {
      statusMessage = tr(STR_BOOKORBIT_SYNC_FAILED);
    } else if (skippedPhases > 0) {
      // Never report a clean sync when phases did not actually run.
      statusMessage = tr(STR_BOOKORBIT_SYNC_PARTIAL);
    } else {
      statusMessage = tr(STR_BOOKORBIT_SYNC_DONE);
    }
    finished = true;
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  // No usable HTTP stack in the simulator; end cleanly rather than pretending.
  statusMessage = tr(STR_BOOKORBIT_UNREACHABLE);
  finished = true;
  requestUpdate();
#else
  BookOrbitBlobStore blobs;
  bookorbit::SyncStateStore syncState(blobs, kStatePath);
  syncState.load();

  BookOrbitHttpTransport transport(BOOKORBIT_STORE.getRootCaPem());
  bookorbit::DeviceIdentity identity;
  identity.deviceId = BOOKORBIT_STORE.getDeviceId();
  identity.deviceModel = CROSSINK_FIRMWARE_DEVICE_TYPE;
  identity.pluginVersion = CROSSINK_VERSION;

  bookorbit::BookOrbitClient client(transport, BOOKORBIT_STORE.getServerUrl(), BOOKORBIT_STORE.getUsername(),
                                    BOOKORBIT_STORE.getMd5Password(), identity);

  // Each phase runs its own request; the outcome decides whether the chain
  // continues. A failed phase leaves its watermark unadvanced and is retried on
  // the next trigger — deliberately no retry loop here.
  bookorbit::Error result;
  std::string body;
  bool phaseRan = true;

  switch (phase) {
    case bookorbit::SyncPhase::Match: {
      bookorbit::MatchCandidate candidate;
      candidate.hash = bookHash;
      candidate.metadataAmbiguous = true;  // no title/author source wired here yet
      std::vector<bookorbit::MatchCandidate> candidates{candidate};

      result = client.postJson("/koreader/plugin/match-check",
                               client.withDeviceFields(bookorbit::encodeMatchCheck(candidates), nowUnix), body);
      if (result.status == bookorbit::Status::Ok) {
        std::vector<bookorbit::MatchResult> matches;
        std::string libraryVersion;
        if (!bookorbit::decodeMatchCheck(body, matches, libraryVersion)) {
          result = bookorbit::Error{bookorbit::Status::InvalidJson, 0};
        } else {
          syncState.setLibraryVersion(libraryVersion);
          matched = !matches.empty();
        }
      }
      break;
    }

    case bookorbit::SyncPhase::Stats: {
      if (!matched) {
        phaseRan = false;
        break;
      }
      BookOrbitBlobStore eventBlobs;
      bookorbit::ReadingEventLog eventLog(eventBlobs, "/.crosspoint/bookorbit_events_" + bookHash + ".bin");

      auto& book = syncState.findOrCreate(bookHash);
      std::vector<bookorbit::ReadingEvent> events;
      if (!eventLog.readAfter(book.statsWatermark, bookorbit::kStatsBatchSize, events) || events.empty()) {
        phaseRan = false;  // nothing new to upload is not a failure, but it is not a sync either
        break;
      }

      result = client.postJson("/koreader/plugin/page-stats",
                               client.withDeviceFields(bookorbit::encodePageStats(bookHash, events), nowUnix), body);
      if (result.status != bookorbit::Status::Ok) break;

      bookorbit::PageStatsAck ack;
      if (!bookorbit::decodePageStats(body, ack)) {
        result = bookorbit::Error{bookorbit::Status::InvalidJson, result.httpStatus};
        break;
      }

      // A book the server does not know must not advance its watermark, or the
      // events would be lost once it is matched.
      if (std::find(ack.unmatched.begin(), ack.unmatched.end(), bookHash) != ack.unmatched.end()) {
        phaseRan = false;
        break;
      }

      const auto watermarkEntry =
          std::find_if(ack.watermarks.begin(), ack.watermarks.end(),
                       [this](const std::pair<std::string, uint32_t>& entry) { return entry.first == bookHash; });
      const uint32_t serverWatermark = watermarkEntry == ack.watermarks.end() ? 0u : watermarkEntry->second;

      // The one-second back-off lives in nextWatermark(): a full batch may have
      // been cut inside a group sharing one startTime, and skipping it would
      // drop those events permanently.
      uint32_t updated = book.statsWatermark;
      bookorbit::nextWatermark(events, bookorbit::kStatsBatchSize, book.statsWatermark, serverWatermark, updated);
      book.statsWatermark = updated;
      break;
    }

    case bookorbit::SyncPhase::State: {
      if (!matched) {
        phaseRan = false;
        break;
      }
      BookOrbitBlobStore stateBlobs;
      bookorbit::BookStateStore stateStore(stateBlobs, kBookStatePath);
      stateStore.load();
      bookorbit::BookStatePhase statePhase(client, syncState, stateStore);
      const auto outcome = statePhase.run(bookHash, false, nowUnix);
      result = (outcome == bookorbit::StatePhaseOutcome::Failed) ? bookorbit::Error{bookorbit::Status::ServerError, 0}
                                                                 : bookorbit::Error{bookorbit::Status::Ok, 0};
      break;
    }

    default:
      // Stats, Progress, Annotations and Bookmarks each need a device-side
      // source (the event log, the reader's live position, BookmarkStore and
      // ClippingStore) that is not wired into this activity yet. Record them as
      // not-run rather than acknowledging success we did not earn: the chain
      // still advances so later phases get their turn, but the screen reports a
      // partial sync rather than "Sync complete".
      phaseRan = false;
      break;
  }

  if (!phaseRan) {
    skippedPhases++;
    result = bookorbit::Error{bookorbit::Status::Ok, 0};
  }

  LOG_DBG(kModule, "phase %s -> status %d", phaseLabel(phase), static_cast<int>(result.status));

  if (!outbox.acknowledge(phase, syncState)) {
    LOG_ERR(kModule, "could not persist acknowledgement for %s", phaseLabel(phase));
    statusMessage = tr(STR_BOOKORBIT_SYNC_FAILED);
    finished = true;
    requestUpdate();
    return;
  }

  if (!outbox.advance(result)) {
    statusMessage = tr(STR_BOOKORBIT_SYNC_FAILED);
    finished = true;
  }
  requestUpdate();
#endif
}

void BookOrbitSyncActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && finished) {
    finishAfterBackPress();
    return;
  }
  // One phase per tick keeps the render task responsive during a sync.
  if (!finished) stepOnePhase();
}

void BookOrbitSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, TouchHeaderBackButton::headerRect(renderer, mappedInput), tr(STR_BOOKORBIT_TITLE));

  // A single status line, vertically centred between the header and the hints.
  GUI.drawHelpText(renderer, Rect{0, static_cast<int>(pageHeight / 2 - 10), pageWidth, 20}, statusMessage.c_str());

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), finished ? tr(STR_OK) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
