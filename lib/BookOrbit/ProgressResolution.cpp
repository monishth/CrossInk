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
  result.remoteTimestamp = remote.timestamp;
  result.remoteDeviceId = remote.deviceId;

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

ProgressAction decideProgressAction(const ResolvedProgress& remote, const uint32_t localReadAt,
                                    const std::string_view thisDeviceId) {
  // Nothing came back, or it is this device's own last push echoed at us.
  // Either way the reader here is the authority.
  if (remote.source == ProgressSource::None) return ProgressAction::PushLocal;
  if (!remote.remoteDeviceId.empty() && remote.remoteDeviceId == thisDeviceId) return ProgressAction::PushLocal;

  // An undated record says nothing about who read last, so it cannot outrank
  // reading we can actually date.
  if (remote.remoteTimestamp <= localReadAt) return ProgressAction::PushLocal;

  // Newer, but only a percentage: there is no spine item or offset to restore
  // from, so the reader cannot be moved to it. Pushing would replace a real
  // position with an older one, so hold and let the reader be told.
  if (remote.source != ProgressSource::Xpointer || remote.spineIndex < 0) return ProgressAction::HoldBoth;

  return ProgressAction::ApplyRemote;
}

}  // namespace bookorbit
