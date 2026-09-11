#include "CatalogDecode.h"

#include "CatalogDecodeCommon.h"
#include "StreamingJsonParser.h"

namespace bookorbit {
namespace {

struct BookPageCtx {
  CatalogPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogBook current;
  bool inFormats = false;
  bool sawRoot = false;
};

void assignBookString(CatalogBook& book, const KeyBuf& key, const std::string_view value) {
  if (key.is("title")) {
    book.title.assign(value);
  } else if (key.is("authors") || key.is("author")) {
    book.authors.assign(value);
  } else if (key.is("series")) {
    book.series.assign(value);
  } else if (key.is("readStatus")) {
    book.readStatus.assign(value);
  } else if (key.is("filename")) {
    book.filename.assign(value);
  }
}

void assignBookNumber(CatalogBook& book, const KeyBuf& key, const char* value, const size_t len) {
  if (key.is("id") || key.is("bookId")) {
    book.bookId = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("fileId")) {
    book.fileId = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("fileBytes") || key.is("bytes") || key.is("size")) {
    book.fileBytes = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("seriesIndex")) {
    book.seriesIndex = static_cast<uint32_t>(parseLong(value, len));
  } else if (key.is("rating")) {
    const long rating = parseLong(value, len);
    book.rating = rating > 0 && rating <= 5 ? static_cast<uint8_t>(rating) : 0;
  } else if (key.is("progressPercentage")) {
    book.progressPercentage = parseFloat(value, len);
  }
}

void onKey(void* ctx, const char* key, const size_t len) { static_cast<BookPageCtx*>(ctx)->key.set(key, len); }

void onObjectStart(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = CatalogBook{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onObjectEnd(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) {
      s->out->items.push_back(std::move(s->current));
    }
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onArrayStart(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  } else if (s->scope.inItem && s->key.is("formats")) {
    s->inFormats = true;
  }
}

void onArrayEnd(void* ctx) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->inFormats && s->scope.arrayDepth == s->scope.itemsArrayDepth + 1) {
    s->inFormats = false;
  } else if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  const std::string_view text(value, len);
  if (s->inFormats) {
    if (!s->current.formats.empty()) s->current.formats += ", ";
    s->current.formats.append(text);
    return;
  }
  if (s->scope.inItem) {
    assignBookString(s->current, s->key, text);
    return;
  }
  if (s->scope.objectDepth == 1 && s->key.is("query")) {
    s->out->query.assign(text);
  }
}

void onNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.inItem) {
    assignBookNumber(s->current, s->key, value, len);
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("page")) {
    const long page = parseLong(value, len);
    s->out->page = page > 0 ? static_cast<uint32_t>(page) : 0u;
  } else if (s->key.is("size")) {
    s->out->size = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onBool(void* ctx, const bool value) {
  auto* s = static_cast<BookPageCtx*>(ctx);
  if (s->scope.objectDepth == 1 && !s->scope.inItem && s->key.is("hasNext")) {
    s->out->hasNext = value;
  }
}

// Nulls simply leave the field at its default, which is what the catalog means
// by a null rating or an absent series.
void onNull(void* ctx) { static_cast<BookPageCtx*>(ctx)->key.clear(); }

JsonCallbacks makeCallbacks(BookPageCtx& ctx) {
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
  return callbacks;
}

bool finish(const BookPageCtx& ctx, const StreamingJsonParser& parser) {
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

}  // namespace

bool decodeBookPage(const std::string_view json, CatalogPage& out) {
  return decodeBookPageChunked(json, json.size(), out);
}

bool decodeBookPageChunked(const std::string_view json, const size_t chunkSize, CatalogPage& out) {
  out = CatalogPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  BookPageCtx ctx;
  ctx.out = &out;
  const JsonCallbacks callbacks = makeCallbacks(ctx);
  StreamingJsonParser parser(callbacks);

  const size_t step = chunkSize > 0 ? chunkSize : json.size();
  for (size_t offset = 0; offset < json.size(); offset += step) {
    const size_t take = json.size() - offset < step ? json.size() - offset : step;
    parser.feed(json.data() + offset, take);
    if (parser.hasError()) return false;
  }
  return finish(ctx, parser);
}

struct EntryPageCtx {
  CatalogEntryPage* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogEntry current;
  bool sawRoot = false;
};

void onEntryKey(void* ctx, const char* key, const size_t len) { static_cast<EntryPageCtx*>(ctx)->key.set(key, len); }

void onEntryObjectStart(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.inItems && s->scope.objectDepth == 2) {
    s->current = CatalogEntry{};
    s->scope.inItem = true;
  }
  s->key.clear();
}

