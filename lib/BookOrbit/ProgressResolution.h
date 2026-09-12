#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

  // Where the resolved xpointer actually lands, in the reader's own terms.
  // Only meaningful when source == Xpointer; a percentage landing has no spine
  // item to name. The offset is a character offset into the spine item's
  // visible text, which is the coordinate the reader restores from when a
  // position arrives from a device with a different layout.
  int spineIndex = -1;
  uint32_t visibleTextOffset = 0;

  // Carried through so the caller can decide whether this record should win:
  // who wrote it and when. A record this device wrote is never applied back to
  // it -- that would undo local reading with our own stale push.
  uint32_t remoteTimestamp = 0;
  std::string remoteDeviceId;
};

// Outbound guard. A record is sendable only when it carries BOTH a parseable
// xpointer and an in-range percentage. Half a record is silent degradation.
bool isSendable(const ProgressRecord& record);

// Inbound choice. xpointerResolved says whether the local resolver turned
// remote.progress into a real position, and resolvedPercentage is where that
// position sits; localPercentage is where the reader currently is.
ResolvedProgress chooseRemoteProgress(const ProgressRecord& remote, bool xpointerResolved, float resolvedPercentage,
                                      float localPercentage);

// What the progress phase should do once it has seen the server's record.
enum class ProgressAction : uint8_t {
  PushLocal,    // this device is at least as current; upload where the reader is
  ApplyRemote,  // another device read further on; move the reader to it
  HoldBoth,     // the remote is newer but unusable here -- overwriting it would
                // lose a real position, so neither side moves
};

/**
 * Chooses a direction for one book.
 *
 * `localReadAt` is when this device last read this book (the newest reading
 * event's start time), NOT when it last pushed: a push stamp would call a
 * device current merely because it synced, and the whole point is to notice
 * that the reading happened elsewhere.
 *
 * A record this device wrote is never applied back to it. With no local
 * reading recorded there is nothing to protect, so a usable remote wins.
 */
ProgressAction decideProgressAction(const ResolvedProgress& remote, uint32_t localReadAt,
                                    std::string_view thisDeviceId);

}  // namespace bookorbit
