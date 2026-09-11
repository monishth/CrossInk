#include "CatalogApi.h"

#include "CatalogDecode.h"
#include "CatalogMutations.h"

namespace bookorbit {
namespace {

constexpr char kCatalogBase[] = "/koreader/plugin/catalog";

std::string catalogPath(const std::string_view suffix) {
  std::string path(kCatalogBase);
  path.append(suffix);
  return path;
}

}  // namespace

Error CatalogApi::getBody(const std::string_view path, std::string& outBody) { return client_.get(path, outBody); }

Error CatalogApi::root(CatalogEntryPage& out) {
  std::string body;
  const Error error = getBody(catalogPath("/root"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::dashboard(DashboardSummary& out) {
  std::string body;
  const Error error = getBody(catalogPath("/dashboard"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeDashboard(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::discover(CatalogEntryPage& out) {
  std::string body;
  const Error error = getBody(catalogPath("/dashboard/discover"), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::dashboardSection(const std::string_view type, CatalogEntryPage& out) {
  if (capabilities_.get(kCapDashboardSections) == Capability::Unsupported) {
    // Known absent: skip the round trip entirely rather than collect a 404 on
    // every dashboard build.
    return {Status::NotFound, 404};
  }

  std::string path = catalogPath("/dashboard/sections/");
  path += urlEncodeComponent(type);

  std::string body;
  const Error error = getBody(path, body);
  if (error.status == Status::NotFound) {
    // A definitive 404 on the feature's own route is a confirmed downgrade.
    capabilities_.markUnsupported(kCapDashboardSections);
    return error;
  }
  // Any 5xx or transport failure falls through untouched: the tri-state stays
  // Unknown (or keeps whatever /version said), never caching a negative.
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::section(const std::string_view section, const CatalogQuery& query, CatalogEntryPage& out) {
  std::string path = catalogPath("/sections/");
  path += urlEncodeComponent(section);

  std::string body;
  const Error error = getBody(query.build(path), body);
  if (error.status != Status::Ok) return error;
  if (!decodeEntryPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::books(const CatalogQuery& query, CatalogPage& out) {
  std::string body;
  const Error error = getBody(query.build(catalogPath("/books")), body);
  if (error.status != Status::Ok) return error;
  if (!decodeBookPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error CatalogApi::setReadStatus(const uint32_t bookId, const ReadStatus status) {
  std::string body;
  return client_.putJson(readStatusPath(bookId), encodeReadStatus(status), body);
}

Error CatalogApi::setRating(const uint32_t bookId, const int rating) {
  std::string body;
  return client_.putJson(ratingPath(bookId), encodeRating(rating), body);
}

}  // namespace bookorbit
