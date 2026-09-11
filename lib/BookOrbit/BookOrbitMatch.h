#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// POST /koreader/plugin/match-check accepts at most this many hashes.
inline constexpr size_t kMatchBatchSize = 500;

// Hint payload for one book. The server decides the match; the client never
// does fuzzy title/author matching itself.
struct MatchCandidate {
  std::string hash;  // partial MD5, lowercase hex — the only real key
  std::string title;
  std::string authors;
  uint32_t lastOpen = 0;
  bool metadataAmbiguous = false;  // when true, title/authors are withheld
};

struct MatchResult {
  std::string hash;
  uint32_t bookFileId = 0;
  uint32_t bookId = 0;
};

std::string encodeMatchCheck(const std::vector<MatchCandidate>& candidates);

// Returns false only on malformed JSON. A well-formed response with no
// matches is a success with an empty result vector.
bool decodeMatchCheck(std::string_view json, std::vector<MatchResult>& out, std::string& libraryVersion);

// Escapes a string for embedding in a JSON string literal.
std::string jsonEscape(std::string_view value);

}  // namespace bookorbit
