#pragma once

#include <string>
#include <string_view>

#include "BookOrbitCapabilities.h"
#include "BookOrbitClient.h"
#include "BookOrbitError.h"
#include "CatalogMutations.h"
#include "CatalogQuery.h"
#include "CatalogTypes.h"

namespace bookorbit {

// The one capability P5 gates on. Servers older than the dashboard-sections
// feature answer its route with 404.
constexpr char kCapDashboardSections[] = "catalogDashboardSections";

// Read side of the BookOrbit catalog. Every method issues one GET through the
// P0 client and decodes the body with the streaming decoders, so no response
// is ever held whole in memory.
class CatalogApi {
 public:
  CatalogApi(BookOrbitClient& client, CapabilityCache& capabilities) : client_(client), capabilities_(capabilities) {}

  Error root(CatalogEntryPage& out);
  Error dashboard(DashboardSummary& out);
  Error discover(CatalogEntryPage& out);

  // Capability-gated on kCapDashboardSections. Returns {NotFound, 404} without
  // issuing a request when the capability is known-Unsupported. A 404 from the
  // route downgrades the capability; a 5xx or transport failure leaves it
  // Unknown, because a blip carries no information about server support.
  Error dashboardSection(std::string_view type, CatalogEntryPage& out);

  Error section(std::string_view section, const CatalogQuery& query, CatalogEntryPage& out);
  Error books(const CatalogQuery& query, CatalogPage& out);

  // Exposed so CatalogManifest (Task 5) can reuse the same request plumbing.
  Error getBody(std::string_view path, std::string& outBody);

  // Write side. Both routes answer with a small JSON envelope that the catalog
  // UI does not need, so the body is decoded only far enough to report status.
  Error setReadStatus(uint32_t bookId, ReadStatus status);

  // rating <= 0 clears the rating explicitly; 1..5 sets it.
  Error setRating(uint32_t bookId, int rating);

 private:
  BookOrbitClient& client_;
  CapabilityCache& capabilities_;
};

}  // namespace bookorbit
