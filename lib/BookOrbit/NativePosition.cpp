#include "NativePosition.h"

#include <cstdlib>

#include "BookOrbitProgress.h"
#include "JsonStructure.h"
#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct PositionCtx {
  NativePosition* out = nullptr;
  std::string key;
  int depth = 0;
  int positionDepth = -1;
};

bool inPosition(const PositionCtx* ctx) { return ctx->positionDepth >= 0 && ctx->depth == ctx->positionDepth; }

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  ctx->key.assign(key, len);
  if (ctx->key == "position" && ctx->depth == 1) {
    ctx->positionDepth = 2;  // the object that opens next
  }
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (!inPosition(ctx) || ctx->key != "xpath") return;
  ctx->out->xpath.assign(value, len);
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (!inPosition(ctx)) return;
  const std::string text(value, len);
  const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10);
  if (ctx->key == "pctQ")
    ctx->out->pctQ = static_cast<uint32_t>(parsed);
  else if (ctx->key == "spine")
    ctx->out->spine = static_cast<uint16_t>(parsed);
  else if (ctx->key == "page")
    ctx->out->page = static_cast<uint16_t>(parsed);
  else if (ctx->key == "pages")
    ctx->out->pages = static_cast<uint16_t>(parsed);
  else if (ctx->key == "para")
    ctx->out->para = static_cast<uint16_t>(parsed);
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  ctx->depth++;
  if (ctx->depth == ctx->positionDepth) ctx->out->present = true;
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<PositionCtx*>(raw);
  if (ctx->depth == ctx->positionDepth) ctx->positionDepth = -1;
  ctx->depth--;
  ctx->key.clear();
}

void onArrayStart(void*) {}
void onArrayEnd(void*) {}
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

void appendNativePosition(std::string& out, const NativePosition& position) {
  if (!position.present) return;

  out += "\"position\":{\"pctQ\":";
  out += std::to_string(position.pctQ);
  out += ",\"spine\":";
  out += std::to_string(position.spine);
  out += ",\"page\":";
  out += std::to_string(position.page);
  out += ",\"pages\":";
  out += std::to_string(position.pages > 0 ? position.pages : 1);
  out += ",\"para\":";
  out += std::to_string(position.para);
  out += ",\"xpath\":";
  const std::string normalized = normalizeXPointer(position.xpath);
  appendJsonString(out, normalized.empty() ? position.xpath : normalized);
  out += '}';
}

bool decodeNativePosition(const std::string_view json, NativePosition& out) {
  out = NativePosition{};

  PositionCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out = NativePosition{};
    return false;
  }
  return true;
}

}  // namespace bookorbit
