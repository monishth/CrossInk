#include "CrossInkAnnotationSource.h"

#include <Logging.h>

#include <ctime>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitBookmarkModel.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "Epub.h"
#include "ProgressMapper.h"
#include "XPointer.h"

std::string crossinkFormatDeviceDatetime(const uint32_t unixTime) {
  const time_t seconds = static_cast<time_t>(unixTime);
  struct tm parts = {};
  localtime_r(&seconds, &parts);
  char buffer[20];  // "YYYY-MM-DD HH:MM:SS" + NUL
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts);
  return buffer;
}

bool prepareStoresForBook(const std::shared_ptr<Epub>& epub) {
  if (!epub) return false;
  const std::string& path = epub->getPath();
  if (path.empty()) return false;

  const bool clippings = CLIPPINGS.loadForBook(path, epub->getTitle(), epub->getAuthor(), "epub");
  const bool bookmarks = BOOKMARKS.loadForBook(path, epub->getTitle(), epub->getAuthor(), "epub");
  if (!clippings) LOG_ERR("BORB", "could not load clippings for %s", path.c_str());
  if (!bookmarks) LOG_ERR("BORB", "could not load bookmarks for %s", path.c_str());
  return clippings && bookmarks;
}

bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out) {
  if (!epub) {
    LOG_ERR("BORB", "no book loaded, cannot collect highlights");
    return false;
  }

  const auto& clippings = CLIPPINGS.getClippings();
  out.clear();
  out.reserve(clippings.size());

  std::string text;
  for (size_t index = 0; index < clippings.size(); index++) {
    const Clipping& clipping = clippings[index];

    CrossPointPosition start;
    start.spineIndex = clipping.spineIndex;
    start.pageNumber = clipping.startPage;
    start.totalPages = clipping.pageCount;
    start.paragraphIndex = clipping.paragraphIndex;
    start.hasParagraphIndex = clipping.paragraphIndex != UINT16_MAX;

    CrossPointPosition end = start;
    end.pageNumber = clipping.endPage;

    bookorbit::Annotation entry;
    entry.datetime = crossinkFormatDeviceDatetime(clipping.timestamp);
    entry.drawer = "lighten";  // CrossInk renders exactly one highlight style
    entry.chapter = clipping.chapterTitle;
    entry.pageno = static_cast<int32_t>(clipping.startPage) + 1;
    entry.posFormat = "xpointer";
    entry.pos0 = ProgressMapper::toKOReader(epub, start).xpath;
    entry.pos1 = ProgressMapper::toKOReader(epub, end).xpath;

    text.clear();
    if (CLIPPINGS.readClippingText(index, text)) {
      entry.text = text;
    }

    // normalizeAnnotations() drops anything whose position is not a real
    // xpointer, so an unmappable clipping is skipped rather than uploaded
    // under a key the server cannot match.
    out.push_back(std::move(entry));
  }
  return true;
}

bool collectBookOrbitBookmarks(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Bookmark>& out) {
  if (!epub) {
    LOG_ERR("BORB", "no book loaded, cannot collect bookmarks");
    return false;
  }

  const auto& bookmarks = BOOKMARKS.getBookmarks();
  out.clear();
  out.reserve(bookmarks.size());

  for (const Bookmark& bookmark : bookmarks) {
    CrossPointPosition position;
    position.spineIndex = bookmark.spineIndex;
    // BookmarkStore keeps intra-spine progress, not a page. Pages move with
    // font size; the fraction does not, so rebuild the page from it.
    position.totalPages = 1000;
    position.pageNumber = static_cast<int>(bookmark.progress * 1000.0f);
    position.paragraphIndex = bookmark.paragraphIndex;
    position.hasParagraphIndex = bookmark.paragraphIndex != UINT16_MAX;

    bookorbit::Bookmark entry;
    entry.datetime = crossinkFormatDeviceDatetime(bookmark.timestamp);
    entry.chapter = bookmark.chapterTitle;
    entry.pos = ProgressMapper::toKOReader(epub, position).xpath;
    out.push_back(std::move(entry));
  }
  return true;
}

CrossInkAnnotationApplier::CrossInkAnnotationApplier(std::shared_ptr<Epub> epub, const bool bookmarksRoute)
    : epub(std::move(epub)), bookmarksRoute(bookmarksRoute) {}

