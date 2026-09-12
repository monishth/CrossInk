#include "BookOrbitBookmarkModel.h"

#include <cstdio>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitMd5.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

std::string truncateUtf8(const std::string& value, const size_t limit) {
  if (value.size() <= limit) return value;
  size_t cut = limit;
  while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
    cut--;
  }
  return value.substr(0, cut);
}

std::string head(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(0, count);
}

std::string tail(const std::string& value, const size_t count) {
  return value.size() <= count ? value : value.substr(value.size() - count);
}

// Matches bookorbit_sidecar.lua's bookmarkHash(): the note and the chapter are
// part of the identity because KOReader does not stamp datetime_updated when a
// dogear is renamed.
uint32_t bookmarkHash(const Bookmark& entry) {
  char lengthBuf[16];
  snprintf(lengthBuf, sizeof(lengthBuf), "%zu", entry.pos.size());

  std::string key;
  key.reserve(entry.datetime.size() + entry.pos.size() + entry.note.size() + entry.chapter.size() + 80);
  key += entry.datetime;
  key += '|';
  key += lengthBuf;
  key += '|';
  key += head(entry.pos, 24);
  key += '|';
  key += tail(entry.pos, 24);
  key += '|';
  key += entry.datetimeUpdated;
  key += '|';
  key += entry.note;
  key += '|';
  key += entry.chapter;

  uint32_t hash = 5381;
  for (const char ch : key) {
    hash = hash * 33u + static_cast<unsigned char>(ch);
  }
  return hash;
}

}  // namespace

NormalizedBookmarks normalizeBookmarks(const std::vector<Bookmark>& raw) {
  NormalizedBookmarks normalized;
  normalized.entries.reserve(raw.size());

  uint32_t sum = 0;
  uint32_t mix = 0;

  for (const auto& candidate : raw) {
    if (!isDeviceDatetime(candidate.datetime)) {
      normalized.complete = false;
      continue;
    }

    const std::string canonicalPos = normalizeXPointer(candidate.pos);
    if (canonicalPos.empty()) {
      normalized.complete = false;
      continue;
    }

    Bookmark entry;
    entry.datetime = candidate.datetime;
    entry.datetimeUpdated = isDeviceDatetime(candidate.datetimeUpdated) ? candidate.datetimeUpdated : std::string();
    entry.pos = canonicalPos.size() > kMaxPosBytes ? canonicalPos.substr(0, kMaxPosBytes) : canonicalPos;
    entry.chapter = truncateUtf8(candidate.chapter, kMaxChapterBytes);
    entry.note = truncateUtf8(candidate.note, kMaxBookmarkNoteBytes);
    entry.pageno = candidate.pageno;

    const std::string& effective = entry.datetimeUpdated.empty() ? entry.datetime : entry.datetimeUpdated;
    if (effective > normalized.maxDatetime) {
      normalized.maxDatetime = effective;
    }

    const uint32_t hash = bookmarkHash(entry);
    sum += hash;
    mix += static_cast<uint32_t>(static_cast<uint64_t>(hash) * ((hash % 8191u) + 1u));

    normalized.entries.push_back(std::move(entry));
  }

  char signature[64];
  snprintf(signature, sizeof(signature), "%zu:%s:%u:%u", normalized.entries.size(), normalized.maxDatetime.c_str(), sum,
           mix);
  normalized.signature = signature;
  return normalized;
}

std::string buildBookmarkKey(const std::string_view datetime, const std::string_view pos) {
  std::string material;
  material.reserve(datetime.size() + pos.size() + 1);
  material.append(datetime);
  material += '|';
  material.append(pos);
  return md5Hex(material);
}

std::vector<BookmarkKey> collectBookmarkKeys(const std::vector<Bookmark>& normalized) {
  std::vector<BookmarkKey> keys;
  keys.reserve(normalized.size());
  for (const auto& entry : normalized) {
    keys.push_back({buildBookmarkKey(entry.datetime, entry.pos), entry.datetime});
  }
  return keys;
}

}  // namespace bookorbit
