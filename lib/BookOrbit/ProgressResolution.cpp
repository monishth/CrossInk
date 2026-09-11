#include "ProgressResolution.h"

#include <cmath>

#include "XPointer.h"

namespace bookorbit {

bool isSendable(const ProgressRecord& record) {
  if (record.document.empty()) return false;
  if (record.percentage < 0.0f || record.percentage > 1.0f) return false;
  return !normalizeXPointer(record.progress).empty();
}

ResolvedProgress chooseRemoteProgress(const ProgressRecord& remote, const bool xpointerResolved,
                                      const float resolvedPercentage, const float localPercentage) {
  ResolvedProgress result;

  const std::string normalized = normalizeXPointer(remote.progress);

  if (xpointerResolved && !normalized.empty()) {
    result.source = ProgressSource::Xpointer;
    result.xpointer = normalized;
    result.percentage = resolvedPercentage;
    // An exact position is never a degradation, however far it moves.
    return result;
  }

  // Nothing usable arrived: no xpointer we could resolve and no percentage to
  // fall back on. A timestamp alone conveys no position, so it must not keep
  // this record alive — syncing to 0% would throw the reader back to the cover.
  if (normalized.empty() && remote.percentage <= 0.0f) {
    return result;
  }

  result.source = ProgressSource::Percentage;
  result.degraded = true;
  result.percentage = remote.percentage;
  result.xpointer = normalized;  // kept for logging even though it did not resolve
  result.jumpNeedsNotice = std::fabs(remote.percentage - localPercentage) > kDegradedJumpThreshold;
  return result;
}

}  // namespace bookorbit