size_t CrossInkAnnotationApplier::applyAdds(const std::vector<bookorbit::RemoteEntry>& adds,
                                            std::vector<bookorbit::AppliedAck>& acks) {
  size_t touched = 0;
  acks.reserve(acks.size() + adds.size());

  for (const auto& remote : adds) {
    bookorbit::AppliedAck ack;
    ack.serverId = remote.serverId;
    ack.version = remote.version;

    const std::string canonical = bookorbit::normalizeXPointer(remote.pos0);
    if (canonical.empty() || !epub) {
      LOG_ERR("BORB", "cannot resolve incoming position for server entry %s", remote.serverId.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    KOReaderPosition incoming;
    incoming.xpath = canonical;
    incoming.percentage = 0.0f;
    const CrossPointPosition local = ProgressMapper::toCrossPoint(epub, incoming);
    if (!local.valid) {
      LOG_ERR("BORB", "incoming position did not resolve: %s", canonical.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    const std::string datetime =
        remote.datetime.empty() ? crossinkFormatDeviceDatetime(static_cast<uint32_t>(time(nullptr))) : remote.datetime;

    bool stored = false;
    if (bookmarksRoute) {
      const float progress =
          local.totalPages > 0 ? static_cast<float>(local.pageNumber) / static_cast<float>(local.totalPages) : 0.0f;
      stored = BOOKMARKS.addBookmark(static_cast<uint16_t>(local.spineIndex), progress, local.totalPages,
                                     remote.chapter.c_str(), local.paragraphIndex,
                                     nullptr) == BookmarkStore::AddResult::Added;
    } else {
      stored = CLIPPINGS.addClipping(static_cast<uint16_t>(local.spineIndex), static_cast<uint16_t>(local.pageNumber),
                                     static_cast<uint16_t>(local.pageNumber), static_cast<uint16_t>(local.totalPages),
                                     0, 0, 0, remote.chapter.c_str(), local.paragraphIndex, remote.text, UINT16_MAX,
                                     0) == ClippingStore::AddResult::Added;
    }

    if (!stored) {
      LOG_ERR("BORB", "local store refused entry %s", remote.serverId.c_str());
      ack.failed = true;
      acks.push_back(ack);
      continue;
    }

    touched++;
    // Report the identity the entry actually got locally, so a later
    // server-side delete can address it.
    ack.datetime = datetime;
    ack.pos0 = canonical;
    ack.key = bookmarksRoute ? bookorbit::buildBookmarkKey(datetime, canonical)
                             : bookorbit::buildAnnotationKey(datetime, canonical);
    acks.push_back(ack);
  }

  if (touched > 0) {
    if (bookmarksRoute) {
      BOOKMARKS.saveToFile();
    } else {
      CLIPPINGS.saveToFile();
    }
  }
  return touched;
}

size_t CrossInkAnnotationApplier::applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                                               std::vector<bookorbit::DeletedAck>& acks) {
  size_t touched = 0;
  acks.reserve(acks.size() + deletes.size());

  for (const auto& remote : deletes) {
    if (bookmarksRoute) {
      const auto& bookmarks = BOOKMARKS.getBookmarks();
      for (size_t index = 0; index < bookmarks.size(); index++) {
        const std::string datetime = crossinkFormatDeviceDatetime(bookmarks[index].timestamp);
        CrossPointPosition position;
        position.spineIndex = bookmarks[index].spineIndex;
        position.totalPages = 1000;
        position.pageNumber = static_cast<int>(bookmarks[index].progress * 1000.0f);
        const std::string pos = bookorbit::normalizeXPointer(ProgressMapper::toKOReader(epub, position).xpath);
        if (!pos.empty() && bookorbit::buildBookmarkKey(datetime, pos) == remote.key) {
          BOOKMARKS.removeBookmarkAt(index);
          touched++;
          break;
        }
      }
    } else {
      const auto& clippings = CLIPPINGS.getClippings();
      for (size_t index = 0; index < clippings.size(); index++) {
        const std::string datetime = crossinkFormatDeviceDatetime(clippings[index].timestamp);
        CrossPointPosition position;
        position.spineIndex = clippings[index].spineIndex;
        position.pageNumber = clippings[index].startPage;
        position.totalPages = clippings[index].pageCount;
        const std::string pos = bookorbit::normalizeXPointer(ProgressMapper::toKOReader(epub, position).xpath);
        if (!pos.empty() && bookorbit::buildAnnotationKey(datetime, pos) == remote.key) {
          CLIPPINGS.removeClippingAt(index);
          touched++;
          break;
        }
      }
    }
    // Ack either way: a missing entry is already in the state the server wants.
    acks.push_back({remote.serverId, false});
  }

  if (touched > 0) {
    if (bookmarksRoute) {
      BOOKMARKS.saveToFile();
    } else {
      CLIPPINGS.saveToFile();
    }
  }
  return touched;
}
