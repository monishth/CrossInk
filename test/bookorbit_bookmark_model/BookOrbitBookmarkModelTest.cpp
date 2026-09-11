#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitBookmarkModel.h"
#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::Bookmark;
using bookorbit::buildBookmarkKey;
using bookorbit::collectBookmarkKeys;
using bookorbit::normalizeBookmarks;

namespace {

constexpr char kPos[] = "/body[1]/DocFragment[5]/body[1]/p[3]/text()[1].0";

Bookmark dogear() {
  Bookmark entry;
  entry.datetime = "2026-09-01 08:00:00";
  entry.pos = kPos;
  entry.chapter = "Chapter 5";
  return entry;
}

}  // namespace

TEST(BookOrbitBookmarkModel, KeepsTheSpecifiedFields) {
  Bookmark entry = dogear();
  entry.datetimeUpdated = "2026-09-02 09:00:00";
  entry.note = "start here";
  entry.pageno = 77;

  const auto normalized = normalizeBookmarks({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].datetime, "2026-09-01 08:00:00");
  EXPECT_EQ(normalized.entries[0].datetimeUpdated, "2026-09-02 09:00:00");
  EXPECT_EQ(normalized.entries[0].pos, kPos);
  EXPECT_EQ(normalized.entries[0].chapter, "Chapter 5");
  EXPECT_EQ(normalized.entries[0].note, "start here");
  EXPECT_EQ(normalized.entries[0].pageno, 77);
}

TEST(BookOrbitBookmarkModel, DropsEntriesWithMalformedDatetime) {
  Bookmark entry = dogear();
  entry.datetime = "2026-09-01";
  EXPECT_TRUE(normalizeBookmarks({entry}).entries.empty());
}

// A paging document stores a page number, not an xpointer. Those are out of
// scope for v1 and must be dropped rather than uploaded unkeyable.
TEST(BookOrbitBookmarkModel, DropsNonXPointerPositions) {
  Bookmark entry = dogear();
  entry.pos = "412";
  EXPECT_TRUE(normalizeBookmarks({entry}).entries.empty());
}

TEST(BookOrbitBookmarkModel, TruncatesChapterAndNote) {
  Bookmark entry = dogear();
  entry.chapter = std::string(550, 'c');
  entry.note = std::string(550, 'n');
  const auto normalized = normalizeBookmarks({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].chapter.size(), 500u);
  EXPECT_EQ(normalized.entries[0].note.size(), 500u);
}

TEST(BookOrbitBookmarkModel, SignatureMatchesTheLuaFormat) {
  EXPECT_EQ(normalizeBookmarks({dogear()}).signature, "1:2026-09-01 08:00:00:3677112071:3604523879");
  EXPECT_EQ(normalizeBookmarks({}).signature, "0::0:0");
}

// The whole point of hashing the note: a rename leaves count, datetime and
// position identical, and must still force an exchange.
TEST(BookOrbitBookmarkModel, RenamingADogearChangesTheSignature) {
  Bookmark renamed = dogear();
  renamed.note = "Read again";
  EXPECT_EQ(normalizeBookmarks({renamed}).signature, "1:2026-09-01 08:00:00:3770681859:2913110196");
  EXPECT_NE(normalizeBookmarks({dogear()}).signature, normalizeBookmarks({renamed}).signature);
}

TEST(BookOrbitBookmarkModel, KeyIsMd5OfDatetimePipePos) {
  EXPECT_EQ(buildBookmarkKey("2026-09-01 08:00:00", kPos),
            bookorbit::md5Hex(std::string("2026-09-01 08:00:00|") + kPos));
}

TEST(BookOrbitBookmarkModel, CollectKeysCarriesKeyAndDatetime) {
  const auto keys = collectBookmarkKeys(normalizeBookmarks({dogear()}).entries);
  ASSERT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys[0].k, buildBookmarkKey("2026-09-01 08:00:00", kPos));
  EXPECT_EQ(keys[0].dt, "2026-09-01 08:00:00");
}

TEST(BookOrbitBookmarkModel, CanonicalizesPositionsThroughXPointer) {
  Bookmark unindexed = dogear();
  unindexed.pos = "/body/DocFragment[5]/body/p[3]/text().0";
  const auto left = normalizeBookmarks({dogear()});
  const auto right = normalizeBookmarks({unindexed});
  ASSERT_EQ(right.entries.size(), 1u);
  EXPECT_EQ(left.entries[0].pos, right.entries[0].pos);
}

TEST(BookOrbitBookmarkModel, MaxDatetimePrefersDatetimeUpdated) {
  Bookmark edited = dogear();
  edited.datetimeUpdated = "2026-09-05 10:11:12";
  EXPECT_EQ(normalizeBookmarks({edited}).maxDatetime, "2026-09-05 10:11:12");
}
