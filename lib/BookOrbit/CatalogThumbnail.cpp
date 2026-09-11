#include "CatalogThumbnail.h"

#include <cstdio>

namespace bookorbit {
namespace {

constexpr char kImagePrefix[] = "image/";
constexpr size_t kImagePrefixLen = sizeof(kImagePrefix) - 1;

char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

bool isImageContentType(std::string_view contentType) {
  while (!contentType.empty() && (contentType.front() == ' ' || contentType.front() == '\t')) {
    contentType.remove_prefix(1);
  }
  if (contentType.size() < kImagePrefixLen + 1) return false;
  for (size_t i = 0; i < kImagePrefixLen; ++i) {
    if (lower(contentType[i]) != kImagePrefix[i]) return false;
  }
  // Require a subtype so "image/" alone, or "imagex/...", cannot slip through.
  const char subtypeFirst = contentType[kImagePrefixLen];
  return subtypeFirst != ';' && subtypeFirst != ' ';
}

std::string thumbnailPath(const uint32_t bookId) {
  // 64 bytes covers the fixed prefix plus a 10-digit id and ".jpg"; well under
  // the 256-byte stack guidance.
  char buffer[64];
  const int written = snprintf(buffer, sizeof(buffer), "/.crosspoint/bookorbit/thumbs/%u.jpg", bookId);
  if (written <= 0) return {};
  return std::string(buffer, static_cast<size_t>(written));
}

bool ThumbnailGate::onHeaders(const std::string_view contentType) {
  if (!isImageContentType(contentType)) {
    rejected_ = true;
    return false;
  }
  if (!writer_.begin()) {
    rejected_ = true;
    return false;
  }
  accepted_ = true;
  return true;
}

bool ThumbnailGate::onData(const uint8_t* data, const size_t len) {
  if (!accepted_ || rejected_) return false;
  return writer_.onData(data, len);
}

}  // namespace bookorbit
