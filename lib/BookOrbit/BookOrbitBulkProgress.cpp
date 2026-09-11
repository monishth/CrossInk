#include "BookOrbitBulkProgress.h"

#include "BookOrbitProgress.h"
#include "JsonStructure.h"
#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct UnmatchedCtx {
  std::vector<std::string>* out = nullptr;
  std::string key;
  bool inUnmatched = false;
};

void onKey(void* raw, const char* key, const size_t len) { static_cast<UnmatchedCtx*>(raw)->key.assign(key, len); }

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  if (ctx->inUnmatched) ctx->out->emplace_back(value, len);
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  if (ctx->key == "unmatched") ctx->inUnmatched = true;
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<UnmatchedCtx*>(raw);
  ctx->inUnmatched = false;
  ctx->key.clear();
}

void onNumber(void*, const char*, size_t) {}
void onObjectStart(void*) {}
void onObjectEnd(void* raw) { static_cast<UnmatchedCtx*>(raw)->key.clear(); }
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

std::string encodeBulkProgress(const std::vector<BulkProgressItem>& items, const size_t offset, size_t& consumed) {
  consumed = 0;
  if (offset >= items.size()) return {};

  const size_t end = (items.size() - offset < kBulkProgressBatchSize) ? items.size() : offset + kBulkProgressBatchSize;

  std::string body;
  body.reserve(128 * (end - offset));
  body += "{\"items\":[";

  size_t emitted = 0;
  for (size_t i = offset; i < end; i++) {
    consumed++;
    const auto& value = items[i];
    // An item with no xpointer would be a percentage-only upload. Skip it
    // rather than degrade the server's record silently.
    if (value.hash.empty() || value.progress.empty()) continue;

    if (emitted > 0) body += ',';
    body += "{\"hash\":";
    appendJsonString(body, value.hash);
    body += ",\"percentage\":";
    appendPercentage(body, value.percentage);
    body += ",\"progress\":";
    const std::string normalized = normalizeXPointer(value.progress);
    appendJsonString(body, normalized.empty() ? value.progress : normalized);
    body += ",\"timestamp\":";
    body += std::to_string(value.timestamp);
    body += '}';
    emitted++;
  }

  if (emitted == 0) return {};
  body += "]}";
  return body;
}

bool decodeBulkProgressResponse(const std::string_view json, std::vector<std::string>& unmatched) {
  unmatched.clear();

  UnmatchedCtx ctx;
  ctx.out = &unmatched;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    unmatched.clear();
    return false;
  }
  return true;
}

}  // namespace bookorbit
