#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitCapabilities.h"
#include "lib/BookOrbit/BookOrbitError.h"

using bookorbit::Capability;
using bookorbit::CapabilityCache;
using bookorbit::classify;

TEST(BookOrbitCapabilities, StartsUnknown) {
  CapabilityCache cache;
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

TEST(BookOrbitCapabilities, AdvertisedNamesBecomeSupported) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync", "catalogDashboardSections"});
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Supported);
  EXPECT_EQ(cache.get("catalogDashboardSections"), Capability::Supported);
}

// A successful /version response that omits a name is a definitive negative.
TEST(BookOrbitCapabilities, OmittedNameBecomesUnsupported) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"catalogDashboardSections"});
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

// The critical rule: a blip must never permanently disable a feature.
TEST(BookOrbitCapabilities, ServerErrorLeavesUnknown) {
  CapabilityCache cache;
  cache.rememberFailure(classify(503, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

TEST(BookOrbitCapabilities, TransportFailureLeavesUnknown) {
  CapabilityCache cache;
  cache.rememberFailure(classify(0, true));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

// A definitive 4xx on /version means the server has no capability endpoint.
TEST(BookOrbitCapabilities, ClientErrorMarksEverythingUnsupported) {
  CapabilityCache cache;
  cache.rememberFailure(classify(404, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

TEST(BookOrbitCapabilities, ConfirmedRouteFailureDowngradesOneCapability) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.markUnsupported("bookmarkSync");
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unsupported);
}

TEST(BookOrbitCapabilities, InvalidateReturnsToUnknown) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.invalidate();
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Unknown);
}

// A transient failure after a good response must not erase what we learned.
TEST(BookOrbitCapabilities, TransientFailureDoesNotClearKnownState) {
  CapabilityCache cache;
  cache.rememberFromVersionResponse({"bookmarkSync"});
  cache.rememberFailure(classify(500, false));
  EXPECT_EQ(cache.get("bookmarkSync"), Capability::Supported);
}
