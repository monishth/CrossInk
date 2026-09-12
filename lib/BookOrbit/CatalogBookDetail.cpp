#include "CatalogBookDetail.h"

#include <cstdlib>

#include "JsonStructure.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

struct DetailCtx {
  std::vector<CatalogFile>* out = nullptr;
  std::string key;
  int depth = 0;
  bool inFiles = false;
  int filesDepth = 0;
  CatalogFile current;
};

void onKey(void* raw, const char* key, const size_t len) { static_cast<DetailCtx*>(raw)->key.assign(key, len); }

void onString(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  if (!ctx->inFiles) return;
  if (ctx->key == "format") {
    ctx->current.format.assign(value, len);
  } else if (ctx->key == "role") {
    ctx->current.role.assign(value, len);
  }
}

void onNumber(void* raw, const char* value, const size_t len) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  if (!ctx->inFiles) return;
  const uint32_t parsed = static_cast<uint32_t>(strtoul(std::string(value, len).c_str(), nullptr, 10));
  if (ctx->key == "id") {
    ctx->current.fileId = parsed;
  } else if (ctx->key == "sizeBytes") {
    ctx->current.sizeBytes = parsed;
  }
}

void onArrayStart(void* raw) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  if (ctx->key == "files") {
    ctx->inFiles = true;
    ctx->filesDepth = ctx->depth;
  }
}

void onArrayEnd(void* raw) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  // Only the files[] array closing ends collection; a nested array must not.
  if (ctx->inFiles && ctx->depth == ctx->filesDepth) ctx->inFiles = false;
}

void onObjectStart(void* raw) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  ctx->depth++;
  if (ctx->inFiles) ctx->current = CatalogFile{};
}

void onObjectEnd(void* raw) {
  auto* ctx = static_cast<DetailCtx*>(raw);
  // A file with no id is unusable — keep it so pickDownloadableFile() can
  // report "nothing to download" rather than silently selecting a bad row.
  if (ctx->inFiles) ctx->out->push_back(ctx->current);
  ctx->depth--;
  ctx->key.clear();
}

void onBool(void*, bool) {}
void onNull(void*) {}

}  // namespace

bool decodeBookDetailFiles(const std::string_view json, std::vector<CatalogFile>& out) {
  out.clear();
  if (!jsonIsStructurallyComplete(json)) return false;

  DetailCtx ctx;
  ctx.out = &out;

  const JsonCallbacks callbacks{
      &ctx, onKey, onString, onNumber, onBool, onNull, onObjectStart, onObjectEnd, onArrayStart, onArrayEnd,
  };

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  if (parser.hasError()) {
    out.clear();
    return false;
  }
  return true;
}

const CatalogFile* pickDownloadableFile(const std::vector<CatalogFile>& files) {
  // EPUB first: it is the only format this reader can open, so downloading an
  // audiobook because it happened to be listed first would waste the transfer.
  for (const auto& file : files) {
    if (file.fileId != 0 && file.format == "epub") return &file;
  }
  for (const auto& file : files) {
    if (file.fileId != 0) return &file;
  }
  return nullptr;
}

std::string bookDetailPath(const uint32_t bookId) { return "/koreader/plugin/catalog/books/" + std::to_string(bookId); }

std::string fileDownloadPath(const uint32_t fileId) {
  return "/koreader/plugin/catalog/files/" + std::to_string(fileId) + "/download";
}

}  // namespace bookorbit
