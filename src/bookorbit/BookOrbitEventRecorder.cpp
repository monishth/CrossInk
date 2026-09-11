#include "BookOrbitEventRecorder.h"

#include <HalClock.h>
#include <Logging.h>

#include <ctime>

#include "ClampPolicy.h"
#include "Epub.h"
#include "Memory.h"
#include "PageResolver.h"
#include "ReadingEvent.h"
#include "network/BookOrbitCredentialStore.h"
#include "network/BookOrbitDocumentHasher.h"

namespace {

constexpr char kModule[] = "BORB";

// One log per book, beside the book's existing cache.
std::string logPathFor(const std::string& hash) { return "/.crosspoint/bookorbit_events_" + hash + ".bin"; }

}  // namespace

BookOrbitEventRecorder::BookOrbitEventRecorder(std::string hash, std::string logPath)
    : bookHash(std::move(hash)), log(blobs, std::move(logPath)) {}

BookOrbitEventRecorder::~BookOrbitEventRecorder() { flush(); }

std::unique_ptr<BookOrbitEventRecorder> BookOrbitEventRecorder::create(const std::string& epubPath) {
  if (!BOOKORBIT_STORE.isEnabled()) return nullptr;

  BookOrbitDocumentHasher hasher;
  const std::string hash = hasher.partialMd5(epubPath);
  if (hash.empty()) {
    LOG_ERR(kModule, "cannot hash %s; not recording events", epubPath.c_str());
    return nullptr;
  }

  // Fallible allocation: `new` is not nothrow on ESP32.
  auto* raw = new (std::nothrow) BookOrbitEventRecorder(hash, logPathFor(hash));
  if (raw == nullptr) {
    LOG_ERR(kModule, "OOM allocating event recorder");
    return nullptr;
  }
  return std::unique_ptr<BookOrbitEventRecorder>(raw);
}

bool BookOrbitEventRecorder::recordPageDwell(Epub& epub, const int spineIndex, const float spineProgress,
                                             const uint32_t dwellSeconds) {
  // Events require wall-clock time. Rather than fabricate a startTime — which
  // would corrupt every derived statistic irreversibly — the event is dropped.
  if (!halClock.isAvailable()) return false;
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute) || year == 0) return false;

  // KOReader's rules: short dwells discarded, long ones clamped not dropped.
  const bookorbit::ClampSettings clamp;
  uint16_t duration = 0;
  if (!bookorbit::clampDwell(dwellSeconds, clamp, duration)) return false;

  // Layout-independent page numbers, so events stay comparable across font and
  // margin changes.
  bookorbit::PageSource source;
  source.hasStablePages = epub.hasStablePageNumbers();
  if (source.hasStablePages) {
    uint32_t page = 0;
    uint32_t count = 0;
    if (epub.resolveReferencePage(spineIndex, spineProgress, page, count)) {
      source.referencePage = page;
      source.referencePageCount = count;
    } else {
      source.hasStablePages = false;
    }
  }
  if (!source.hasStablePages) {
    source.sizeProgress = epub.calculateSizeProgress(spineIndex, spineProgress);
    source.bookSize = epub.getBookSize();
  }

  uint32_t page = 0;
  uint16_t totalPages = 0;
  if (!bookorbit::resolvePage(source, page, totalPages)) return false;

  bookorbit::ReadingEvent event;
  event.page = page;
  event.totalPages = totalPages;
  event.durationSeconds = duration;
  // The dwell just ended, so it began `duration` seconds ago.
  const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
  event.startTime = now > duration ? now - duration : now;

  // append() buffers and writes only every kFlushEveryNEvents, which is what
  // keeps page turns off the SD card.
  log.append(event);
  LOG_DBG(kModule, "event page=%u/%u dur=%u", static_cast<unsigned>(page), static_cast<unsigned>(totalPages),
          static_cast<unsigned>(duration));
  return true;
}

bool BookOrbitEventRecorder::flush() { return log.flush(); }
