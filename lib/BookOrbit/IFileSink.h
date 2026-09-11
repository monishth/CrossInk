#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bookorbit {

// Narrow file interface for streamed transfers. Deliberately smaller than
// IBlobStore (P0 Task 4): a download is written incrementally and never held in
// a buffer, so this exposes an open/write/close cursor rather than a
// whole-blob write.
class IFileSink {
 public:
  virtual ~IFileSink() = default;

  // Opens `path` for writing, truncating anything already there.
  virtual bool open(std::string_view path) = 0;
  virtual bool write(const uint8_t* data, size_t len) = 0;
  // Flushes and closes the current file. Safe to call when nothing is open.
  virtual bool close() = 0;
  // Atomic rename. `from` must be closed first.
  virtual bool publish(std::string_view from, std::string_view to) = 0;
  virtual bool remove(std::string_view path) = 0;
  virtual bool exists(std::string_view path) = 0;
};

}  // namespace bookorbit
