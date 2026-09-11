#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "lib/BookOrbit/IBlobStore.h"
#include "lib/BookOrbit/ReadingEventLog.h"

namespace {

class FakeBlobStore : public bookorbit::IBlobStore {
 public:
  std::map<std::string, std::vector<uint8_t>> files;

  bool read(const std::string_view path, std::vector<uint8_t>& out) override {
    const auto it = files.find(std::string(path));
    if (it == files.end()) return false;
    out = it->second;
    return true;
  }
  bool write(const std::string_view path, const uint8_t* data, const size_t len) override {
    files[std::string(path)] = std::vector<uint8_t>(data, data + len);
    return true;
  }
  bool append(const std::string_view path, const uint8_t* data, const size_t len) override {
    auto& file = files[std::string(path)];
    file.insert(file.end(), data, data + len);
    return true;
  }
  bool rename(const std::string_view from, const std::string_view to) override {
    const auto it = files.find(std::string(from));
    if (it == files.end()) return false;
    files[std::string(to)] = it->second;
    files.erase(it);
    return true;
  }
  bool remove(const std::string_view path) override { return files.erase(std::string(path)) > 0; }
  bool exists(const std::string_view path) override { return files.count(std::string(path)) > 0; }
};

bookorbit::ReadingEvent makeEvent(const uint32_t page, const uint32_t startTime, const uint16_t duration) {
  bookorbit::ReadingEvent event;
  event.page = page;
  event.startTime = startTime;
  event.durationSeconds = duration;
  event.totalPages = 310;
  return event;
}

}  // namespace

using bookorbit::kFlushEveryNEvents;
using bookorbit::ReadingEvent;
using bookorbit::ReadingEventLog;

TEST(ReadingEventLog, AppendBuffersWithoutWriting) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  EXPECT_EQ(log.pendingCount(), 1u);
  EXPECT_FALSE(store.exists("/events.bin"));
}

TEST(ReadingEventLog, FlushPersistsBufferedEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 1010, 20));
  ASSERT_TRUE(log.flush());
  EXPECT_EQ(log.pendingCount(), 0u);
  EXPECT_EQ(store.files["/events.bin"].size(), 2u * bookorbit::kEventBytes);
}

// AGENTS.md resource rule 8: do not write on every page turn.
TEST(ReadingEventLog, AutoFlushesAtFiftyEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  EXPECT_EQ(kFlushEveryNEvents, 50u);
  for (uint32_t i = 0; i < kFlushEveryNEvents; i++) {
    log.append(makeEvent(i + 1, 1000 + i, 10));
  }
  EXPECT_EQ(log.pendingCount(), 0u);
  EXPECT_EQ(store.files["/events.bin"].size(), kFlushEveryNEvents * bookorbit::kEventBytes);
}

TEST(ReadingEventLog, AppendsRatherThanOverwriting) {
  FakeBlobStore store;
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(1, 1000, 10));
    ASSERT_TRUE(log.flush());
  }
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(2, 1010, 20));
    ASSERT_TRUE(log.flush());
  }
  EXPECT_EQ(store.files["/events.bin"].size(), 2u * bookorbit::kEventBytes);
}

TEST(ReadingEventLog, ReadAfterFiltersByWatermark) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 2000, 20));
  log.append(makeEvent(3, 3000, 30));
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(1000, 500, out));
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].startTime, 2000u);
  EXPECT_EQ(out[1].startTime, 3000u);
}

TEST(ReadingEventLog, ReadAfterRespectsLimit) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  for (uint32_t i = 0; i < 10; i++) {
    log.append(makeEvent(i + 1, 1000 + i, 10));
  }
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 4, out));
  EXPECT_EQ(out.size(), 4u);
}

// Matches the Lua client's "ORDER BY start_time, page".
TEST(ReadingEventLog, ReadAfterOrdersByStartTimeThenPage) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(5, 2000, 10));
  log.append(makeEvent(2, 2000, 10));
  log.append(makeEvent(9, 1000, 10));
  ASSERT_TRUE(log.flush());

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].startTime, 1000u);
  EXPECT_EQ(out[1].page, 2u);
  EXPECT_EQ(out[2].page, 5u);
}

TEST(ReadingEventLog, ReadAfterIncludesPendingEvents) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  ASSERT_TRUE(log.flush());
  log.append(makeEvent(2, 2000, 20));  // still buffered

  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  EXPECT_EQ(out.size(), 2u);
}

TEST(ReadingEventLog, MaxStartTimeReportsLatest) {
  FakeBlobStore store;
  ReadingEventLog log(store, "/events.bin");
  log.append(makeEvent(1, 1000, 10));
  log.append(makeEvent(2, 5000, 20));
  ASSERT_TRUE(log.flush());

  uint32_t latest = 0;
  ASSERT_TRUE(log.maxStartTime(latest));
  EXPECT_EQ(latest, 5000u);
}

TEST(ReadingEventLog, MaxStartTimeFailsOnEmptyLog) {
  FakeBlobStore store;
  const ReadingEventLog log(store, "/events.bin");
  uint32_t latest = 0;
  EXPECT_FALSE(log.maxStartTime(latest));
}

// A truncated tail (power loss mid-write) must not corrupt the whole log.
TEST(ReadingEventLog, IgnoresTruncatedTrailingRecord) {
  FakeBlobStore store;
  {
    ReadingEventLog log(store, "/events.bin");
    log.append(makeEvent(1, 1000, 10));
    ASSERT_TRUE(log.flush());
  }
  store.files["/events.bin"].resize(bookorbit::kEventBytes + 7);  // half a record

  const ReadingEventLog log(store, "/events.bin");
  std::vector<ReadingEvent> out;
  ASSERT_TRUE(log.readAfter(0, 500, out));
  EXPECT_EQ(out.size(), 1u);
}
