#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "ReadingEventLog.h"
#include "network/BookOrbitBlobStore.h"

class Epub;

/**
 * Records page dwells as BookOrbit reading events for one reading session.
 *
 * Owned by the reader activity: constructed in onEnter(), destroyed in
 * onExit(). That lifetime is load-bearing rather than incidental —
 * ReadingEventLog buffers and only writes every kFlushEveryNEvents events, so a
 * recorder rebuilt per page turn would hold an always-empty buffer and force an
 * SD write on every turn, which `AGENTS.md`'s "debounce persistent writes" rule
 * forbids. The remaining buffered events are flushed once, on destruction.
 *
 * The interesting logic — the clamp rules and layout-independent page numbers —
 * lives in `lib/BookOrbit` and is host-tested. This is only the glue.
 */
class BookOrbitEventRecorder {
 public:
  /** Returns nullptr when BookOrbit is off or the book cannot be hashed. */
  static std::unique_ptr<BookOrbitEventRecorder> create(const std::string& epubPath);

  ~BookOrbitEventRecorder();

  BookOrbitEventRecorder(const BookOrbitEventRecorder&) = delete;
  BookOrbitEventRecorder& operator=(const BookOrbitEventRecorder&) = delete;

  /**
   * @param epub         open book, for reference-page resolution
   * @param spineIndex   current spine item
   * @param spineProgress 0..1 position within that spine item, the same value
   *                      the status bar feeds resolveReferencePage()
   * @param dwellSeconds raw seconds on the page, before clamping
   * @return true when an event was buffered
   */
  bool recordPageDwell(const Epub& epub, int spineIndex, float spineProgress, uint32_t dwellSeconds);

  /** Writes anything still buffered. Called on destruction and at reader exit. */
  bool flush();

 private:
  BookOrbitEventRecorder(std::string hash, std::string logPath);

  std::string bookHash;
  BookOrbitBlobStore blobs;
  bookorbit::ReadingEventLog log;
};
