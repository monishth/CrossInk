#include "BookOrbitLocalState.h"

#include <HalClock.h>
#include <Logging.h>

#include "BookOrbitBookState.h"
#include "BookOrbitDate.h"
#include "BookStateStore.h"
#include "CrossPointSettings.h"
#include "network/BookOrbitBlobStore.h"
#include "network/BookOrbitCredentialStore.h"
#include "network/BookOrbitDocumentHasher.h"

namespace {

constexpr char kModule[] = "BORB";
constexpr char kBookStatePath[] = "/.crosspoint/bookorbit_book_states.bin";

// Days since 1970-01-01 for a civil date, and the inverse. Howard Hinnant's
// algorithms — exact, branch-light, and free of any dependency on the C library
// timezone state, which is what makes them the right fit here.
long daysFromCivil(int y, const unsigned m, const unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + static_cast<long>(doe) - 719468L;
}

void civilFromDays(long z, uint16_t& year, uint8_t& month, uint8_t& day) {
  z += 719468L;
  const long era = (z >= 0 ? z : z - 146096L) / 146097L;
  const unsigned doe = static_cast<unsigned>(z - era * 146097L);
  const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const long y = static_cast<long>(yoe) + era * 400L;
  const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const unsigned mp = (5u * doy + 2u) / 153u;
  const unsigned d = doy - (153u * mp + 2u) / 5u + 1u;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  year = static_cast<uint16_t>(y + (m <= 2));
  month = static_cast<uint8_t>(m);
  day = static_cast<uint8_t>(d);
}

// Today's local date, or an invalid date when the RTC has nothing to offer.
//
// The RTC keeps UTC, so the configured offset is applied here — otherwise an
// edit made in the evening west of Greenwich would be stamped with tomorrow's
// date and could lose a conflict it should have won.
//
// An invalid date is not an error to shout about: it means this edit cannot
// participate in date-driven conflict resolution, and is refused rather than
// stamped with a fabricated date.
bookorbit::DateOnly todayLocal() {
  bookorbit::DateOnly date;
  if (!halClock.isAvailable()) return date;

  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return date;
  if (year == 0 || month == 0 || day == 0) return date;

  // clockUtcOffsetQ is quarter-hours biased by 48 (i.e. 48 == UTC+0).
  const long offsetMinutes = (static_cast<long>(SETTINGS.clockUtcOffsetQ) - 48L) * 15L;
  const long utcMinutes = daysFromCivil(year, month, day) * 1440L + hour * 60L + minute;
  const long localMinutes = utcMinutes + offsetMinutes;

  // Floor-divide so times before midnight UTC land on the previous local day.
  const long localDays = (localMinutes >= 0 ? localMinutes : localMinutes - 1439L) / 1440L;
  civilFromDays(localDays, date.year, date.month, date.day);
  return date;
}

// Opens the store and resolves the book's hash. Returns false when BookOrbit is
// switched off, the book cannot be hashed, or the store will not load.
bool openFor(const std::string& epubPath, BookOrbitBlobStore& blobs, bookorbit::BookStateStore& store,
             std::string& hashOut) {
  if (!BOOKORBIT_STORE.isEnabled()) return false;

  BookOrbitDocumentHasher hasher;
  hashOut = hasher.partialMd5(epubPath);
  if (hashOut.empty()) {
    LOG_ERR(kModule, "cannot hash %s", epubPath.c_str());
    return false;
  }
  (void)blobs;
  return store.load();
}

}  // namespace

namespace bookorbit_local {

bool recordCompletion(const std::string& epubPath, const bool isCompleted) {
  const bookorbit::DateOnly today = todayLocal();
  if (!today.valid()) {
    LOG_DBG(kModule, "no valid date; not recording completion for sync");
    return false;
  }

  BookOrbitBlobStore blobs;
  bookorbit::BookStateStore store(blobs, kBookStatePath);
  std::string hash;
  if (!openFor(epubPath, blobs, store, hash)) return false;

  auto& record = store.findOrCreate(hash);
  if (!bookorbit::applyCompletionToggle(record.local, isCompleted, today)) {
    return false;  // already in that state
  }
  return store.flush();
}

bool recordRating(const std::string& epubPath, const int rating) {
  const bookorbit::DateOnly today = todayLocal();
  if (!today.valid()) {
    LOG_DBG(kModule, "no valid date; not recording rating for sync");
    return false;
  }

  BookOrbitBlobStore blobs;
  bookorbit::BookStateStore store(blobs, kBookStatePath);
  std::string hash;
  if (!openFor(epubPath, blobs, store, hash)) return false;

  auto& record = store.findOrCreate(hash);
  if (rating <= 0) {
    record.local.clearRating(today);
  } else if (!record.local.setRating(rating, today)) {
    return false;
  }
  return store.flush();
}

int storedRating(const std::string& epubPath) {
  BookOrbitBlobStore blobs;
  bookorbit::BookStateStore store(blobs, kBookStatePath);
  std::string hash;
  if (!openFor(epubPath, blobs, store, hash)) return 0;

  const auto* record = store.find(hash);
  if (record == nullptr || !record->local.ratingSet) return 0;
  return static_cast<int>(record->local.rating);
}

}  // namespace bookorbit_local
