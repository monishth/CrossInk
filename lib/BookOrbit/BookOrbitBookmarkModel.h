#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// A dogear's note is a short label, not prose; the Lua client caps it at 500,
// and the cap must match or the two disagree on what was uploaded.
inline constexpr size_t kMaxBookmarkNoteBytes = 500;

// One position-only bookmark in BookOrbit's wire shape.
struct Bookmark {
  std::string datetime;
  std::string datetimeUpdated;
  std::string pos;  // crengine xpointer; page-number positions are dropped
  std::string chapter;
  std::string note;
  int32_t pageno = -1;
};

struct BookmarkKey {
  std::string k;
  std::string dt;
};

struct NormalizedBookmarks {
  std::vector<Bookmark> entries;
  std::string maxDatetime;
  std::string signature;  // "count:maxDatetime:hash1:hash2"
};

NormalizedBookmarks normalizeBookmarks(const std::vector<Bookmark>& raw);

// md5(datetime + "|" + pos) — BookOrbitBookmarks.buildKey() in Lua.
std::string buildBookmarkKey(std::string_view datetime, std::string_view pos);

std::vector<BookmarkKey> collectBookmarkKeys(const std::vector<Bookmark>& normalized);

}  // namespace bookorbit
