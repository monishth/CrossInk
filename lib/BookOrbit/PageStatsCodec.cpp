#include "PageStatsCodec.h"

#include "BookOrbitMatch.h"  // jsonEscape
#include "JsonStructure.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

struct AckCtx {
  PageStatsAck* out = nullptr;
  std::string key;
  bool inUnmatched = false;
  bool inResults = false;
  std::string hash;
  uint32_t watermark = 0;
  bool sawWatermark = false;
};

void onKey(void* raw, const char* key, const size_t len) { static_cast<AckCtx*>(raw)->key.assign(key, len); }

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inUnmatched) {
    ctx->out->unmatched.emplace_back(value, len);
  } else if (ctx->inResults && ctx->key == "hash") {
    ctx->hash.assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults && ctx->key == "watermark") {
    ctx->watermark = static_cast<uint32_t>(strtoul(std::string(value, len).c_str(), nullptr, 10));
    ctx->sawWatermark = true;
  }
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->key == "unmatched")
    ctx->inUnmatched = true;
  else if (ctx->key == "results")
    ctx->inResults = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  ctx->inUnmatched = false;
  ctx->inResults = false;
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults) {
    ctx->hash.clear();
    ctx->watermark = 0;
    ctx->sawWatermark = false;
  }
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<AckCtx*>(raw);
  if (ctx->inResults && !ctx->hash.empty() && ctx->sawWatermark) {
    ctx->out->watermarks.emplace_back(ctx->hash, ctx->watermark);
  }
  ctx->key.clear();
}

void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

std::string encodePageStats(const std::string_view hash, const std::vector<ReadingEvent>& events) {
  std::string json = R"({"books":[{"hash":")";
  json += jsonEscape(hash);
  json += R"(","events":[)";
  for (size_t i = 0; i < events.size(); i++) {
    const auto& event = events[i];
    if (i > 0) json += ',';
    json += R"({"page":)" + std::to_string(event.page);
    json += R"(,"startTime":)" + std::to_string(event.startTime);
    json += R"(,"durationSeconds":)" + std::to_string(event.durationSeconds);
    json += R"(,"totalPages":)" + std::to_string(event.totalPages);
    json += '}';
  }
  json += "]}]}";
  return json;
}

bool decodePageStats(const std::string_view json, PageStatsAck& out) {
  out.unmatched.clear();
  out.watermarks.clear();

  AckCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.unmatched.clear();
    out.watermarks.clear();
    return false;
  }
  return true;
}

bool nextWatermark(const std::vector<ReadingEvent>& sent, const size_t batchSize, const uint32_t oldWatermark,
                   const uint32_t serverWatermark, uint32_t& outWatermark) {
  if (sent.empty()) {
    outWatermark = oldWatermark;
    return false;
  }

  if (sent.size() < batchSize) {
    outWatermark = serverWatermark;
    return false;
  }

  // Full batch: back off one second so a group sharing the cut timestamp is
  // re-fetched next round.
  const uint32_t lastStart = sent.back().startTime;
  const uint32_t backedOff = lastStart > 0 ? lastStart - 1 : 0;
  outWatermark = (backedOff <= oldWatermark) ? serverWatermark : backedOff;
  return true;
}

}  // namespace bookorbit
