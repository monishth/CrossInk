#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "CatalogDownload.h"

namespace bookorbit {

// Sent as the Accept header on the thumbnail route, matching the Lua client
// (bookorbit_api.lua:603).
constexpr char kThumbnailAccept[] = "image/jpeg,image/*";

// Covers are display assets, not books. Half a megabyte is far more than an
// 800x480 1-bit panel can use and still bounds a misbehaving server.
constexpr size_t kMaxThumbnailBytes = 512u * 1024u;

// True when the media type is image/*. Case-insensitive, tolerant of leading
// space and of parameters after ";". An empty or absent type is false: a
// thumbnail route that does not say it is sending an image is not trusted.
bool isImageContentType(std::string_view contentType);

std::string thumbnailPath(uint32_t bookId);

// Wraps a PartFileWriter with the content-type check. The writer is opened
// only once the headers prove the body is an image, so a login page or a JSON
// error served in place of a cover never creates a .part at all.
class ThumbnailGate {
 public:
  explicit ThumbnailGate(PartFileWriter& writer) : writer_(writer) {}

  // Call once, with the response Content-Type, before any body byte.
  // Returns false to abort the transfer.
  bool onHeaders(std::string_view contentType);

  // Body chunk. Refuses everything until onHeaders has accepted.
  bool onData(const uint8_t* data, size_t len);

  bool rejected() const { return rejected_; }

 private:
  PartFileWriter& writer_;
  bool accepted_ = false;
  bool rejected_ = false;
};

}  // namespace bookorbit
