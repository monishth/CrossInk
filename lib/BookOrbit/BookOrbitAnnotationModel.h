#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// Truncation limits, verbatim from the spec's P4 field mapping.
inline constexpr size_t kMaxAnnotationTextBytes = 10000;
inline constexpr size_t kMaxAnnotationNoteBytes = 5000;
inline constexpr size_t kMaxChapterBytes = 500;
inline constexpr size_t kMaxPosBytes = 4000;
inline constexpr size_t kMaxColorBytes = 30;

// One highlight in BookOrbit's wire shape. pageno == -1 means "absent"; the
// encoder omits the field rather than sending a sentinel.
struct Annotation {
  std::string datetime;         // "YYYY-MM-DD HH:MM:SS", local device time
  std::string datetimeUpdated;  // same format, empty when never edited
  std::string drawer;           // lighten | underscore | strikeout | invert
  std::string color;
  std::string text;
  std::string note;
  std::string chapter;
  int32_t pageno = -1;
  std::string posFormat;  // always "xpointer" after normalization
  std::string pos0;
  std::string pos1;
};

// The server's deletion-detection set: k is md5(datetime|pos0), dt the datetime.
struct AnnotationKey {
  std::string k;
  std::string dt;
};

struct NormalizedAnnotations {
  std::vector<Annotation> entries;
  std::string maxDatetime;  // max of (datetimeUpdated ? datetimeUpdated : datetime)
  std::string signature;    // "count:maxDatetime:hash1:hash2"
};

// True for exactly "YYYY-MM-DD HH:MM:SS". BookOrbit keys on this string, so a
// differently-shaped timestamp is not merely ugly — it is a different key.
bool isDeviceDatetime(std::string_view value);

// Drops entries that cannot be synced (no/unknown drawer, malformed datetime,
// position that is not a crengine xpointer), canonicalizes every position
// through P2's XPointer, truncates to the spec limits, and computes the
// order-independent change signature.
NormalizedAnnotations normalizeAnnotations(const std::vector<Annotation>& raw);

// md5(datetime + "|" + pos0) — BookOrbitAnnotations.buildKey() in Lua.
// pos0 must already be canonical, or the key will not match the server's.
std::string buildAnnotationKey(std::string_view datetime, std::string_view pos0);

std::vector<AnnotationKey> collectAnnotationKeys(const std::vector<Annotation>& normalized);

}  // namespace bookorbit
