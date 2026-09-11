#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitBookmarkModel.h"

namespace bookorbit {

// Limits, verbatim from the spec's P4 section.
inline constexpr size_t kUploadChunk = 50;
inline constexpr size_t kMaxPullRounds = 10;
inline constexpr size_t kMaxAnnotationKeysPerBook = 5000;
inline constexpr size_t kMaxBookmarkKeysPerBook = 500;

inline constexpr char kAnnotationExchangePath[] = "/koreader/plugin/annotations/exchange";
inline constexpr char kAnnotationAckPath[] = "/koreader/plugin/annotations/exchange-ack";
inline constexpr char kBookmarkExchangePath[] = "/koreader/plugin/bookmarks/exchange";
inline constexpr char kBookmarkAckPath[] = "/koreader/plugin/bookmarks/exchange-ack";

// Leg 1 of the exchange. keysComplete asserts "these are ALL my local keys",
// which is what lets the server detect entries deleted on-device. When the key
// set is over the cap, pass keysComplete=false: the keys array is then emitted
// empty and the server skips deletion detection for this book rather than
// deleting what it cannot see.
std::string encodeAnnotationExchange(std::string_view hash, const std::vector<AnnotationKey>& keys, bool keysComplete,
                                     const std::vector<Annotation>& changes);

std::string encodeBookmarkExchange(std::string_view hash, const std::vector<BookmarkKey>& keys, bool keysComplete,
                                   const std::vector<Bookmark>& changes);

}  // namespace bookorbit
