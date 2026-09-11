#include "BookOrbitExchangeRequest.h"

#include <string>

#include "BookOrbitMatch.h"  // jsonEscape

namespace bookorbit {
namespace {

void appendString(std::string& json, const char* name, const std::string_view value, const bool comma) {
  if (comma) json += ',';
  json += '"';
  json += name;
  json += "\":\"";
  json += jsonEscape(value);
  json += '"';
}

// Optional fields are omitted rather than sent empty: the server treats an
// absent note differently from a cleared one.
void appendOptionalString(std::string& json, const char* name, const std::string& value) {
  if (value.empty()) return;
  appendString(json, name, value, true);
}

void appendOptionalPageno(std::string& json, const int32_t pageno) {
  if (pageno < 0) return;
  json += ",\"pageno\":";
  json += std::to_string(pageno);
}

template <typename KeyT>
void appendKeys(std::string& json, const std::vector<KeyT>& keys, const bool keysComplete) {
  json += ",\"keys\":[";
  if (keysComplete) {
    for (size_t i = 0; i < keys.size(); i++) {
      if (i > 0) json += ',';
      json += R"({"k":")";
      json += jsonEscape(keys[i].k);
      json += R"(","dt":")";
      json += jsonEscape(keys[i].dt);
      json += "\"}";
    }
  }
  json += "],\"keysComplete\":";
  json += keysComplete ? "true" : "false";
}

std::string openBook(const std::string_view hash) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += '"';
  return json;
}

}  // namespace

std::string encodeAnnotationExchange(const std::string_view hash, const std::vector<AnnotationKey>& keys,
                                     const bool keysComplete, const std::vector<Annotation>& changes) {
  std::string json = openBook(hash);
  appendKeys(json, keys, keysComplete);
  json += ",\"changes\":[";
  for (size_t i = 0; i < changes.size(); i++) {
    const auto& entry = changes[i];
    if (i > 0) json += ',';
    json += '{';
    appendString(json, "datetime", entry.datetime, false);
    appendOptionalString(json, "datetimeUpdated", entry.datetimeUpdated);
    appendString(json, "drawer", entry.drawer, true);
    appendOptionalString(json, "color", entry.color);
    appendOptionalString(json, "text", entry.text);
    appendOptionalString(json, "note", entry.note);
    appendOptionalString(json, "chapter", entry.chapter);
    appendOptionalPageno(json, entry.pageno);
    appendString(json, "posFormat", entry.posFormat.empty() ? std::string_view("xpointer") : entry.posFormat, true);
    appendString(json, "pos0", entry.pos0, true);
    appendOptionalString(json, "pos1", entry.pos1);
    json += '}';
  }
  json += "]}]}";
  return json;
}

std::string encodeBookmarkExchange(const std::string_view hash, const std::vector<BookmarkKey>& keys,
                                   const bool keysComplete, const std::vector<Bookmark>& changes) {
  std::string json = openBook(hash);
  appendKeys(json, keys, keysComplete);
  json += ",\"changes\":[";
  for (size_t i = 0; i < changes.size(); i++) {
    const auto& entry = changes[i];
    if (i > 0) json += ',';
    json += '{';
    appendString(json, "datetime", entry.datetime, false);
    appendOptionalString(json, "datetimeUpdated", entry.datetimeUpdated);
    appendString(json, "pos", entry.pos, true);
    appendOptionalPageno(json, entry.pageno);
    appendOptionalString(json, "chapter", entry.chapter);
    appendOptionalString(json, "note", entry.note);
    json += '}';
  }
  json += "]}]}";
  return json;
}

}  // namespace bookorbit
