#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bookorbit {

// Percent-encodes one query component. Unreserved set is RFC 3986's
// ALPHA / DIGIT / "-" / "_" / "." / "~", matching KOReader's util.urlEncode,
// so a URL built here is byte-identical to the one the Lua plugin sends.
std::string urlEncodeComponent(std::string_view value);

// Catalog query parameters. Empty values are dropped, keys are sorted
// alphabetically, and both sides of every pair are percent-encoded — the three
// rules from bookorbit_api.lua:266-283. Sorting is what makes a URL stable
// enough to compare in a test and to use as a cache key.
class CatalogQuery {
 public:
  CatalogQuery() { params_.reserve(kTypicalParams); }

  void set(std::string_view key, std::string_view value);
  void set(std::string_view key, long value);
  void clear(std::string_view key);

  bool empty() const { return params_.empty(); }

  // Returns path unchanged when no parameter survived the empty-value drop.
  std::string build(std::string_view path) const;

 private:
  // The widest catalog request carries 11 parameters (see the filter set in
  // the spec). Reserving that avoids reallocation without a fixed array.
  static constexpr size_t kTypicalParams = 12;

  std::vector<std::pair<std::string, std::string>> params_;
};

}  // namespace bookorbit
