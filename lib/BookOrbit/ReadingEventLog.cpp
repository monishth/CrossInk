#include "ReadingEventLog.h"

#include <algorithm>
#include <utility>

namespace bookorbit {

ReadingEventLog::ReadingEventLog(IBlobStore& store, std::string logPath) : blobs(store), path(std::move(logPath)) {}

void ReadingEventLog::append(const ReadingEvent& event) {
  pending.push_back(event);
  if (pending.size() >= kFlushEveryNEvents) {
    flush();
  }
}

bool ReadingEventLog::flush() {
  if (pending.empty()) return true;

  std::vector<uint8_t> raw;
  raw.resize(pending.size() * kEventBytes);
  for (size_t i = 0; i < pending.size(); i++) {
    encodeEvent(pending[i], raw.data() + i * kEventBytes);
  }
  if (!blobs.append(path, raw.data(), raw.size())) {
    return false;  // keep pending so the next flush retries
  }
  pending.clear();
  return true;
}

bool ReadingEventLog::readAll(std::vector<ReadingEvent>& out) const {
  out.clear();
  std::vector<uint8_t> raw;
  if (blobs.read(path, raw)) {
    // A truncated trailing record (power loss mid-write) is ignored rather
    // than invalidating the whole log.
    const size_t whole = raw.size() / kEventBytes;
    out.reserve(whole + pending.size());
    for (size_t i = 0; i < whole; i++) {
      ReadingEvent event;
      if (decodeEvent(raw.data() + i * kEventBytes, event)) {
        out.push_back(event);
      }
    }
  }
  out.insert(out.end(), pending.begin(), pending.end());
  return true;
}

bool ReadingEventLog::readAfter(const uint32_t watermark, const size_t limit, std::vector<ReadingEvent>& out) const {
  std::vector<ReadingEvent> all;
  if (!readAll(all)) return false;

  out.clear();
  for (const auto& event : all) {
    if (event.startTime > watermark) {
      out.push_back(event);
    }
  }
  std::sort(out.begin(), out.end(), [](const ReadingEvent& a, const ReadingEvent& b) {
    if (a.startTime != b.startTime) return a.startTime < b.startTime;
    return a.page < b.page;
  });
  if (out.size() > limit) {
    out.resize(limit);
  }
  return true;
}

bool ReadingEventLog::maxStartTime(uint32_t& out) const {
  std::vector<ReadingEvent> all;
  if (!readAll(all) || all.empty()) return false;
  uint32_t latest = 0;
  for (const auto& event : all) {
    latest = std::max(latest, event.startTime);
  }
  out = latest;
  return true;
}

}  // namespace bookorbit
