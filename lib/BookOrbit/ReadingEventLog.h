#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "IBlobStore.h"
#include "ReadingEvent.h"

namespace bookorbit {

// Matches KOReader's MAX_PAGETURNS_BEFORE_FLUSH and satisfies AGENTS.md
// resource rule 8 (do not write persistently on every page turn).
inline constexpr size_t kFlushEveryNEvents = 50;

// Append-only per-book reading event log. Buffers in RAM and flushes in
// batches; a crash loses at most kFlushEveryNEvents events, exactly as
// KOReader's in-memory page_stat buffer does.
class ReadingEventLog {
 public:
  ReadingEventLog(IBlobStore& store, std::string path);

  void append(const ReadingEvent& event);
  bool flush();
  size_t pendingCount() const { return pending.size(); }

  // Events with startTime > watermark, ordered by (startTime, page), capped at
  // limit. Includes still-buffered events so a sync never misses recent reading.
  bool readAfter(uint32_t watermark, size_t limit, std::vector<ReadingEvent>& out) const;

  bool maxStartTime(uint32_t& out) const;

 private:
  bool readAll(std::vector<ReadingEvent>& out) const;

  IBlobStore& blobs;
  std::string path;
  std::vector<ReadingEvent> pending;
};

}  // namespace bookorbit
