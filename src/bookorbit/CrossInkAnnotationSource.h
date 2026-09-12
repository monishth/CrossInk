#pragma once

#include <Epub.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "BookOrbitAnnotationModel.h"
#include "BookOrbitAnnotationSync.h"
#include "BookOrbitBookmarkModel.h"

// Local device time as "YYYY-MM-DD HH:MM:SS" — the exact shape BookOrbit keys
// on. A wrong shape is not a cosmetic problem: it is a different identity key.
std::string crossinkFormatDeviceDatetime(uint32_t unixTime);

// Reads the currently loaded ClippingStore / BookmarkStore for the open book
// and produces wire-shaped records. Both return false when no book is loaded.
// Loads ClippingStore and BookmarkStore for this book.
//
// Both are per-book singletons that hold no path until loadForBook() is called,
// and they silently refuse every read and write until then. The sync path never
// opens the reader, so nothing else does this for us: without it the exchange
// uploads an empty key set (making the server think the device has nothing) and
// then fails to store everything the server sends back.
bool prepareStoresForBook(const std::shared_ptr<Epub>& epub);

bool collectBookOrbitAnnotations(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Annotation>& out);
bool collectBookOrbitBookmarks(const std::shared_ptr<Epub>& epub, std::vector<bookorbit::Bookmark>& out);

// Applies server-side changes to the local stores. One instance serves one
// route: bookmarksRoute selects BookmarkStore over ClippingStore.
class CrossInkAnnotationApplier : public bookorbit::IAnnotationApplier {
 public:
  CrossInkAnnotationApplier(std::shared_ptr<Epub> epub, bool bookmarksRoute);

  size_t applyAdds(const std::vector<bookorbit::RemoteEntry>& adds, std::vector<bookorbit::AppliedAck>& acks) override;
  size_t applyDeletes(const std::vector<bookorbit::RemoteEntry>& deletes,
                      std::vector<bookorbit::DeletedAck>& acks) override;

 private:
  std::shared_ptr<Epub> epub;
  bool bookmarksRoute;
};
