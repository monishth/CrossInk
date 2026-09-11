#include "BookOrbitProgress.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "JsonStructure.h"
#include "StreamingJsonParser.h"
#include "XPointer.h"

namespace bookorbit {
namespace {

struct DecodeCtx {
  ProgressRecord* out = nullptr;
  std::string key;
  int depth = 0;
};

void onKey(void* raw, const char* key, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->key.assign(key, len);
}

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->depth != 1) return;
  const std::string text(value, len);
  if (ctx->key == "progress") {
    const std::string normalized = normalizeXPointer(text);
    ctx->out->progress = normalized.empty() ? text : normalized;
  } else if (ctx->key == "device") {
    ctx->out->device = text;
  } else if (ctx->key == "device_id") {
    ctx->out->deviceId = text;
  } else if (ctx->key == "document") {
    ctx->out->document = text;
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  if (ctx->depth != 1) return;
  const std::string text(value, len);
  if (ctx->key == "percentage") {
    ctx->out->percentage = std::strtof(text.c_str(), nullptr);
  } else if (ctx->key == "timestamp") {
    ctx->out->timestamp = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
  }
}

void onObjectStart(void* raw) { static_cast<DecodeCtx*>(raw)->depth++; }

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<DecodeCtx*>(raw);
  ctx->depth--;
  ctx->key.clear();
}

void onArrayStart(void*) {}
void onArrayEnd(void*) {}
void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

void appendJsonString(std::string& out, const std::string_view value) {
  out += '"';
  for (const char c : value) {
    switch (c) {
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
        if (static_cast<unsigned char>(c) < 0x20) {
          char escape[7];
          std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(c));
          out += escape;
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void appendPercentage(std::string& out, const float percentage) {
  float clamped = percentage;
  if (clamped < 0.0f) clamped = 0.0f;
  if (clamped > 1.0f) clamped = 1.0f;
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "%.6f", static_cast<double>(clamped));
  out += buffer;
}

std::string progressGetPath(const std::string_view digest) {
  if (digest.empty()) return {};
  std::string path = "/koreader/syncs/progress/";
  path.append(digest);
  return path;
}

std::string encodePutProgress(const ProgressRecord& record) {
  if (record.document.empty() || record.progress.empty()) return {};

  std::string body;
  body.reserve(256);
  body += "{\"document\":";
  appendJsonString(body, record.document);
  body += ",\"percentage\":";
  appendPercentage(body, record.percentage);
  body += ",\"progress\":";
  appendJsonString(body, record.progress);
  body += ",\"device\":";
  appendJsonString(body, record.device);
  body += ",\"device_id\":";
  appendJsonString(body, record.deviceId);
  body += ",\"timestamp\":";
  body += std::to_string(record.timestamp);
  // Appends nothing when absent, so a device that has no native position sends
  // exactly the bytes it sent before Approach B existed.
  appendNativePosition(body, record.position);
  body += '}';
  return body;
}

bool decodeProgressResponse(const std::string_view json, ProgressRecord& out) {
  out = ProgressRecord{};

  DecodeCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  if (!jsonIsStructurallyComplete(json)) return false;

  StreamingJsonParser parser(callbacks);
  // json.data() is not null-terminated, but feed() takes an explicit length.
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out = ProgressRecord{};
    return false;
  }

  // Approach B is optional: a response without a position object decodes fine
  // with present == false, so this can never fail a pull.
  decodeNativePosition(json, out.position);
  return true;
}

}  // namespace bookorbit
