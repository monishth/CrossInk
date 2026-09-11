#include "BookOrbitSyncState.h"

#include <cstring>

#include "AtomicBlobWriter.h"

namespace bookorbit {
namespace {

template <size_t N>
void copyInto(char (&dest)[N], const std::string_view value) {
  const size_t len = value.size() < (N - 1) ? value.size() : (N - 1);
  std::memcpy(dest, value.data(), len);
  dest[len] = '\0';
}

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool readU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& value) {
  if (pos + 4 > in.size()) return false;
  value = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
  pos += 4;
  return true;
}

void appendStr(std::vector<uint8_t>& out, const char* value) {
  const size_t len = std::strlen(value);
  out.push_back(static_cast<uint8_t>(len));
  out.insert(out.end(), value, value + len);
}

template <size_t N>
bool readStr(const std::vector<uint8_t>& in, size_t& pos, char (&dest)[N]) {
  if (pos >= in.size()) return false;
  const size_t len = in[pos++];
  if (pos + len > in.size() || len >= N) return false;
  std::memcpy(dest, in.data() + pos, len);
  dest[len] = '\0';
  pos += len;
  return true;
}

}  // namespace

void BookSyncState::setMd5(const std::string_view value) { copyInto(md5, value); }
void BookSyncState::setMatchVerifiedVersion(const std::string_view value) { copyInto(matchVerifiedVersion, value); }

SyncStateStore::SyncStateStore(IBlobStore& store, std::string statePath) : blobs(store), path(std::move(statePath)) {}

bool SyncStateStore::load() {
  books.clear();
  library.clear();

  std::vector<uint8_t> raw;
  if (!readBlobWithBackup(blobs, path, raw)) {
    return true;  // nothing persisted yet
  }

  size_t pos = 0;
  if (raw.empty() || raw[pos++] != kFormatVersion) {
    return true;  // unknown or corrupt format: start clean
  }

  char libraryBuf[24] = {};
  if (!readStr(raw, pos, libraryBuf)) return true;
  library = libraryBuf;

  uint32_t count = 0;
  if (!readU32(raw, pos, count)) return true;

  for (uint32_t i = 0; i < count; i++) {
    BookSyncState book;
    if (!readStr(raw, pos, book.md5)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.statsWatermark)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.matchVerifiedAt)) {
      books.clear();
      return true;
    }
    if (!readStr(raw, pos, book.matchVerifiedVersion)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.bookId)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.fileId)) {
      books.clear();
      return true;
    }
    uint32_t pctBits = 0;
    if (!readU32(raw, pos, pctBits)) {
      books.clear();
      return true;
    }
    std::memcpy(&book.progressPushedPct, &pctBits, sizeof(float));
    if (!readStr(raw, pos, book.annSignature)) {
      books.clear();
      return true;
    }
    if (!readStr(raw, pos, book.bmSignature)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.annExchangedAt)) {
      books.clear();
      return true;
    }
    if (!readU32(raw, pos, book.bmExchangedAt)) {
      books.clear();
      return true;
    }
    if (!readStr(raw, pos, book.statusSyncedModified)) {
      books.clear();
      return true;
    }
    books.push_back(book);
  }
  return true;
}

bool SyncStateStore::flush() {
  std::vector<uint8_t> raw;
  raw.reserve(64 + books.size() * 160);
  raw.push_back(kFormatVersion);
  appendStr(raw, library.c_str());
  appendU32(raw, static_cast<uint32_t>(books.size()));

  for (const auto& book : books) {
    appendStr(raw, book.md5);
    appendU32(raw, book.statsWatermark);
    appendU32(raw, book.matchVerifiedAt);
    appendStr(raw, book.matchVerifiedVersion);
    appendU32(raw, book.bookId);
    appendU32(raw, book.fileId);
    uint32_t pctBits = 0;
    std::memcpy(&pctBits, &book.progressPushedPct, sizeof(float));
    appendU32(raw, pctBits);
    appendStr(raw, book.annSignature);
    appendStr(raw, book.bmSignature);
    appendU32(raw, book.annExchangedAt);
    appendU32(raw, book.bmExchangedAt);
    appendStr(raw, book.statusSyncedModified);
  }

  return atomicWriteBlob(blobs, path, raw.data(), raw.size());
}

BookSyncState* SyncStateStore::find(const std::string_view md5) {
  for (auto& book : books) {
    if (md5 == book.md5) return &book;
  }
  return nullptr;
}

BookSyncState& SyncStateStore::findOrCreate(const std::string_view md5) {
  if (auto* existing = find(md5)) return *existing;
  BookSyncState book;
  book.setMd5(md5);
  books.push_back(book);
  return books.back();
}

void SyncStateStore::setLibraryVersion(const std::string_view value) { library = std::string(value); }

bool SyncStateStore::isMatchFresh(const BookSyncState& book, const uint32_t nowUnix) const {
  if (book.matchVerifiedAt == 0) return false;
  if (nowUnix < book.matchVerifiedAt) return false;  // clock went backwards
  if (nowUnix - book.matchVerifiedAt >= kMatchMaxAgeSeconds) return false;
  return library == book.matchVerifiedVersion;
}

}  // namespace bookorbit
