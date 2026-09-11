#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitBulkProgress.h"
#include "BookOrbitClient.h"
#include "BookOrbitProgress.h"
#include "ProgressMapper.h"
#include "ProgressResolution.h"

class Epub;

/**
 * The BookOrbit progress phase: pushes the reader's position at xpointer
 * fidelity and resolves an inbound one against the book's real spine XHTML.
 *
 * Device-only, because it needs the open Epub to read spine items and to run
 * ProgressMapper. The policy it enforces — never degrade silently — lives in
 * `lib/BookOrbit/ProgressResolution` and is host-tested; the resolver in
 * `lib/BookOrbit/XPointerResolver` likewise.
 */
class BookOrbitProgressPhase {
 public:
  BookOrbitProgressPhase(bookorbit::BookOrbitClient& client, std::shared_ptr<Epub> epub, std::string deviceName,
                         std::string deviceId, uint32_t nowUnix)
      : client(client),
        epub(std::move(epub)),
        deviceName(std::move(deviceName)),
        deviceId(std::move(deviceId)),
        nowUnix(nowUnix) {}

  /** Sends xpointer and percentage together, or refuses to send at all. */
  bool push(const std::string& md5, const CrossPointPosition& position, float percentage);

  /**
   * Fetches the remote position and resolves it. `out.source` reports whether
   * the landing is exact, a percentage fallback, or nothing usable; callers
   * must surface `out.jumpNeedsNotice` rather than jumping silently.
   */
  bool pull(const std::string& md5, float localPercentage, bookorbit::ResolvedProgress& out);

  /** Bulk push for books other than the open one. */
  bool pushBulk(const std::vector<bookorbit::BulkProgressItem>& items, std::vector<std::string>& unmatched);

 private:
  // Streams one spine item's XHTML into a string. Returns false when the index
  // is out of range or the item cannot be read.
  bool readSpineItem(int spineIndex, std::string& out) const;

  // Converts an offset-within-spine-item into a whole-book percentage.
  float spinePercentage(int spineIndex, float withinSpine) const;

  bookorbit::BookOrbitClient& client;
  std::shared_ptr<Epub> epub;
  std::string deviceName;
  std::string deviceId;
  uint32_t nowUnix;
};
