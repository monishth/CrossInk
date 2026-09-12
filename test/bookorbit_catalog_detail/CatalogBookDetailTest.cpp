#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/CatalogBookDetail.h"

using bookorbit::CatalogFile;
using bookorbit::decodeBookDetailFiles;
using bookorbit::pickDownloadableFile;

namespace {

// Shaped after the real response from koreader-catalog.service.ts:704-714:
// files[] entries carry id, format, role and sizeBytes.
const char* kDetail =
    R"({"id":42,"title":"We Solve Murders","authors":"Richard Osman","files":[)"
    R"({"id":7,"format":"epub","role":"primary","sizeBytes":1048576,"downloadUrl":"/koreader/plugin/catalog/files/7/download"},)"
    R"({"id":8,"format":"mp3","role":"content","sizeBytes":90000000,"downloadUrl":"/koreader/plugin/catalog/files/8/download"}]})";

}  // namespace

TEST(CatalogBookDetail, DecodesEveryFileEntry) {
  std::vector<CatalogFile> files;
  ASSERT_TRUE(decodeBookDetailFiles(kDetail, files));
  ASSERT_EQ(files.size(), 2u);

  EXPECT_EQ(files[0].fileId, 7u);
  EXPECT_EQ(files[0].format, "epub");
  EXPECT_EQ(files[0].role, "primary");
  EXPECT_EQ(files[0].sizeBytes, 1048576u);

  EXPECT_EQ(files[1].fileId, 8u);
  EXPECT_EQ(files[1].format, "mp3");
}

// The bug this whole unit exists to fix: the books *list* response carries no
// file id at all, so a download must be driven from the detail response.
TEST(CatalogBookDetail, PrefersAnEpubOverOtherFormats) {
  std::vector<CatalogFile> files;
  ASSERT_TRUE(decodeBookDetailFiles(kDetail, files));

  const CatalogFile* chosen = pickDownloadableFile(files);
  ASSERT_NE(chosen, nullptr);
  EXPECT_EQ(chosen->fileId, 7u);
  EXPECT_EQ(chosen->format, "epub");
}

TEST(CatalogBookDetail, FallsBackToTheFirstFileWhenNoEpubIsPresent) {
  std::vector<CatalogFile> files;
  ASSERT_TRUE(decodeBookDetailFiles(R"({"files":[{"id":3,"format":"pdf","role":"primary","sizeBytes":10}]})", files));

  const CatalogFile* chosen = pickDownloadableFile(files);
  ASSERT_NE(chosen, nullptr);
  EXPECT_EQ(chosen->fileId, 3u);
}

TEST(CatalogBookDetail, ReturnsNullWhenThereIsNothingToDownload) {
  std::vector<CatalogFile> empty;
  EXPECT_EQ(pickDownloadableFile(empty), nullptr);
}

TEST(CatalogBookDetail, IgnoresAFileWithoutAUsableId) {
  std::vector<CatalogFile> files;
  ASSERT_TRUE(decodeBookDetailFiles(R"({"files":[{"format":"epub","sizeBytes":10}]})", files));
  EXPECT_EQ(pickDownloadableFile(files), nullptr);
}

TEST(CatalogBookDetail, DetailWithNoFilesArrayDecodesEmpty) {
  std::vector<CatalogFile> files;
  EXPECT_TRUE(decodeBookDetailFiles(R"({"id":42,"title":"No files"})", files));
  EXPECT_TRUE(files.empty());
}

TEST(CatalogBookDetail, RejectsMalformedJson) {
  std::vector<CatalogFile> files;
  EXPECT_FALSE(decodeBookDetailFiles("{not json", files));
}

// The detail path is what the activity builds; pinned so a typo cannot slip
// past into a 404 the user only sees as "Download failed".
TEST(CatalogBookDetail, BuildsTheDetailPath) {
  EXPECT_EQ(bookorbit::bookDetailPath(42), "/koreader/plugin/catalog/books/42");
}

TEST(CatalogBookDetail, BuildsTheDownloadPath) {
  EXPECT_EQ(bookorbit::fileDownloadPath(7), "/koreader/plugin/catalog/files/7/download");
}
