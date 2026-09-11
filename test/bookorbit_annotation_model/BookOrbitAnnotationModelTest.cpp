#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lib/BookOrbit/BookOrbitAnnotationModel.h"
#include "lib/BookOrbit/BookOrbitMd5.h"

using bookorbit::Annotation;
using bookorbit::buildAnnotationKey;
using bookorbit::collectAnnotationKeys;
using bookorbit::normalizeAnnotations;

namespace {

constexpr char kPosA[] = "/body[1]/DocFragment[2]/body[1]/div[1]/p[7]/text()[1].0";
constexpr char kPosB[] = "/body[1]/DocFragment[3]/body[1]/p[42]/text()[1].17";

Annotation highlightA() {
  Annotation entry;
  entry.datetime = "2026-08-21 09:15:42";
  entry.drawer = "lighten";
  entry.text = "the sea, the sea";
  entry.chapter = "Chapter Two";
  entry.pageno = 31;
  entry.pos0 = kPosA;
  entry.pos1 = kPosA;
  return entry;
}

Annotation highlightB() {
  Annotation entry;
  entry.datetime = "2026-09-11 14:03:00";
  entry.datetimeUpdated = "2026-09-11 15:00:00";
  entry.drawer = "underscore";
  entry.color = "yellow";
  entry.text = "a second highlight";
  entry.note = "look this up";
  entry.pos0 = kPosB;
  entry.pos1 = kPosB;
  return entry;
}

}  // namespace

TEST(BookOrbitAnnotationModel, KeepsTheSpecifiedFields) {
  const auto normalized = normalizeAnnotations({highlightB()});
  ASSERT_EQ(normalized.entries.size(), 1u);
  const auto& entry = normalized.entries[0];
  EXPECT_EQ(entry.datetime, "2026-09-11 14:03:00");
  EXPECT_EQ(entry.datetimeUpdated, "2026-09-11 15:00:00");
  EXPECT_EQ(entry.drawer, "underscore");
  EXPECT_EQ(entry.color, "yellow");
  EXPECT_EQ(entry.text, "a second highlight");
  EXPECT_EQ(entry.note, "look this up");
  EXPECT_EQ(entry.posFormat, "xpointer");
  EXPECT_EQ(entry.pos0, kPosB);
  EXPECT_EQ(entry.pos1, kPosB);
}

TEST(BookOrbitAnnotationModel, AcceptsEveryAllowedDrawer) {
  for (const char* drawer : {"lighten", "underscore", "strikeout", "invert"}) {
    Annotation entry = highlightA();
    entry.drawer = drawer;
    EXPECT_EQ(normalizeAnnotations({entry}).entries.size(), 1u) << drawer;
  }
}

// drawer == "" marks a position-only bookmark; it belongs to the bookmark
// route, not the annotation route.
TEST(BookOrbitAnnotationModel, DropsEntriesWithNoDrawer) {
  Annotation entry = highlightA();
  entry.drawer.clear();
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsUnknownDrawers) {
  Annotation entry = highlightA();
  entry.drawer = "sparkle";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsEntriesWithMalformedDatetime) {
  Annotation entry = highlightA();
  entry.datetime = "2026-08-21T09:15:42Z";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

TEST(BookOrbitAnnotationModel, DropsMalformedDatetimeUpdatedButKeepsTheEntry) {
  Annotation entry = highlightA();
  entry.datetimeUpdated = "yesterday";
  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_TRUE(normalized.entries[0].datetimeUpdated.empty());
}

// An unresolvable position cannot be keyed, so it must never be uploaded.
TEST(BookOrbitAnnotationModel, DropsEntriesWhosePositionIsNotAnXPointer) {
  Annotation entry = highlightA();
  entry.pos0 = "page 42";
  EXPECT_TRUE(normalizeAnnotations({entry}).entries.empty());
}

// P2 canonicalizes; an indexed and an unindexed form of the same position
// must produce the same key, or the server sees two annotations.
TEST(BookOrbitAnnotationModel, CanonicalizesPositionsThroughXPointer) {
  Annotation indexed = highlightA();
  Annotation unindexed = highlightA();
  unindexed.pos0 = "/body/DocFragment[2]/body/div/p[7]/text().0";
  unindexed.pos1 = unindexed.pos0;

  const auto left = normalizeAnnotations({indexed});
  const auto right = normalizeAnnotations({unindexed});
  ASSERT_EQ(left.entries.size(), 1u);
  ASSERT_EQ(right.entries.size(), 1u);
  EXPECT_EQ(left.entries[0].pos0, right.entries[0].pos0);
  EXPECT_EQ(left.signature, right.signature);
}

TEST(BookOrbitAnnotationModel, TruncatesToTheSpecLimits) {
  Annotation entry = highlightA();
  entry.text = std::string(10050, 'a');
  entry.note = std::string(5050, 'b');
  entry.chapter = std::string(550, 'c');
  entry.color = std::string(40, 'd');

  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].text.size(), 10000u);
  EXPECT_EQ(normalized.entries[0].note.size(), 5000u);
  EXPECT_EQ(normalized.entries[0].chapter.size(), 500u);
  EXPECT_EQ(normalized.entries[0].color.size(), 30u);
}

