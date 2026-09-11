#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "IBlobStore.h"

namespace bookorbit {

// Writes a blob so that a crash or power loss can never leave a torn file.
// Sequence mirrors GlobalReadingStats.cpp:197-267 — write <path>.tmp, verify,
// rotate <path> to <path>.bak, rename <path>.tmp to <path>.
bool atomicWriteBlob(IBlobStore& store, std::string_view path, const uint8_t* data, size_t len);

// Reads <path>, falling back to <path>.bak when the primary is missing.
bool readBlobWithBackup(IBlobStore& store, std::string_view path, std::vector<uint8_t>& out);

}  // namespace bookorbit
