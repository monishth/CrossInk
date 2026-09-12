#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bookorbit {

// One downloadable file attached to a book.
//
// This lives in the book *detail* response, not the books list: the list
// carries only `formats` (a set of extension strings) and no file id at all, so
// a download cannot be issued straight from a listing row.
struct CatalogFile {
  uint32_t fileId = 0;
  uint32_t sizeBytes = 0;
  std::string format;  // normalized extension, e.g. "epub"
  std::string role;    // "primary" | "content"
};

// Reads files[] out of GET /koreader/plugin/catalog/books/{bookId}.
// A detail response with no files[] decodes successfully and yields none.
bool decodeBookDetailFiles(std::string_view json, std::vector<CatalogFile>& out);

// Chooses what to download: the first EPUB, else the first file with a usable
// id. Returns nullptr when nothing is downloadable. The pointer is into `files`
// and is valid only while it lives.
const CatalogFile* pickDownloadableFile(const std::vector<CatalogFile>& files);

std::string bookDetailPath(uint32_t bookId);
std::string fileDownloadPath(uint32_t fileId);

}  // namespace bookorbit
