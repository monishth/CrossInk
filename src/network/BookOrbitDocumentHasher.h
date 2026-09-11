#pragma once
#include <string>

#include "CatalogSyncKey.h"

/**
 * IDocumentHasher over the existing KOReaderDocumentId::calculate() — 12
 * offsets at 1024 << 2i, 1024 bytes each. This is deliberately the same code
 * KOSync already uses: the hash it returns is BookOrbit's only book key, so a
 * second implementation would be a second source of truth.
 *
 * Note the spec's first correction: BookOrbit forbids the FILENAME match
 * method, so this never calls calculateFromFilename() regardless of the KOSync
 * setting.
 */
class BookOrbitDocumentHasher final : public bookorbit::IDocumentHasher {
 public:
  std::string partialMd5(std::string_view path) override;
};
