#include "BookOrbitAnnotationModel.h"

#include <cstdio>

#include "BookOrbitMd5.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

bool isDigits(const std::string_view value, const size_t from, const size_t count) {
  for (size_t i = from; i < from + count; i++) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

bool isAllowedDrawer(const std::string_view drawer) {
  return drawer == "lighten" || drawer == "underscore" || drawer == "strikeout" || drawer == "invert";
}

// Byte truncation, used for positions only: they are ASCII, and the server
// hashed exactly these bytes.
std::string truncateBytes(const std::string& value, const size_t limit) {
  if (value.size() <= limit) return value;
  return value.substr(0, limit);
}

// Truncation for human text. Backs off to the start of the last complete
// UTF-8 sequence so the JSON body never carries a split character.
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

// djb2 over the entry's identity fields, matching bookorbit_sidecar.lua's
// entryHash(). Positions can be kilobytes long, so only their length and both
// ends are sampled: this signal decides whether a book may skip an exchange,
// never what gets uploaded.
uint32_t entryHash(const Annotation& entry) {
  char lengthBuf[16];
  snprintf(lengthBuf, sizeof(lengthBuf), "%zu", entry.pos0.size());

  std::string key;
  key.reserve(entry.datetime.size() + entry.pos0.size() + 80);
  key += entry.datetime;
  key += '|';
  key += lengthBuf;
  key += '|';
  key += head(entry.pos0, 24);
  key += '|';
  key += tail(entry.pos0, 24);
  key += '|';
  key += entry.datetimeUpdated;

  uint32_t hash = 5381;
  for (const char ch : key) {
    hash = hash * 33u + static_cast<unsigned char>(ch);  // wraps at 2^32, as Lua's % 4294967296 does
  }
  return hash;
}

}  // namespace

bool isDeviceDatetime(const std::string_view value) {
  if (value.size() != 19) return false;
  if (value[4] != '-' || value[7] != '-' || value[10] != ' ' || value[13] != ':' || value[16] != ':') return false;
  return isDigits(value, 0, 4) && isDigits(value, 5, 2) && isDigits(value, 8, 2) && isDigits(value, 11, 2) &&
         isDigits(value, 14, 2) && isDigits(value, 17, 2);
}

NormalizedAnnotations normalizeAnnotations(const std::vector<Annotation>& raw) {
  NormalizedAnnotations normalized;
  normalized.entries.reserve(raw.size());

  uint32_t sum = 0;
  uint32_t mix = 0;

  for (const auto& candidate : raw) {
    if (!isAllowedDrawer(candidate.drawer)) continue;
    if (!isDeviceDatetime(candidate.datetime)) continue;

    const std::string canonicalPos0 = normalizeXPointer(candidate.pos0);
    if (canonicalPos0.empty()) continue;

    Annotation entry;
    entry.datetime = candidate.datetime;
    entry.datetimeUpdated = isDeviceDatetime(candidate.datetimeUpdated) ? candidate.datetimeUpdated : std::string();
    entry.drawer = candidate.drawer;
    entry.color = truncateUtf8(candidate.color, kMaxColorBytes);
    entry.text = truncateUtf8(candidate.text, kMaxAnnotationTextBytes);
    entry.note = truncateUtf8(candidate.note, kMaxAnnotationNoteBytes);
    entry.chapter = truncateUtf8(candidate.chapter, kMaxChapterBytes);
    entry.pageno = candidate.pageno;
    entry.posFormat = "xpointer";
    entry.pos0 = truncateBytes(canonicalPos0, kMaxPosBytes);
    entry.pos1 = truncateBytes(normalizeXPointer(candidate.pos1), kMaxPosBytes);

    const std::string& effective = entry.datetimeUpdated.empty() ? entry.datetime : entry.datetimeUpdated;
    if (effective > normalized.maxDatetime) {
      normalized.maxDatetime = effective;
    }

    const uint32_t hash = entryHash(entry);
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

std::string buildAnnotationKey(const std::string_view datetime, const std::string_view pos0) {
  std::string material;
  material.reserve(datetime.size() + pos0.size() + 1);
  material.append(datetime);
  material += '|';
  material.append(pos0);
  return md5Hex(material);
}

std::vector<AnnotationKey> collectAnnotationKeys(const std::vector<Annotation>& normalized) {
  std::vector<AnnotationKey> keys;
  keys.reserve(normalized.size());
  for (const auto& entry : normalized) {
    keys.push_back({buildAnnotationKey(entry.datetime, entry.pos0), entry.datetime});
  }
  return keys;
}

}  // namespace bookorbit