void onEntryObjectEnd(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->out->items.size() < kMaxPageItems) s->out->items.push_back(std::move(s->current));
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onEntryArrayStart(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  s->scope.arrayDepth++;
  if (!s->scope.inItems && s->scope.objectDepth == 1 && s->key.is("items")) {
    s->scope.inItems = true;
    s->scope.itemsArrayDepth = s->scope.arrayDepth;
  }
}

void onEntryArrayEnd(void* ctx) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItems && s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->scope.inItems = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void assignEntryString(CatalogEntry& entry, const KeyBuf& key, const std::string_view value) {
  if (key.is("id")) {
    entry.id.assign(value);
  } else if (key.is("title") || key.is("text") || key.is("name")) {
    entry.title.assign(value);
  } else if (key.is("kind") || key.is("type")) {
    entry.kind.assign(value);
  } else if (key.is("seriesId")) {
    entry.seriesId.assign(value);
  }
}

void onEntryString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem) assignEntryString(s->current, s->key, std::string_view(value, len));
}

void onEntryNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.inItem) {
    // A numeric id or seriesId is stored verbatim: the query builder encodes it
    // as a string either way, so keeping one representation avoids a lossy
    // round-trip through long.
    if (s->key.is("id")) {
      s->current.id.assign(value, len);
    } else if (s->key.is("seriesId")) {
      s->current.seriesId.assign(value, len);
    } else if (s->key.is("count")) {
      s->current.count = static_cast<uint32_t>(parseLong(value, len));
    }
    return;
  }
  if (s->scope.objectDepth == 1 && s->key.is("page")) {
    const long page = parseLong(value, len);
    s->out->page = page > 0 ? static_cast<uint32_t>(page) : 0u;
  }
}

void onEntryBool(void* ctx, const bool value) {
  auto* s = static_cast<EntryPageCtx*>(ctx);
  if (s->scope.objectDepth == 1 && !s->scope.inItem && s->key.is("hasNext")) s->out->hasNext = value;
}

void onEntryNull(void* ctx) { static_cast<EntryPageCtx*>(ctx)->key.clear(); }

struct DashboardCtx {
  DashboardSummary* out = nullptr;
  KeyBuf key;
  DecodeScope scope;
  CatalogBook currentBook;
  CatalogEntry currentEntry;
  bool inBooks = false;
  bool inSections = false;
  bool sawRoot = false;
};

void onDashKey(void* ctx, const char* key, const size_t len) { static_cast<DashboardCtx*>(ctx)->key.set(key, len); }

void onDashObjectStart(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  s->scope.objectDepth++;
  if (s->scope.objectDepth == 1) s->sawRoot = true;
  if (s->scope.objectDepth == 2) {
    if (s->inBooks) {
      s->currentBook = CatalogBook{};
      s->scope.inItem = true;
    } else if (s->inSections) {
      s->currentEntry = CatalogEntry{};
      s->scope.inItem = true;
    }
  }
  s->key.clear();
}

void onDashObjectEnd(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.inItem && s->scope.objectDepth == 2) {
    if (s->inBooks && s->out->continueReading.size() < kMaxPageItems) {
      s->out->continueReading.push_back(std::move(s->currentBook));
    } else if (s->inSections && s->out->sections.size() < kMaxPageItems) {
      s->out->sections.push_back(std::move(s->currentEntry));
    }
    s->scope.inItem = false;
  }
  s->scope.objectDepth--;
  s->key.clear();
}

