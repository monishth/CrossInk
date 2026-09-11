#include "CatalogSyncKey.h"

namespace bookorbit {
namespace {

bool isLowerHex(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

constexpr size_t kPartialMd5Length = 32;

}  // namespace

bool isPartialMd5(const std::string_view value) {
  if (value.size() != kPartialMd5Length) return false;
  for (const char c : value) {
    if (!isLowerHex(c)) return false;
  }
  return true;
}

bool finalizeDownload(IFileSink& sink, IDocumentHasher& hasher, PartFileWriter& writer, const ManifestItem& item,
                      DownloadedBook& out) {
  out = DownloadedBook{};

  // A failed or abandoned transfer is not a book. Its .part stays on disk and
  // is never hashed, so nothing downstream can mistake it for one.
  if (!writer.commit()) return false;

  const std::string hash = hasher.partialMd5(writer.finalPath());
  if (!isPartialMd5(hash)) {
    // Unidentifiable: remove it rather than leave a book no phase can sync.
    sink.remove(writer.finalPath());
    return false;
  }

  out.path = writer.finalPath();
  out.hash = hash;
  out.bookId = item.bookId;
  out.fileId = item.fileId;
  out.bytes = writer.bytes();
  out.serverHashMismatch = !item.hash.empty() && item.hash != hash;
  return true;
}

}  // namespace bookorbit
