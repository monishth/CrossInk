#include <gtest/gtest.h>

#include <cstdint>

#include "lib/BookOrbit/ReadingEvent.h"

using bookorbit::decodeEvent;
using bookorbit::encodeEvent;
using bookorbit::kEventBytes;
using bookorbit::ReadingEvent;

TEST(ReadingEvent, RecordIsSixteenBytes) {
  EXPECT_EQ(kEventBytes, 16u);
  EXPECT_EQ(sizeof(ReadingEvent), 16u);
}

TEST(ReadingEvent, RoundTripsAllFields) {
  ReadingEvent in;
  in.page = 42;
  in.startTime = 1787561453u;
  in.durationSeconds = 37;
  in.totalPages = 310;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);

  ReadingEvent out;
  ASSERT_TRUE(decodeEvent(buf, out));
  EXPECT_EQ(out.page, 42u);
  EXPECT_EQ(out.startTime, 1787561453u);
  EXPECT_EQ(out.durationSeconds, 37);
  EXPECT_EQ(out.totalPages, 310);
}

// Little-endian is explicit so a log written on one target reads on another.
TEST(ReadingEvent, EncodesLittleEndian) {
  ReadingEvent in;
  in.page = 0x04030201u;
  in.startTime = 0u;
  in.durationSeconds = 0;
  in.totalPages = 0;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);
  EXPECT_EQ(buf[0], 0x01);
  EXPECT_EQ(buf[1], 0x02);
  EXPECT_EQ(buf[2], 0x03);
  EXPECT_EQ(buf[3], 0x04);
}

TEST(ReadingEvent, HandlesMaximumValues) {
  ReadingEvent in;
  in.page = 0xFFFFFFFFu;
  in.startTime = 0xFFFFFFFFu;
  in.durationSeconds = 0xFFFF;
  in.totalPages = 0xFFFF;

  uint8_t buf[kEventBytes];
  encodeEvent(in, buf);

  ReadingEvent out;
  ASSERT_TRUE(decodeEvent(buf, out));
  EXPECT_EQ(out.page, 0xFFFFFFFFu);
  EXPECT_EQ(out.startTime, 0xFFFFFFFFu);
  EXPECT_EQ(out.durationSeconds, 0xFFFF);
  EXPECT_EQ(out.totalPages, 0xFFFF);
}
