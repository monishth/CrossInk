#include "BookOrbitExchangeResponse.h"

#include <cstdlib>
#include <cstring>

#include "JsonStructure.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

enum class Section : uint8_t { None, Unmatched, Add, Delete };

// StreamingJsonParser is a C-callback tokenizer over a void* ctx, so the
// decoder is a small depth-tracking state machine rather than a DOM walk.
struct DecodeCtx {
  ExchangeResponse* out = nullptr;
  std::string key;
  Section section = Section::None;
  bool inResults = false;
  int arrayDepth = 0;
  int sectionArrayDepth = 0;
  int resultsArrayDepth = 0;
  int objectDepth = 0;
  int resultObjectDepth = 0;
  ExchangeBookResult result;
  RemoteEntry entry;
};

void assignEntryString(RemoteEntry& entry, const std::string& key, const char* value, const size_t len) {
  const std::string text(value, len);
  if (key == "serverId")
    entry.serverId = text;
  else if (key == "key")
    entry.key = text;
  else if (key == "datetime")
    entry.datetime = text;
  else if (key == "datetimeUpdated")
    entry.datetimeUpdated = text;
  else if (key == "drawer")
    entry.drawer = text;
  else if (key == "color")
    entry.color = text;
  else if (key == "text")
    entry.text = text;
  else if (key == "note")
    entry.note = text;
  else if (key == "chapter")
    entry.chapter = text;
  else if (key == "title")
    entry.title = text;
  else if (key == "posFormat")
    entry.posFormat = text;
  else if (key == "pos0" || key == "pos")
    entry.pos0 = text;
  else if (key == "pos1")
    entry.pos1 = text;
}

void onKey(void* raw, const char* key, const size_t len) { static_cast<DecodeCtx*>(raw)->key.assign(key, len); }

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section == Section::Unmatched) {
    ctx->out->unmatched.emplace_back(value, len);
    return;
  }
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    assignEntryString(ctx->entry, ctx->key, value, len);
    return;
  }
  if (ctx->inResults && ctx->key == "hash") {
    ctx->result.hash.assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section != Section::Add && ctx->section != Section::Delete) return;
  const std::string text(value, len);
  if (ctx->key == "serverId") {
    ctx->entry.serverId = text;  // kept as text; the ack echoes it verbatim
  } else if (ctx->key == "pageno") {
    ctx->entry.pageno = static_cast<int32_t>(strtol(text.c_str(), nullptr, 10));
  }
}

void onBool(void* raw, const bool value) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->inResults && ctx->section == Section::None && ctx->key == "more") {
    ctx->result.more = value;
  }
}

void onNull(void*) {}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->arrayDepth++;
  if (ctx->key == "unmatched" && ctx->section == Section::None) {
    ctx->section = Section::Unmatched;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  } else if (ctx->key == "results" && !ctx->inResults) {
    ctx->inResults = true;
    ctx->resultsArrayDepth = ctx->arrayDepth;
  } else if (ctx->inResults && ctx->key == "add") {
    ctx->section = Section::Add;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  } else if (ctx->inResults && ctx->key == "delete") {
    ctx->section = Section::Delete;
    ctx->sectionArrayDepth = ctx->arrayDepth;
  }
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section != Section::None && ctx->arrayDepth == ctx->sectionArrayDepth) {
    ctx->section = Section::None;
    ctx->sectionArrayDepth = 0;
  }
  if (ctx->inResults && ctx->arrayDepth == ctx->resultsArrayDepth) {
    ctx->inResults = false;
    ctx->resultsArrayDepth = 0;
  }
  ctx->arrayDepth--;
  ctx->key.clear();
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->objectDepth++;
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    ctx->entry = RemoteEntry{};
  } else if (ctx->inResults && ctx->resultObjectDepth == 0) {
    ctx->resultObjectDepth = ctx->objectDepth;
    ctx->result = ExchangeBookResult{};
  }
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->section == Section::Add || ctx->section == Section::Delete) {
    // An entry with no serverId cannot be acknowledged, so applying it would
    // guarantee the server re-sends it forever. Drop it here.
    if (!ctx->entry.serverId.empty()) {
      if (ctx->section == Section::Add) {
        ctx->result.add.push_back(ctx->entry);
      } else {
        ctx->result.remove.push_back(ctx->entry);
      }
    }
  } else if (ctx->resultObjectDepth != 0 && ctx->objectDepth == ctx->resultObjectDepth) {
    ctx->out->results.push_back(ctx->result);
    ctx->resultObjectDepth = 0;
  }
  ctx->objectDepth--;
  ctx->key.clear();
}

}  // namespace

bool decodeExchangeResponse(const std::string_view json, ExchangeResponse& out) {
  out.unmatched.clear();
  out.results.clear();

  DecodeCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  // json.data() is not NUL-terminated, but feed() takes an explicit length, so
  // this is safe. Never pass it to a C string API.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.unmatched.clear();
    out.results.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
