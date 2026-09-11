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
