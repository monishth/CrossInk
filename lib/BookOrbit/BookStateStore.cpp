#include "BookStateStore.h"

#include <cstring>

#include "AtomicBlobWriter.h"

namespace bookorbit {
namespace {

void appendU8(std::vector<uint8_t>& out, const uint8_t value) { out.push_back(value); }

void appendU32(std::vector<uint8_t>& out, const uint32_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

bool readU8(const std::vector<uint8_t>& in, size_t& pos, uint8_t& value) {
  if (pos >= in.size()) return false;
  value = in[pos++];
  return true;
}

bool readU32(const std::vector<uint8_t>& in, size_t& pos, uint32_t& value) {
  if (pos + 4 > in.size()) return false;
  value = static_cast<uint32_t>(in[pos]) | (static_cast<uint32_t>(in[pos + 1]) << 8) |
          (static_cast<uint32_t>(in[pos + 2]) << 16) | (static_cast<uint32_t>(in[pos + 3]) << 24);
  pos += 4;
  return true;
}

void appendDate(std::vector<uint8_t>& out, const DateOnly& date) {
  appendU32(out, date.year);
  appendU8(out, date.month);
  appendU8(out, date.day);
}

bool readDate(const std::vector<uint8_t>& in, size_t& pos, DateOnly& date) {
  uint32_t year = 0;
  if (!readU32(in, pos, year)) return false;
  if (!readU8(in, pos, date.month)) return false;
  if (!readU8(in, pos, date.day)) return false;
  date.year = static_cast<uint16_t>(year);
  return true;
}

// Review notes reach 10000 bytes, so the length prefix is 32-bit rather than
// the single byte BookOrbitSyncState uses for its short fixed fields.
void appendString(std::vector<uint8_t>& out, const std::string& value) {
  appendU32(out, static_cast<uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

bool readString(const std::vector<uint8_t>& in, size_t& pos, std::string& value) {
  uint32_t len = 0;
  if (!readU32(in, pos, len)) return false;
  if (len > kReviewNoteMaxBytes) return false;
  if (pos + len > in.size()) return false;
  value.assign(reinterpret_cast<const char*>(in.data() + pos), len);
  pos += len;
  return true;
}

bool readFixed(const std::vector<uint8_t>& in, size_t& pos, char (&dest)[33]) {
  uint8_t len = 0;
  if (!readU8(in, pos, len)) return false;
  if (len >= sizeof(dest) || pos + len > in.size()) return false;
  std::memcpy(dest, in.data() + pos, len);
  dest[len] = '\0';
  pos += len;
  return true;
}

}  // namespace

BookStateStore::BookStateStore(IBlobStore& blobStore, std::string statePath)
    : blobs(blobStore), path(std::move(statePath)) {}

bool BookStateStore::load() {
  records.clear();

  std::vector<uint8_t> raw;
  if (!readBlobWithBackup(blobs, path, raw)) {
    return true;  // nothing persisted yet
  }

  size_t pos = 0;
  uint8_t version = 0;
  if (!readU8(raw, pos, version) || version != kFormatVersion) {
    return true;  // unknown or corrupt format: start clean
  }

  uint32_t count = 0;
  if (!readU32(raw, pos, count)) return true;

  for (uint32_t i = 0; i < count; i++) {
    BookStateRecord record;
    uint8_t statusKnown = 0;
    uint8_t status = 0;
    uint8_t ratingSet = 0;
    uint8_t reviewSet = 0;
    uint8_t syncedRatingKnown = 0;
    uint8_t syncedRatingSet = 0;
    uint8_t syncedReviewKnown = 0;
    uint8_t syncedReviewSet = 0;

    const bool ok = readFixed(raw, pos, record.md5) && readU8(raw, pos, statusKnown) && readU8(raw, pos, status) &&
                    readDate(raw, pos, record.local.statusModified) && readU8(raw, pos, ratingSet) &&
                    readU8(raw, pos, record.local.rating) && readU8(raw, pos, reviewSet) &&
                    readString(raw, pos, record.local.reviewNote) && readDate(raw, pos, record.local.reviewModified) &&
                    readU8(raw, pos, syncedRatingKnown) && readU8(raw, pos, syncedRatingSet) &&
                    readU8(raw, pos, record.synced.rating) && readU8(raw, pos, syncedReviewKnown) &&
                    readU8(raw, pos, syncedReviewSet) && readString(raw, pos, record.synced.reviewNote) &&
                    readDate(raw, pos, record.synced.statusSyncedModified) && readU32(raw, pos, record.statePulledAt);
    if (!ok) {
      records.clear();
      return true;
    }

    record.local.statusKnown = statusKnown != 0;
    record.local.status =
        status <= static_cast<uint8_t>(BookStatus::Abandoned) ? static_cast<BookStatus>(status) : BookStatus::Reading;
    record.local.ratingSet = ratingSet != 0;
    record.local.reviewSet = reviewSet != 0;
    record.synced.ratingKnown = syncedRatingKnown != 0;
    record.synced.ratingSet = syncedRatingSet != 0;
    record.synced.reviewKnown = syncedReviewKnown != 0;
    record.synced.reviewSet = syncedReviewSet != 0;
    records.push_back(record);
  }
  return true;
}

bool BookStateStore::flush() {
  std::vector<uint8_t> raw;
  raw.reserve(64 + records.size() * 96);
  appendU8(raw, kFormatVersion);
  appendU32(raw, static_cast<uint32_t>(records.size()));

  for (const auto& record : records) {
    const size_t md5Len = std::strlen(record.md5);
    appendU8(raw, static_cast<uint8_t>(md5Len));
    raw.insert(raw.end(), record.md5, record.md5 + md5Len);
    appendU8(raw, record.local.statusKnown ? 1 : 0);
    appendU8(raw, static_cast<uint8_t>(record.local.status));
    appendDate(raw, record.local.statusModified);
    appendU8(raw, record.local.ratingSet ? 1 : 0);
    appendU8(raw, record.local.rating);
    appendU8(raw, record.local.reviewSet ? 1 : 0);
    appendString(raw, truncateReview(record.local.reviewNote));
    appendDate(raw, record.local.reviewModified);
    appendU8(raw, record.synced.ratingKnown ? 1 : 0);
    appendU8(raw, record.synced.ratingSet ? 1 : 0);
    appendU8(raw, record.synced.rating);
    appendU8(raw, record.synced.reviewKnown ? 1 : 0);
    appendU8(raw, record.synced.reviewSet ? 1 : 0);
    appendString(raw, truncateReview(record.synced.reviewNote));
    appendDate(raw, record.synced.statusSyncedModified);
    appendU32(raw, record.statePulledAt);
  }

  return atomicWriteBlob(blobs, path, raw.data(), raw.size());
}

BookStateRecord* BookStateStore::find(const std::string_view md5) {
  for (auto& record : records) {
    if (md5 == record.md5) return &record;
  }
  return nullptr;
}

BookStateRecord& BookStateStore::findOrCreate(const std::string_view md5) {
  if (auto* existing = find(md5)) return *existing;
  BookStateRecord record;
  const size_t len = md5.size() < sizeof(record.md5) - 1 ? md5.size() : sizeof(record.md5) - 1;
  std::memcpy(record.md5, md5.data(), len);
  record.md5[len] = '\0';
  records.push_back(record);
  return records.back();
}

bool BookStateStore::needsStatePull(const BookStateRecord& record, const uint32_t nowUnix) const {
  if (record.statePulledAt == 0) return true;
  if (nowUnix < record.statePulledAt) return true;  // clock went backwards
  return (nowUnix - record.statePulledAt) > kStatePullMaxAgeSeconds;
}

void BookStateStore::markStatePulled(BookStateRecord& record, const uint32_t nowUnix) {
  record.statePulledAt = nowUnix;
}

}  // namespace bookorbit
