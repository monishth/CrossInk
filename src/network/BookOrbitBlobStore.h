#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IBlobStore.h"

/**
 * IBlobStore over the HAL storage layer, backing BookOrbit's durable sync
 * state, event log and book-state records.
 *
 * Every method opens, uses and explicitly closes a single FsFile. On real
 * hardware only one reader may hold a given path open at a time, so no handle
 * is ever cached across calls.
 */
class BookOrbitBlobStore final : public bookorbit::IBlobStore {
 public:
  BookOrbitBlobStore() = default;

  bool read(std::string_view path, std::vector<uint8_t>& out) override;
  bool write(std::string_view path, const uint8_t* data, size_t len) override;
  bool append(std::string_view path, const uint8_t* data, size_t len) override;
  bool rename(std::string_view from, std::string_view to) override;
  bool remove(std::string_view path) override;
  bool exists(std::string_view path) override;
};
