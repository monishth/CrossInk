#include <gtest/gtest.h>

#include "lib/BookOrbit/PageResolver.h"

using bookorbit::kNominalPageBytes;
using bookorbit::PageSource;
using bookorbit::resolvePage;

TEST(PageResolver, PrefersStableReferencePages) {
  PageSource source;
  source.hasStablePages = true;
  source.referencePage = 42;
  source.referencePageCount = 310;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(page, 42u);
  EXPECT_EQ(total, 310);
}

TEST(PageResolver, FallsBackToByteBasedPages) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.5f;
  source.bookSize = 100 * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 100);
  EXPECT_EQ(page, 51u);  // floor(0.5 * 100) + 1
}

TEST(PageResolver, FallbackRoundsPartialPageUp) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.0f;
  source.bookSize = kNominalPageBytes + 1;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 2);  // ceil(2049 / 2048)
  EXPECT_EQ(page, 1u);
}

TEST(PageResolver, FallbackClampsAtFinalPage) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 1.0f;
  source.bookSize = 10 * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 10);
  EXPECT_EQ(page, 10u);  // never total + 1
}

TEST(PageResolver, RejectsEmptyBook) {
  PageSource source;
  source.hasStablePages = false;
  source.bookSize = 0;

  uint32_t page = 0;
  uint16_t total = 0;
  EXPECT_FALSE(resolvePage(source, page, total));
}

TEST(PageResolver, RejectsZeroReferencePageCount) {
  PageSource source;
  source.hasStablePages = true;
  source.referencePage = 1;
  source.referencePageCount = 0;

  uint32_t page = 0;
  uint16_t total = 0;
  EXPECT_FALSE(resolvePage(source, page, total));
}

// totalPages is a uint16_t on the wire; a pathological book must not wrap.
TEST(PageResolver, ClampsTotalToSixteenBitMaximum) {
  PageSource source;
  source.hasStablePages = false;
  source.sizeProgress = 0.0f;
  source.bookSize = static_cast<size_t>(70000) * kNominalPageBytes;

  uint32_t page = 0;
  uint16_t total = 0;
  ASSERT_TRUE(resolvePage(source, page, total));
  EXPECT_EQ(total, 0xFFFF);
}