void onDashArrayStart(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  s->scope.arrayDepth++;
  if (s->scope.objectDepth == 1 && s->scope.arrayDepth == 1) {
    if (s->key.is("continueReading") || s->key.is("items")) {
      s->inBooks = true;
      s->scope.itemsArrayDepth = s->scope.arrayDepth;
    } else if (s->key.is("sections") || s->key.is("shelves")) {
      s->inSections = true;
      s->scope.itemsArrayDepth = s->scope.arrayDepth;
    }
  }
}

void onDashArrayEnd(void* ctx) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.arrayDepth == s->scope.itemsArrayDepth) {
    s->inBooks = false;
    s->inSections = false;
    s->scope.itemsArrayDepth = -1;
  }
  s->scope.arrayDepth--;
  s->key.clear();
}

void onDashString(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (!s->scope.inItem) return;
  const std::string_view text(value, len);
  if (s->inBooks) {
    assignBookString(s->currentBook, s->key, text);
  } else if (s->inSections) {
    assignEntryString(s->currentEntry, s->key, text);
  }
}

void onDashNumber(void* ctx, const char* value, const size_t len) {
  auto* s = static_cast<DashboardCtx*>(ctx);
  if (s->scope.inItem) {
    if (s->inBooks) {
      assignBookNumber(s->currentBook, s->key, value, len);
    } else if (s->inSections) {
      if (s->key.is("id")) {
        s->currentEntry.id.assign(value, len);
      } else if (s->key.is("count")) {
        s->currentEntry.count = static_cast<uint32_t>(parseLong(value, len));
      }
    }
    return;
  }
  if (s->scope.objectDepth != 1) return;
  if (s->key.is("totalBooks")) {
    s->out->totalBooks = static_cast<uint32_t>(parseLong(value, len));
  } else if (s->key.is("finishedBooks")) {
    s->out->finishedBooks = static_cast<uint32_t>(parseLong(value, len));
  }
}

void onDashBool(void*, bool) {}

void onDashNull(void* ctx) { static_cast<DashboardCtx*>(ctx)->key.clear(); }

bool decodeEntryPage(const std::string_view json, CatalogEntryPage& out) {
  out = CatalogEntryPage{};
  if (json.empty()) return false;
  out.items.reserve(kCatalogPageSize);

  EntryPageCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onEntryKey;
  callbacks.onString = onEntryString;
  callbacks.onNumber = onEntryNumber;
  callbacks.onBool = onEntryBool;
  callbacks.onNull = onEntryNull;
  callbacks.onObjectStart = onEntryObjectStart;
  callbacks.onObjectEnd = onEntryObjectEnd;
  callbacks.onArrayStart = onEntryArrayStart;
  callbacks.onArrayEnd = onEntryArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}

bool decodeDashboard(const std::string_view json, DashboardSummary& out) {
  out = DashboardSummary{};
  if (json.empty()) return false;
  out.continueReading.reserve(8);
  out.sections.reserve(8);

  DashboardCtx ctx;
  ctx.out = &out;
  JsonCallbacks callbacks{};
  callbacks.ctx = &ctx;
  callbacks.onKey = onDashKey;
  callbacks.onString = onDashString;
  callbacks.onNumber = onDashNumber;
  callbacks.onBool = onDashBool;
  callbacks.onNull = onDashNull;
  callbacks.onObjectStart = onDashObjectStart;
  callbacks.onObjectEnd = onDashObjectEnd;
  callbacks.onArrayStart = onDashArrayStart;
  callbacks.onArrayEnd = onDashArrayEnd;

  StreamingJsonParser parser(callbacks);
  parser.feed(json.data(), json.size());
  return !parser.hasError() && ctx.sawRoot && ctx.scope.objectDepth == 0;
}
}  // namespace bookorbit
