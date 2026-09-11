#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bookorbit {

// Byte-blob persistence, injected so the BookOrbit core stays host-testable.
// The device implementation wraps FsFile; tests use an in-memory fake.
class IBlobStore {
 public:
  virtual ~IBlobStore() = default;

  virtual bool read(std::string_view path, std::vector<uint8_t>& out) = 0;
  virtual bool write(std::string_view path, const uint8_t* data, size_t len) = 0;
  // Appends to the end of a file, creating it when absent. Separate from
  // write() because the event log must never rewrite what it already wrote.
  virtual bool append(std::string_view path, const uint8_t* data, size_t len) = 0;
  virtual bool rename(std::string_view from, std::string_view to) = 0;
  virtual bool remove(std::string_view path) = 0;
  virtual bool exists(std::string_view path) = 0;
};

}  // namespace bookorbit
