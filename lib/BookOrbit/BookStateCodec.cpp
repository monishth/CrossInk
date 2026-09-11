#include "BookStateCodec.h"

#include <cstdlib>

#include "BookOrbitMatch.h"  // jsonEscape
#include "JsonStructure.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

void appendDate(std::string& json, const char* key, const DateOnly& date) {
  if (!date.valid()) return;
  char buf[11] = {};
  formatDateOnly(date, buf, sizeof(buf));
  json += ",\"";
  json += key;
  json += "\":\"";
  json += buf;
  json += '"';
}

}  // namespace

bool buildStatePayload(const std::string_view hash, const LocalBookState& local, const SyncedBookState& synced,
                       const bool forcePull, BookStatePayload& out) {
  out = BookStatePayload{};
  out.hash = std::string(hash);

  const bool statusChanged =
      local.statusKnown && compareDateOnly(local.statusModified, synced.statusSyncedModified) != 0;

  const bool ratingKnown = synced.ratingKnown;
  bool ratingChanged = false;
  if (local.ratingSet) {
    ratingChanged = !ratingKnown || !synced.ratingSet || local.rating != synced.rating;
  } else if (ratingKnown && synced.ratingSet) {
    ratingChanged = true;  // the user removed a rating the server still holds
  }

  const bool reviewKnown = synced.reviewKnown;
  bool reviewChanged = false;
  if (local.reviewSet) {
    reviewChanged = !reviewKnown || !synced.reviewSet || local.reviewNote != synced.reviewNote;
  } else if (reviewKnown && synced.reviewSet) {
    reviewChanged = true;
  }

  if (!statusChanged && !ratingChanged && !reviewChanged) {
    // A forced pull is a bare hash: no local field is asserted, but the server
    // still answers with its rating and review.
    return forcePull;
  }

  if (statusChanged) {
    out.hasStatus = true;
    out.status = local.status;
    out.statusModified = local.statusModified;
  }
  if (ratingChanged) {
    if (local.ratingSet) {
      out.hasRating = true;
      out.rating = local.rating;
    } else {
      out.ratingCleared = true;
    }
    if (!out.statusModified.valid()) {
      out.statusModified = local.statusModified;
    }
  }
  if (reviewChanged) {
    if (local.reviewSet) {
      out.hasReview = true;
      out.reviewNote = local.reviewNote;
    } else {
      out.reviewCleared = true;
    }
    out.reviewModified = local.reviewModified;
  }
  return true;
}

std::string encodeBookStates(const std::vector<BookStatePayload>& books) {
  std::string json = R"({"books":[)";
  for (size_t i = 0; i < books.size(); i++) {
    const auto& book = books[i];
    if (i > 0) json += ',';
    json += R"({"hash":")";
    json += jsonEscape(book.hash);
    json += '"';
    if (book.hasStatus) {
      json += R"(,"status":")";
      json += statusToString(book.status);
      json += '"';
    }
    appendDate(json, "statusModified", book.statusModified);
    if (book.hasRating) {
      json += R"(,"rating":)";
      json += std::to_string(static_cast<unsigned>(book.rating));
    }
    if (book.ratingCleared) {
      json += R"(,"ratingCleared":true)";
    }
    if (book.hasReview) {
      json += R"(,"reviewNote":")";
      json += jsonEscape(book.reviewNote);
      json += '"';
    }
    if (book.reviewCleared) {
      json += R"(,"reviewCleared":true)";
    }
    appendDate(json, "reviewModified", book.reviewModified);
    json += '}';
  }
  json += "]}";
  return json;
}

namespace {

// StreamingJsonParser is a C-callback tokenizer with a void* ctx, a 512-byte
// token buffer and 32 nesting levels, so decoding is a small state machine
// rather than a DOM walk. Two sibling arrays have to be told apart, which is
// what `section` tracks.
enum class StateSection : uint8_t { None, Unmatched, Results };

struct StateDecodeCtx {
  std::vector<std::string>* unmatched = nullptr;
  std::vector<ServerBookState>* results = nullptr;
  std::string key;
  StateSection section = StateSection::None;
  int objectDepth = 0;
  ServerBookState current;
};

void stateOnKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void stateOnString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  const std::string_view text(value, len);
  if (ctx->section == StateSection::Unmatched) {
    ctx->unmatched->emplace_back(text);
    return;
  }
  if (ctx->section != StateSection::Results) return;
  if (ctx->key == "hash") {
    ctx->current.hash.assign(text);
  } else if (ctx->key == "reviewNote") {
    ctx->current.reviewNote = truncateReview(text);
  } else if (ctx->key == "ratingUpdatedAt") {
    parseDateOnly(text, ctx->current.ratingUpdatedAt);
  } else if (ctx->key == "reviewUpdatedAt") {
    parseDateOnly(text, ctx->current.reviewUpdatedAt);
  }
}

void stateOnNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section != StateSection::Results || ctx->key != "rating") return;
  const std::string digits(value, len);
  uint8_t normalized = 0;
  if (normalizeRating(atoi(digits.c_str()), normalized)) {
    ctx->current.rating = normalized;
  } else {
    // Out of range: the server says it holds a rating we cannot represent.
    // Treat it as "known, unset" rather than inventing a value.
    ctx->current.rating = 0;
    ctx->current.ratingSet = false;
  }
}

void stateOnBool(void* raw, const bool value) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section != StateSection::Results) return;
  if (ctx->key == "ratingSet") {
    ctx->current.ratingKnown = true;
    ctx->current.ratingSet = value;
  } else if (ctx->key == "reviewNoteSet") {
    ctx->current.reviewKnown = true;
    ctx->current.reviewNoteSet = value;
  }
}

void stateOnNull(void*) {}

void stateOnArrayStart(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->key == "unmatched") {
    ctx->section = StateSection::Unmatched;
  } else if (ctx->key == "results") {
    ctx->section = StateSection::Results;
  }
}

void stateOnArrayEnd(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->section = StateSection::None;
  ctx->key.clear();
}

void stateOnObjectStart(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  ctx->objectDepth++;
  if (ctx->section == StateSection::Results) {
    ctx->current = ServerBookState{};
  }
}

void stateOnObjectEnd(void* raw) {
  auto* ctx = static_cast<StateDecodeCtx*>(raw);
  if (ctx->section == StateSection::Results && !ctx->current.hash.empty()) {
    // A rating that never passed normalizeRating leaves ratingSet false, so a
    // bad value degrades to "server holds nothing" instead of a wrong star count.
    if (ctx->current.ratingSet && ctx->current.rating == 0) {
      ctx->current.ratingSet = false;
    }
    if (!ctx->current.reviewNoteSet) {
      ctx->current.reviewNote.clear();
    }
    ctx->results->push_back(ctx->current);
  }
  ctx->objectDepth--;
  ctx->key.clear();
}

}  // namespace

bool decodeBookStates(const std::string_view json, std::vector<std::string>& unmatched,
                      std::vector<ServerBookState>& results) {
  unmatched.clear();
  results.clear();

  StateDecodeCtx ctx;
  ctx.unmatched = &unmatched;
  ctx.results = &results;

  const JsonCallbacks callbacks{
      &ctx,        stateOnKey,         stateOnString,    stateOnNumber,     stateOnBool,
      stateOnNull, stateOnObjectStart, stateOnObjectEnd, stateOnArrayStart, stateOnArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  // json.data() is not null-terminated, but feed() takes an explicit length, so
  // passing it here is safe. Never hand it to a C string API.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    unmatched.clear();
    results.clear();
    return false;
  }
  return true;
}
}  // namespace bookorbit
