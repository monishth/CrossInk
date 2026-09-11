#include "BookOrbitDocumentHasher.h"

#include <Logging.h>

#include "KOReaderDocumentId.h"

std::string BookOrbitDocumentHasher::partialMd5(const std::string_view path) {
  // string_view::data() is not null-terminated; KOReaderDocumentId takes a
  // std::string, so materialize once here.
  const std::string filePath(path);
  std::string hash = KOReaderDocumentId::calculate(filePath);
  if (hash.empty()) {
    LOG_ERR("BOHASH", "Partial MD5 failed for %s", filePath.c_str());
  }
  return hash;
}
