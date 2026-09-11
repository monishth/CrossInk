#include "BookOrbitMatch.h"

#include "JsonStructure.h"
#include "StreamingJsonParser.h"

namespace bookorbit {

std::string jsonEscape(const std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

std::string encodeMatchCheck(const std::vector<MatchCandidate>& candidates) {
  std::string json = R"({"hashes":[)";
  for (size_t i = 0; i < candidates.size(); i++) {
    if (i > 0) json += ',';
    json += '"';
    json += jsonEscape(candidates[i].hash);
    json += '"';
  }
  json += R"(],"books":[)";
  for (size_t i = 0; i < candidates.size(); i++) {
    const auto& candidate = candidates[i];
    if (i > 0) json += ',';
    json += R"({"hash":")";
    json += jsonEscape(candidate.hash);
    json += '"';
    // Ambiguous metadata is withheld entirely rather than risking the wrong
    // title being associated server-side.
    if (!candidate.metadataAmbiguous) {
      if (!candidate.title.empty()) {
        json += R"(,"title":")" + jsonEscape(candidate.title) + '"';
      }
      if (!candidate.authors.empty()) {
        json += R"(,"authors":")" + jsonEscape(candidate.authors) + '"';
      }
    }
    if (candidate.lastOpen != 0) {
      json += R"(,"lastOpen":)" + std::to_string(candidate.lastOpen);
    }
    json += R"(,"source":"current_file")";
    json += R"(,"metadataAmbiguous":)";
    json += candidate.metadataAmbiguous ? "true" : "false";
    json += '}';
  }
  json += "]}";
  return json;
}

namespace {

// StreamingJsonParser uses C-style callbacks with a void* ctx and a 512-byte
// token buffer, so the decoder is a small state machine rather than a DOM walk.
struct MatchDecodeCtx {
  std::vector<MatchResult>* out = nullptr;
  std::string* libraryVersion = nullptr;
  std::string key;  // most recent key at the current level
  int depth = 0;
  bool inMatches = false;
  MatchResult current;
};

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->inMatches) {
    if (ctx->key == "hash") ctx->current.hash.assign(value, len);
  } else if (ctx->key == "libraryVersion") {
    ctx->libraryVersion->assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (!ctx->inMatches) return;
  const uint32_t parsed = static_cast<uint32_t>(strtoul(std::string(value, len).c_str(), nullptr, 10));
  if (ctx->key == "bookFileId")
    ctx->current.bookFileId = parsed;
  else if (ctx->key == "bookId")
    ctx->current.bookId = parsed;
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->key == "matches") ctx->inMatches = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->inMatches = false;
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  ctx->depth++;
  if (ctx->inMatches) ctx->current = MatchResult{};
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<MatchDecodeCtx*>(raw);
  if (ctx->inMatches && !ctx->current.hash.empty()) {
    ctx->out->push_back(ctx->current);
  }
  ctx->depth--;
  ctx->key.clear();
}

void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

bool decodeMatchCheck(const std::string_view json, std::vector<MatchResult>& out, std::string& libraryVersion) {
  out.clear();
  libraryVersion.clear();

  if (!jsonIsStructurallyComplete(json)) return false;

  MatchDecodeCtx ctx;
  ctx.out = &out;
  ctx.libraryVersion = &libraryVersion;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.clear();
    libraryVersion.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
