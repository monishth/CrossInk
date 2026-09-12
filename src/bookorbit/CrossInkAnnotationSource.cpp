#include "CrossInkAnnotationSource.h"

#include <Logging.h>

#include <ctime>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitBookmarkModel.h"
#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "Epub.h"
#include "ProgressMapper.h"
#include "SpineText.h"
#include "XPointer.h"
#include "XPointerResolver.h"

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

namespace {

// The position for one anchored entry, or "" when it has no anchor or the
// anchor does not resolve.
//
// ProgressMapper's resolvers only understand pre-2020 crengine DOM, so against
// a document any current KOReader wrote they resolve nothing and fall through
// to a synthetic "/body/DocFragment[N]/body" -- a whole chapter, which is both
// the wrong place and the same identity for every highlight in it. This is the
// P2 mapper instead, the one the corpus validates in both directions.
std::string positionFromAnchor(SpineTextCache& spines, const int spineIndex, const uint32_t visibleTextOffset) {
  if (visibleTextOffset == CLIPPING_VISIBLE_OFFSET_NONE || spineIndex < 0) return {};

  const std::string* xhtml = spines.get(spineIndex);
  if (xhtml == nullptr) return {};

  bookorbit::XPointer anchor;
  if (!bookorbit::resolveOffsetToXPointer(*xhtml, visibleTextOffset, spineIndex, anchor)) {
    LOG_ERR("BORB", "offset %u did not resolve in spine %d", static_cast<unsigned>(visibleTextOffset), spineIndex);
    return {};
  }
  return bookorbit::emitXPointer(anchor);
}

// Where an incoming xpointer lands, in the terms the local stores use.
struct IncomingLanding {
  int spineIndex = -1;
  uint32_t visibleTextOffset = CLIPPING_VISIBLE_OFFSET_NONE;
  float withinSpine = 0.0f;  // 0..1 through the spine item's visible text
  bool resolved = false;
};

// Resolves through the P2 mapper, the same path BookOrbitProgressPhase::pull
// uses. ProgressMapper::toCrossPoint cannot read modern crengine DOM and
// silently returns spine 0, which is why every highlight this device accepted
// from the server landed at the start of the book.
IncomingLanding resolveIncoming(SpineTextCache& spines, const std::string& canonical) {
  IncomingLanding landing;

  bookorbit::XPointer target;
  if (!bookorbit::parseXPointer(canonical, target)) return landing;
  // Synthetic boxing steps exist in crengine's DOM but never in the source
  // XHTML walked here.
  bookorbit::stripSyntheticSteps(target);

  const int spineIndex = target.docFragment - 1;
  const std::string* xhtml = spines.get(spineIndex);
  if (xhtml == nullptr) return landing;

  uint32_t offset = 0;
  uint32_t length = 0;
  if (!bookorbit::resolveXPointerToOffset(*xhtml, target, offset) || !bookorbit::visibleTextLength(*xhtml, length) ||
      length == 0) {
    return landing;
  }

  landing.spineIndex = spineIndex;
  landing.visibleTextOffset = offset;
  landing.withinSpine = static_cast<float>(offset) / static_cast<float>(length);
  landing.resolved = true;
  return landing;
}

// The position for one stored clipping: the anchor when it has one, the legacy
// mapping otherwise. Collection and delete-matching MUST agree, or a delete
// recomputes a key that never matches what was uploaded and silently does
// nothing, so both go through here.
std::string bookmarkPosition(SpineTextCache& spines, const std::shared_ptr<Epub>& epub, const Bookmark& bookmark) {
  std::string pos = positionFromAnchor(spines, bookmark.spineIndex, bookmark.visibleTextOffset);
  if (!pos.empty()) return pos;

  CrossPointPosition position;
  position.spineIndex = bookmark.spineIndex;
  // BookmarkStore keeps intra-spine progress, not a page. Pages move with font
  // size; the fraction does not, so rebuild the page from it.
  position.totalPages = 1000;
  position.pageNumber = static_cast<int>(bookmark.progress * 1000.0f);
  position.paragraphIndex = bookmark.paragraphIndex;
  position.hasParagraphIndex = bookmark.paragraphIndex != UINT16_MAX;
  return ProgressMapper::toKOReader(epub, position).xpath;
}

std::string clippingPosition(SpineTextCache& spines, const std::shared_ptr<Epub>& epub, const Clipping& clipping) {
  std::string pos = positionFromAnchor(spines, clipping.spineIndex, clipping.visibleTextOffset);
  if (!pos.empty()) return pos;

  CrossPointPosition position;
  position.spineIndex = clipping.spineIndex;
  position.pageNumber = clipping.startPage;
  position.totalPages = clipping.pageCount;
  position.paragraphIndex = clipping.paragraphIndex;
  position.hasParagraphIndex = clipping.paragraphIndex != UINT16_MAX;
  return ProgressMapper::toKOReader(epub, position).xpath;
}

}  // namespace

bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out) {
  if (!epub) {
    LOG_ERR("BORB", "no book loaded, cannot collect highlights");
    return false;
  }

  const auto& clippings = CLIPPINGS.getClippings();
  out.clear();
  out.reserve(clippings.size());

  SpineTextCache spines(epub);
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
    // Anchored entries get the validated mapping; ones saved before the anchor
    // existed keep the legacy behaviour rather than losing their position.
    entry.pos0 = clippingPosition(spines, epub, clipping);
    entry.pos1 = clipping.endPage == clipping.startPage ? entry.pos0 : ProgressMapper::toKOReader(epub, end).xpath;
    if (entry.pos1.empty()) entry.pos1 = entry.pos0;

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

  SpineTextCache spines(epub);
  for (const Bookmark& bookmark : bookmarks) {
    bookorbit::Bookmark entry;
    entry.datetime = crossinkFormatDeviceDatetime(bookmark.timestamp);
    entry.chapter = bookmark.chapterTitle;
    entry.pos = bookmarkPosition(spines, epub, bookmark);
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

  SpineTextCache spines(epub);
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

    const IncomingLanding landing = resolveIncoming(spines, canonical);
    CrossPointPosition local;
    if (landing.resolved) {
      // The page is rebuilt from the fraction rather than carried: page counts
      // belong to a layout, and this one was written by a different device.
      // The offset is the anchor that survives, and it is what goes back out.
      local.spineIndex = landing.spineIndex;
      local.totalPages = 1000;
      local.pageNumber = static_cast<int>(landing.withinSpine * 1000.0f);
      local.valid = true;
    } else {
      // Legacy documents still resolve through the old mapper.
      KOReaderPosition incoming;
      incoming.xpath = canonical;
      incoming.percentage = 0.0f;
      local = ProgressMapper::toCrossPoint(epub, incoming);
    }
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
                                     remote.chapter.c_str(), local.paragraphIndex, nullptr,
                                     landing.visibleTextOffset) == BookmarkStore::AddResult::Added;
    } else {
      stored = CLIPPINGS.addClipping(static_cast<uint16_t>(local.spineIndex), static_cast<uint16_t>(local.pageNumber),
                                     static_cast<uint16_t>(local.pageNumber), static_cast<uint16_t>(local.totalPages),
                                     0, 0, 0, remote.chapter.c_str(), local.paragraphIndex, remote.text, UINT16_MAX, 0,
                                     landing.visibleTextOffset) == ClippingStore::AddResult::Added;
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

  SpineTextCache spines(epub);
  for (const auto& remote : deletes) {
    if (bookmarksRoute) {
      const auto& bookmarks = BOOKMARKS.getBookmarks();
      for (size_t index = 0; index < bookmarks.size(); index++) {
        const std::string datetime = crossinkFormatDeviceDatetime(bookmarks[index].timestamp);
        const std::string pos = bookorbit::normalizeXPointer(bookmarkPosition(spines, epub, bookmarks[index]));
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
        const std::string pos = bookorbit::normalizeXPointer(clippingPosition(spines, epub, clippings[index]));
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
