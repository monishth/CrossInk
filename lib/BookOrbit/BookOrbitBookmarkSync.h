#pragma once

#include <cstdint>
#include <string_view>

#include "BookOrbitAnnotationSync.h"
#include "BookOrbitBookmarkModel.h"
#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "BookOrbitSyncState.h"

namespace bookorbit {

// The capability name the server advertises from /koreader/plugin/version.
inline constexpr char kBookmarkCapability[] = "bookmarkSync";

// Position-only bookmarks (dogears) over the same three-legged exchange.
//
// Capability handling is tri-state and deliberately asymmetric:
//   Unsupported -> skipped without a request (outcome.skipped)
//   Unknown     -> tried; a new plugin on an old server learns from the 404
//   Supported   -> tried
// Only Status::NotFound on this route calls markUnsupported. A 5xx or a
// transport failure leaves the capability Unknown, because it says nothing
// about whether the server supports bookmarks.
Error exchangeBookmarks(BookOrbitClient& client, CapabilityCache& capabilities, BookSyncState& book,
                        std::string_view hash, const NormalizedBookmarks& local, IAnnotationApplier& applier,
                        uint32_t nowUnix, ExchangeOutcome& outcome);

}  // namespace bookorbit
