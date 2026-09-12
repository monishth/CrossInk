#include "BookOrbitSyncActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>
#include <utility>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitAnnotationSync.h"
#include "BookOrbitBookmarkModel.h"
#include "BookOrbitBookmarkSync.h"
#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "BookOrbitError.h"
#include "BookOrbitMatch.h"
#include "BookOrbitSyncState.h"
#include "BookStatePhase.h"
#include "BookStateStore.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "MappedInputManager.h"
#include "PageStatsCodec.h"
#include "ReadingEventLog.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/EpubReaderUtils.h"
#include "bookorbit/BookOrbitTime.h"
#include "bookorbit/CrossInkAnnotationSource.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "network/BookOrbitBlobStore.h"
#include "network/BookOrbitCredentialStore.h"
#include "network/BookOrbitDocumentHasher.h"
#include "network/BookOrbitProgressPhase.h"
#include "network/WifiUtils.h"

#ifdef SIMULATOR
#include "network/SimulatorHttpTransport.h"
using BookOrbitTransport = SimulatorHttpTransport;
#else
#include "network/BookOrbitHttpTransport.h"
using BookOrbitTransport = BookOrbitHttpTransport;
#endif

namespace {
constexpr char kModule[] = "BORB";
constexpr char kStatePath[] = "/.crosspoint/bookorbit_state.bin";
constexpr char kBookStatePath[] = "/.crosspoint/bookorbit_book_states.bin";
}  // namespace

BookOrbitSyncActivity::BookOrbitSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                             const Mode activityMode)
    : Activity(NAME, renderer, mappedInput), mode(activityMode), epubPath(std::move(path)) {}

void BookOrbitSyncActivity::runConnectionTest() {
  finished = true;

  BookOrbitTransport transport(BOOKORBIT_STORE.getRootCaPem());
  bookorbit::DeviceIdentity identity;
  identity.deviceId = BOOKORBIT_STORE.getDeviceId();
  identity.deviceModel = CROSSINK_FIRMWARE_DEVICE_TYPE;
  identity.pluginVersion = CROSSINK_VERSION;

  bookorbit::BookOrbitClient client(transport, BOOKORBIT_STORE.getServerUrl(), BOOKORBIT_STORE.getUsername(),
                                    BOOKORBIT_STORE.getMd5Password(), identity);

  std::string body;
  const bookorbit::Error error = client.get("/koreader/users/auth", body);
  switch (error.status) {
    case bookorbit::Status::Ok:
      break;
    case bookorbit::Status::Unauthorized:
      statusMessage = tr(STR_BOOKORBIT_AUTH_FAILED);
      return;
    case bookorbit::Status::Transport:
      // Wi-Fi is genuinely up on this path, so a failure here is a real network
      // or TLS problem rather than the absent-interface case. SecureClient
      // prints the wolfSSL error code, which names which.
      LOG_ERR(kModule, "connection test failed at transport level");
      statusMessage = tr(STR_BOOKORBIT_UNREACHABLE);
      return;
    default:
      statusMessage = tr(STR_BOOKORBIT_UNREACHABLE);
      return;
  }

  // Seed the capability cache so the first real sync need not negotiate.
  std::string versionBody;
  client.get("/koreader/plugin/version", versionBody);
  statusMessage = tr(STR_BOOKORBIT_CONNECTED);
}

void BookOrbitSyncActivity::onEnter() {
  Activity::onEnter();
  finished = false;
  started = false;
  networkReady = false;
  statusMessage = mode == Mode::ConnectionTest ? tr(STR_BOOKORBIT_TESTING) : tr(STR_BOOKORBIT_SYNCING);
  BOOKORBIT_STORE.ensureLoaded();

  // Free the loaded reader font before the TLS stack needs heap.
  sdFontSystem.releaseLoadedFont(renderer);

  // A network boot target does not connect Wi-Fi — it boots minimally so that
  // there is heap for the activity to bring Wi-Fi up itself. Issuing requests
  // without doing that panics inside the network stack on a null semaphore.
  if (hasActiveStationWifiConnection()) {
    networkReady = true;
    requestUpdate();
    return;
  }

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             statusMessage = tr(STR_WIFI_CONN_FAILED);
                             finished = true;
                           } else {
                             WiFi.setSleep(false);
                             networkReady = true;
                           }
                           requestUpdate();
                         });
}

