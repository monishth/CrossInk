#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "CatalogDownload.h"
#include "CatalogManifest.h"
#include "IFileSink.h"

namespace bookorbit {

// Injected hasher. The device binding wraps KOReaderDocumentId::calculate()
// (lib/KOReaderSync/KOReaderDocumentId.h:27), which already implements
// KOReader's partial MD5 exactly: 12 offsets at 1024 << 2i, 1024 bytes each.
// Injecting it keeps this logic host-testable without an SD card.
class IDocumentHasher {
 public:
  virtual ~IDocumentHasher() = default;
  // Returns a 32-character lowercase hex digest, or "" on failure.
  virtual std::string partialMd5(std::string_view path) = 0;
};

// A book that finished downloading and now has a sync key.
//
// `hash` is BookOrbit's only book identity: it is MatchCandidate.hash in P0's
// match-check, the key of BookSyncState, and the "hash" field of P1's
// page-stats, P2's progress, P3's book-states and P4's exchanges. Producing it
// here is what makes a freshly downloaded book immediately syncable.
struct DownloadedBook {
  std::string path;
  std::string hash;
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  size_t bytes = 0;
  // The server's own hash disagreed with the device's. Diagnostic only: the
  // device hash is always the one used, because it describes the bytes that
  // are actually on this card.
  bool serverHashMismatch = false;
};

// True for exactly 32 lowercase hex digits — the shape KOReaderDocumentId emits
// and the only shape BookOrbit accepts as a book key.
bool isPartialMd5(std::string_view value);

// Publishes the completed transfer and hashes the published file.
//
// Order matters: publish first, then hash the final path. Hashing the .part
// would key the book by a path that is about to disappear, and a failed publish
// would mint a sync key for a book that is not on the device.
//
// On an empty or malformed digest the published file is removed: a book the
// device cannot identify can never be synced by any later phase, so leaving it
// would create a permanently invisible library entry.
bool finalizeDownload(IFileSink& sink, IDocumentHasher& hasher, PartFileWriter& writer, const ManifestItem& item,
                      DownloadedBook& out);

}  // namespace bookorbit
