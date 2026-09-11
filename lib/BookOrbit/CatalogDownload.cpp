#include "CatalogDownload.h"

namespace bookorbit {
namespace {

// Servers may add container overhead, so the recorded size is a margin rather
// than an exact ceiling (bookorbit_transfer_policy.lua:19-21).
constexpr size_t kSizeMarginNumerator = 5;    // 1.25 == 5/4, done in integers so
constexpr size_t kSizeMarginDenominator = 4;  // no float rounding creeps in
constexpr size_t kSizeSlackBytes = 1024u * 1024u;

}  // namespace

size_t maxBytesForExpected(const size_t expectedBytes) {
  if (expectedBytes == 0) return kMaxTransferBytes;
  // Guard the multiply before it happens rather than detecting the wrap after.
  if (expectedBytes > kMaxTransferBytes / kSizeMarginNumerator) return kMaxTransferBytes;
  const size_t scaled = (expectedBytes * kSizeMarginNumerator + kSizeMarginDenominator - 1) / kSizeMarginDenominator;
  if (scaled > kMaxTransferBytes - kSizeSlackBytes) return kMaxTransferBytes;
  return scaled + kSizeSlackBytes;
}

std::string partPathFor(const std::string_view finalPath) {
  std::string path(finalPath);
  path += ".part";
  return path;
}

bool PartFileWriter::begin() {
  if (started_) return false;
  // open() truncates, so a .part left behind by an interrupted run is
  // discarded rather than appended to.
  if (!sink_.open(partPath_)) {
    failed_ = true;
    return false;
  }
  started_ = true;
  return true;
}

bool PartFileWriter::onData(const uint8_t* data, const size_t len) {
  if (!started_ || closed_ || failed_) return false;
  if (len == 0) return true;

  // Checked before the write: not one byte past the cap reaches the card.
  if (len > maxBytes_ - bytes_) {
    capExceeded_ = true;
    failed_ = true;
    return false;
  }
  if (!sink_.write(data, len)) {
    failed_ = true;
    return false;
  }
  bytes_ += len;
  return true;
}

bool PartFileWriter::commit() {
  if (!started_ || failed_) {
    return false;
  }
  if (!closed_) {
    sink_.close();
    closed_ = true;
  }
  if (!sink_.publish(partPath_, finalPath_)) {
    // The .part stays on disk; the final path never appeared, so nothing
    // downstream can mistake this for a complete book.
    failed_ = true;
    return false;
  }
  return true;
}

void PartFileWriter::abandon() {
  if (started_ && !closed_) {
    sink_.close();
    closed_ = true;
  }
  failed_ = true;
}

}  // namespace bookorbit
