#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "IFileSink.h"

namespace bookorbit {

// Ceiling for a transfer whose size the server did not record
// (bookorbit_transfer_policy.lua:17).
constexpr size_t kMaxTransferBytes = 512u * 1024u * 1024u;

// Byte cap for one transfer: min(512 MiB, ceil(expected * 1.25) + 1 MiB).
// The margin exists because servers may add container overhead, so the
// recorded size is a bound rather than an exact length.
size_t maxBytesForExpected(size_t expectedBytes);

// "<name>.part" — the temp path a transfer writes to before publication.
std::string partPathFor(std::string_view finalPath);

// Streams a download into "<final>.part" and publishes it with an atomic
// rename. The invariant: the final path only ever appears once the whole body
// has arrived within the cap, so a ".part" left by a battery pull can never be
// mistaken for a complete book.
//
// Holds no buffer of its own. Bytes arrive in the transport's own chunk (2 KB
// for SecureHttpClient) and go straight to the sink, so RAM use is independent
// of file size — the property that keeps a large book safe on an ESP32-C3.
class PartFileWriter {
 public:
  PartFileWriter(IFileSink& sink, std::string finalPath, const size_t maxBytes)
      : sink_(sink),
        finalPath_(std::move(finalPath)),
        partPath_(partPathFor(finalPath_)),
        maxBytes_(maxBytes > 0 ? maxBytes : kMaxTransferBytes) {}

  // Opens the .part file, truncating any leftover from an interrupted run.
  bool begin();

  // Feed one body chunk. Returns false to abort the transfer: the cap would be
  // exceeded, or the card rejected the write. A false return leaves the writer
  // permanently failed — commit() will refuse.
  bool onData(const uint8_t* data, size_t len);

  // Closes the .part and renames it onto the final path. Returns false if the
  // transfer failed, was abandoned, or the rename did not succeed.
  bool commit();

  // Closes the .part and leaves it in place. Used on cancel or transport loss.
  void abandon();

  bool capExceeded() const { return capExceeded_; }
  bool failed() const { return failed_; }
  size_t bytes() const { return bytes_; }
  const std::string& partPath() const { return partPath_; }
  const std::string& finalPath() const { return finalPath_; }

 private:
  IFileSink& sink_;
  std::string finalPath_;
  std::string partPath_;
  size_t maxBytes_;
  size_t bytes_ = 0;
  bool started_ = false;
  bool closed_ = false;
  bool failed_ = false;
  bool capExceeded_ = false;
};

}  // namespace bookorbit