void BookOrbitSyncActivity::onExit() {
  statusMessage.clear();
  bookHash.clear();
  epub.reset();
  Activity::onExit();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
  // Entered from a minimal network boot, so the full app state has to be
  // restored even if nothing succeeded.
  silentRestartAfterNetwork();
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

void BookOrbitSyncActivity::ensureEpubLoaded() {
  if (epub) return;
  epub = std::make_shared<Epub>(epubPath, "/.crosspoint");
  epub->setupCacheDir();
  // Metadata only: progress mapping and annotation collection need the spine
  // and x-locations, not CSS, and rebuilding a missing cache here would stall
  // the sync for minutes.
  if (!epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) {
    LOG_ERR(kModule, "could not load %s for sync", epubPath.c_str());
    epub.reset();
  }
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

  // std::time() counts from boot on this device until something syncs it, so
  // it stamps uploads with 1970 dates that look plausible and permanently skew
  // every derived statistic. Refuse to sync rather than send fabricated times.
  uint32_t nowUnix = 0;
  if (!bookorbit_time::deviceUnixTime(nowUnix)) {
    LOG_ERR(kModule, "no trustworthy clock; refusing to sync");
    statusMessage = tr(STR_BOOKORBIT_CLOCK_UNSET);
    finished = true;
    requestUpdate();
    return;
  }
  const bookorbit::SyncPhase phase = outbox.currentPhase();
  if (phase == bookorbit::SyncPhase::Done) {
    if (outbox.hadErrors()) {
      statusMessage = tr(STR_BOOKORBIT_SYNC_FAILED);
    } else if (degradedLanding) {
      // An approximate landing outranks "complete": the user needs to know the
      // position was resolved by percentage, not exactly. Setting it during the
      // phase would be overwritten by this message, so it is carried here.
      statusMessage = tr(STR_BOOKORBIT_APPROXIMATE_POSITION);
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

  BookOrbitBlobStore blobs;
  bookorbit::SyncStateStore syncState(blobs, kStatePath);
  syncState.load();

  BookOrbitTransport transport(BOOKORBIT_STORE.getRootCaPem());
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

    case bookorbit::SyncPhase::Progress: {
      if (!matched) {
        phaseRan = false;
        break;
      }
      ensureEpubLoaded();
      if (!epub) {
        phaseRan = false;
        break;
      }

      // The reader persists its position per book; read it back rather than
      // inventing one, so a push reflects exactly where the user actually is.
      EpubReaderUtils::Progress saved;
      if (!EpubReaderUtils::loadProgress(*epub, saved, kModule)) {
        phaseRan = false;
        break;
      }

      int spineIndex = saved.spineIndex;
      if (spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) spineIndex = 0;

      CrossPointPosition here = {spineIndex, saved.pageNumber, saved.hasPageCount ? std::max(1, saved.pageCount) : 1};
      if (saved.hasVisibleTextOffset) {
        here.visibleTextOffset = saved.visibleTextOffset;
        here.hasVisibleTextOffset = true;
      }

      const float localPercentage = epub->calculateSizeProgress(
          spineIndex,
          here.totalPages > 0 ? static_cast<float>(here.pageNumber) / static_cast<float>(here.totalPages) : 0.0f);

      BookOrbitProgressPhase progress(client, epub, identity.deviceModel, identity.deviceId, nowUnix);

      // Pull first: an inbound position is only useful before we overwrite it.
      bookorbit::ResolvedProgress resolved;
      if (progress.pull(bookHash, localPercentage, resolved)) {
        // Never apply a degraded landing silently — say so on screen.
        if (resolved.jumpNeedsNotice) {
          LOG_INF(kModule, "degraded landing: remote %.4f local %.4f", static_cast<double>(resolved.percentage),
                  static_cast<double>(localPercentage));
          degradedLanding = true;
        }
      }

      if (!progress.push(bookHash, here, localPercentage)) {
        result = bookorbit::Error{bookorbit::Status::ServerError, 0};
      }
      break;
    }

    case bookorbit::SyncPhase::Annotations: {
      if (!matched) {
        phaseRan = false;
        break;
      }
      ensureEpubLoaded();
      if (!epub) {
        phaseRan = false;
        break;
      }

      // Both stores are unloaded on this path; without this the exchange sends
      // an empty key set and cannot store anything the server returns. An empty
      // key set is not harmless: the server reads it as "the user deleted every
      // highlight in this book" and soft-deletes them all. So a store that will
      // not load has to stop the exchange, not shrink it.
      std::vector<bookorbit::Annotation> raw;
      if (!prepareStoresForBook(epub) || !collectBookOrbitAnnotations(epub, raw)) {
        LOG_ERR(kModule, "highlights unreadable for %s; skipping the exchange rather than reporting none",
                epub->getPath().c_str());
        phaseRan = false;
        break;
      }
      const auto local = bookorbit::normalizeAnnotations(raw);

      CrossInkAnnotationApplier applier(epub, false);
      auto& book = syncState.findOrCreate(bookHash);
      bookorbit::ExchangeOutcome outcome;
      result = bookorbit::exchangeAnnotations(client, book, bookHash, local, applier, nowUnix, outcome);
      break;
    }

    case bookorbit::SyncPhase::Bookmarks: {
      if (!matched) {
        phaseRan = false;
        break;
      }
      ensureEpubLoaded();
      if (!epub) {
        phaseRan = false;
        break;
      }

      // Same rule as the annotation phase: no readable list means no key set,
      // and no key set means the server must not be told one.
      std::vector<bookorbit::Bookmark> raw;
      if (!prepareStoresForBook(epub) || !collectBookOrbitBookmarks(epub, raw)) {
        LOG_ERR(kModule, "bookmarks unreadable for %s; skipping the exchange rather than reporting none",
                epub->getPath().c_str());
        phaseRan = false;
        break;
      }
      const auto local = bookorbit::normalizeBookmarks(raw);

      CrossInkAnnotationApplier applier(epub, true);
      auto& book = syncState.findOrCreate(bookHash);
      bookorbit::CapabilityCache capabilities;
      bookorbit::ExchangeOutcome outcome;
      result = bookorbit::exchangeBookmarks(client, capabilities, book, bookHash, local, applier, nowUnix, outcome);
      break;
    }

    case bookorbit::SyncPhase::Done:
      // Handled above; listed so adding a phase to SyncPhase produces a
      // -Wswitch warning here rather than silently counting as skipped.
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
  if (finished || !networkReady) return;
  if (mode == Mode::ConnectionTest) {
    runConnectionTest();
    requestUpdate();
    return;
  }
  // One phase per tick keeps the render task responsive during a sync.
  stepOnePhase();
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
