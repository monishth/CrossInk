#include "CatalogManifest.h"

#include "CatalogDecodeCommon.h"
#include "CatalogQuery.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

constexpr char kManifestPath[] = "/koreader/plugin/catalog/manifest";

struct ManifestCtx {
  ManifestPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  ManifestItem current;
  bool sawRoot = false;
};

void onKey(void* ctx, const char* key, const size_t len) { static_cast<ManifestCtx*>(ctx)->key.set(key, len); }

void onObjectStart(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = ManifestItem{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onObjectEnd(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) s->out->items.push_back(std::move(s->current));
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onArrayStart(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  }
}

void onArrayEnd(void* ctx) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  const std::string_view text(value, len);
  if (s->scope.inItem) {
    if (s->key.is("hash") || s->key.is("partialMd5")) {
      s->current.hash.assign(text);
    } else if (s->key.is("title")) {
      s->current.title.assign(text);
    } else if (s->key.is("filename") || s->key.is("devicePath")) {
      s->current.filename.assign(text);
    } else if (s->key.is("format") || s->key.is("filetype")) {
      s->current.format.assign(text);
    }
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("nextCursor")) {
    s->out->nextCursor.assign(text);
  } else if (s->key.is("manifestVersion") || s->key.is("libraryVersion")) {
    s->out->manifestVersion.assign(text);
  }
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (!s->scope.inItem) return;
  if (s->key.is("bookId") || s->key.is("id")) {
    s->current.bookId = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("fileId")) {
    s->current.fileId = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("fileBytes") || s->key.is("bytes") || s->key.is("size")) {
    s->current.fileBytes = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onBool(void* ctx, const bool value) {
  auto* s = static_cast<ManifestCtx*>(ctx);
  if (s->scope.objectDepth != 1 || s->scope.inItem) return;
  if (s->key.is("hasNext")) {
    s->out->hasNext = value;
  } else if (s->key.is("restartRequired")) {
    s->out->restartRequired = value;
  }
}

void onNull(void* ctx) { static_cast<ManifestCtx*>(ctx)->key.clear(); }

}  // namespace

bool decodeManifestPage(const std::string_view json, ManifestPage& out) {
  out = ManifestPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  ManifestCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onKey;
  callbacks.onString = onString;
  callbacks.onNumber = onNumber;
  callbacks.onBool = onBool;
  callbacks.onNull = onNull;
  callbacks.onObjectStart = onObjectStart;
  callbacks.onObjectEnd = onObjectEnd;
  callbacks.onArrayStart = onArrayStart;
  callbacks.onArrayEnd = onArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

void ManifestEnumerator::reset() {
  cursor_.clear();
  hasNext_ = true;
  started_ = false;
  restarts_ = 0;
}

Error ManifestEnumerator::fetch(ManifestPage& out) {
  CatalogQuery query;
  query.set("deviceId", deviceId_);
  query.set("size", static_cast<long>(kCatalogPageSize));
  query.set("cursor", cursor_);  // empty cursor is dropped by CatalogQuery

  std::string body;
  const Error error = api_.getBody(query.build(kManifestPath), body);
  if (error.status != Status::Ok) return error;
  if (!decodeManifestPage(body, out)) return {Status::InvalidJson, error.httpStatus};
  return error;
}

Error ManifestEnumerator::next(ManifestPage& out) {
  if (!hasNext_ && started_) {
    out = ManifestPage{};
    return {Status::Ok, 200};
  }

  for (;;) {
    const Error error = fetch(out);
    if (error.status != Status::Ok) {
      // Any failure stops the walk. There are no retry loops in this client:
      // the next sync trigger starts over.
      hasNext_ = false;
      return error;
    }

    if (!out.manifestVersion.empty()) manifestVersion_ = out.manifestVersion;

    if (out.restartRequired) {
      if (restarts_ >= kMaxManifestRestarts) {
        hasNext_ = false;
        return {Status::ClientError, 409};
      }
      restarts_++;
      // Drop the cursor and enumerate from the beginning. Books already on
      // device are not transferred again, so a restart costs listing work only.
      cursor_.clear();
      started_ = false;
      continue;
    }

    started_ = true;
    hasNext_ = out.hasNext && !out.nextCursor.empty();
    cursor_ = hasNext_ ? out.nextCursor : std::string();
    return error;
  }
}

}  // namespace bookorbit
