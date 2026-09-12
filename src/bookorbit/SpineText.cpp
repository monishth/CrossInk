#include "SpineText.h"

#include <Logging.h>
#include <Print.h>

#include "Epub.h"

namespace {

constexpr char kModule[] = "BOST";

// Print sink that accumulates a streamed spine item into a std::string.
class StringPrint final : public Print {
 public:
  explicit StringPrint(std::string& target) : out(target) {}

  size_t write(const uint8_t byte) override {
    out.push_back(static_cast<char>(byte));
    return 1;
  }

  size_t write(const uint8_t* buffer, const size_t size) override {
    out.append(reinterpret_cast<const char*>(buffer), size);
    return size;
  }

 private:
  std::string& out;
};

}  // namespace

const std::string* SpineTextCache::get(const int spineIndex) {
  if (spineIndex >= 0 && spineIndex == cachedIndex && !cached.empty()) return &cached;

  release();
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) return nullptr;

  const auto item = epub->getSpineItem(spineIndex);
  if (item.href.empty()) return nullptr;

  StringPrint sink(cached);
  if (!epub->readItemContentsToStream(item.href, sink, 1024) || cached.empty()) {
    LOG_ERR(kModule, "could not read spine item %d", spineIndex);
    release();
    return nullptr;
  }

  cachedIndex = spineIndex;
  return &cached;
}

void SpineTextCache::release() {
  cachedIndex = -1;
  // shrink_to_fit is advisory; swapping with an empty string actually returns
  // the buffer, which matters when the next step needs a large allocation.
  std::string().swap(cached);
}