// Truncation must not split a multi-byte character, or the JSON body carries
// an invalid UTF-8 sequence.
TEST(BookOrbitAnnotationModel, TruncationDoesNotSplitUtf8) {
  Annotation entry = highlightA();
  entry.chapter.clear();
  for (int i = 0; i < 200; i++) entry.chapter += "\xE2\x80\x94";  // em dash, 3 bytes
  const auto normalized = normalizeAnnotations({entry});
  ASSERT_EQ(normalized.entries.size(), 1u);
  const std::string& chapter = normalized.entries[0].chapter;
  EXPECT_LE(chapter.size(), 500u);
  EXPECT_EQ(chapter.size() % 3, 0u);
}

TEST(BookOrbitAnnotationModel, MaxDatetimePrefersDatetimeUpdated) {
  const auto normalized = normalizeAnnotations({highlightA(), highlightB()});
  EXPECT_EQ(normalized.maxDatetime, "2026-09-11 15:00:00");
}

// The signature is "count:maxDatetime:hash1:hash2", the exact string the Lua
// plugin writes, so a book synced by KOReader and by CrossInk agrees.
TEST(BookOrbitAnnotationModel, SignatureMatchesTheLuaFormat) {
  EXPECT_EQ(normalizeAnnotations({highlightA(), highlightB()}).signature,
            "2:2026-09-11 15:00:00:3677518418:4274252292");
  EXPECT_EQ(normalizeAnnotations({highlightA()}).signature, "1:2026-08-21 09:15:42:1143470404:3130326164");
  EXPECT_EQ(normalizeAnnotations({}).signature, "0::0:0");
}

// Order-independent: the signature is a sum and a mix, not a running digest.
TEST(BookOrbitAnnotationModel, SignatureIgnoresEntryOrder) {
  EXPECT_EQ(normalizeAnnotations({highlightA(), highlightB()}).signature,
            normalizeAnnotations({highlightB(), highlightA()}).signature);
}

// Count plus max datetime alone cannot see a delete-plus-add that keeps both
// unchanged; the per-entry hashes are what catch it.
TEST(BookOrbitAnnotationModel, SignatureChangesWhenAnEntryIsReplaced) {
  Annotation replacement = highlightA();
  replacement.pos0 = kPosB;
  replacement.pos1 = kPosB;
  EXPECT_NE(normalizeAnnotations({highlightA()}).signature, normalizeAnnotations({replacement}).signature);
}

TEST(BookOrbitAnnotationModel, SignatureFitsTheBookSyncStateField) {
  // BookSyncState::annSignature is char[48]; a 5-digit count plus a 19-char
  // datetime plus two 10-digit hashes is 47 characters at worst.
  EXPECT_LE(normalizeAnnotations({highlightA(), highlightB()}).signature.size(), 47u);
}

TEST(BookOrbitAnnotationModel, KeyIsMd5OfDatetimePipePos0) {
  EXPECT_EQ(buildAnnotationKey("2026-09-11 14:03:00", kPosB),
            bookorbit::md5Hex(std::string("2026-09-11 14:03:00|") + kPosB));
  EXPECT_EQ(buildAnnotationKey("2026-09-11 14:03:00", kPosB), "08759494897afa79aec0d37d83498d30");
}

TEST(BookOrbitAnnotationModel, CollectKeysCarriesKeyAndDatetime) {
  const auto normalized = normalizeAnnotations({highlightA(), highlightB()});
  const auto keys = collectAnnotationKeys(normalized.entries);
  ASSERT_EQ(keys.size(), 2u);
  EXPECT_EQ(keys[0].k, buildAnnotationKey("2026-08-21 09:15:42", kPosA));
  EXPECT_EQ(keys[0].dt, "2026-08-21 09:15:42");
  EXPECT_EQ(keys[1].dt, "2026-09-11 14:03:00");
}

// The key hashes datetime, never datetimeUpdated: an edited highlight keeps
// its identity.
TEST(BookOrbitAnnotationModel, EditingAnEntryDoesNotChangeItsKey) {
  Annotation edited = highlightB();
  edited.note = "a different note";
  edited.datetimeUpdated = "2026-09-12 08:00:00";
  const auto before = collectAnnotationKeys(normalizeAnnotations({highlightB()}).entries);
  const auto after = collectAnnotationKeys(normalizeAnnotations({edited}).entries);
  ASSERT_EQ(before.size(), 1u);
  ASSERT_EQ(after.size(), 1u);
  EXPECT_EQ(before[0].k, after[0].k);
}

TEST(BookOrbitAnnotationModel, AbsentPagenoStaysAbsent) {
  const auto normalized = normalizeAnnotations({highlightB()});
  ASSERT_EQ(normalized.entries.size(), 1u);
  EXPECT_EQ(normalized.entries[0].pageno, -1);
}
