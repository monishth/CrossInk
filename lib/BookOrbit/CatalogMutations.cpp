#include "CatalogMutations.h"

#include <cstdio>

namespace bookorbit {
namespace {

// 96 bytes covers the fixed route plus a 10-digit id; under the 256-byte
// stack guidance.
constexpr size_t kPathScratch = 96;

std::string formatPath(const char* format, const uint32_t bookId) {
  char buffer[kPathScratch];
  const int written = snprintf(buffer, sizeof(buffer), format, bookId);
  if (written <= 0) return {};
  return std::string(buffer, static_cast<size_t>(written));
}

}  // namespace

const char* readStatusToWire(const ReadStatus status) {
  switch (status) {
    case ReadStatus::Reading:
      return "reading";
    case ReadStatus::Finished:
      return "finished";
    case ReadStatus::Abandoned:
      return "abandoned";
    case ReadStatus::Unread:
    default:
      return "unread";
  }
}

bool readStatusFromWire(const std::string_view wire, ReadStatus& out) {
  if (wire == "unread") {
    out = ReadStatus::Unread;
  } else if (wire == "reading") {
    out = ReadStatus::Reading;
  } else if (wire == "finished") {
    out = ReadStatus::Finished;
  } else if (wire == "abandoned") {
    out = ReadStatus::Abandoned;
  } else {
    return false;
  }
  return true;
}

std::string encodeReadStatus(const ReadStatus status) {
  std::string json = R"({"status":")";
  json += readStatusToWire(status);
  json += R"("})";
  return json;
}

std::string encodeRating(const int rating) {
  if (rating <= 0) return R"({"rating":null})";
  const int clamped = rating > 5 ? 5 : rating;
  std::string json = R"({"rating":)";
  json += static_cast<char>('0' + clamped);
  json += '}';
  return json;
}

std::string readStatusPath(const uint32_t bookId) {
  return formatPath("/koreader/plugin/catalog/books/%u/read-status", bookId);
}

std::string ratingPath(const uint32_t bookId) { return formatPath("/koreader/plugin/catalog/books/%u/rating", bookId); }

}  // namespace bookorbit
