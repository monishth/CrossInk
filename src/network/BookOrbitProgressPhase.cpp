#include "BookOrbitProgressPhase.h"

#include <Logging.h>

#include <string>

#include "Epub.h"
#include "XPointer.h"
#include "XPointerResolver.h"

namespace {

// "/body/DocFragment[N]/body" with nothing after it: the synthetic form that
// names a chapter and no position inside it.
bool isChapterLevelXPointer(const std::string& xpath) {
  static constexpr char kSuffix[] = "]/body";
  return xpath.size() > sizeof(kSuffix) && xpath.rfind("/body/DocFragment[", 0) == 0 &&
         xpath.compare(xpath.size() - (sizeof(kSuffix) - 1), sizeof(kSuffix) - 1, kSuffix) == 0;
}

constexpr char kModule[] = "BOP";
constexpr char kProgressPutPath[] = "/koreader/syncs/progress";
constexpr char kBulkProgressPath[] = "/koreader/plugin/progress";

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

bool BookOrbitProgressPhase::readSpineItem(const int spineIndex, std::string& out) const {
  out.clear();
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) return false;

  const auto item = epub->getSpineItem(spineIndex);
  if (item.href.empty()) return false;

  StringPrint sink(out);
  if (!epub->readItemContentsToStream(item.href, sink, 1024)) {
    LOG_ERR(kModule, "could not read spine item %d", spineIndex);
    out.clear();
    return false;
  }
  return !out.empty();
}

float BookOrbitProgressPhase::spinePercentage(const int spineIndex, const float withinSpine) const {
  if (!epub) return 0.0f;
  // calculateSizeProgress already weights spine items by byte size, which is
  // the same basis the reader's own progress uses — so a resolved xpointer and
  // the local percentage are directly comparable.
  return epub->calculateSizeProgress(spineIndex, withinSpine);
}

bool BookOrbitProgressPhase::push(const std::string& md5, const CrossPointPosition& position, const float percentage) {
  bookorbit::ProgressRecord record;
  record.document = md5;
  record.percentage = percentage;
  record.progress = ProgressMapper::toKOReader(epub, position).xpath;
  record.device = deviceName;
  record.deviceId = deviceId;
  record.timestamp = nowUnix;

  // A chapter-level xpointer ("/body/DocFragment[N]/body") is what every
  // resolver falls back to, and it lands another reader at the start of the
  // chapter instead of where this one actually is. It is a legitimate answer
  // only at the very top of a chapter; anywhere else it means the precise
  // resolvers were skipped or failed, so say which, rather than shipping a
  // silently coarse position.
  if (isChapterLevelXPointer(record.progress)) {
    LOG_INF(kModule, "coarse position for %s: spine=%d page=%d/%d offset=%s -> %s", md5.c_str(), position.spineIndex,
            position.pageNumber, position.totalPages,
            position.hasVisibleTextOffset ? std::to_string(position.visibleTextOffset).c_str() : "none",
            record.progress.c_str());
  }

  // Never degrade silently: refuse to push half a record rather than write a
  // percentage-only row a KOReader client would then treat as authoritative.
  if (!bookorbit::isSendable(record)) {
    LOG_ERR(kModule, "refusing incomplete progress push for %s", md5.c_str());
    return false;
  }

  const std::string body = bookorbit::encodePutProgress(record);
  if (body.empty() || body.size() > bookorbit::kMaxBodyBytes) {
    LOG_ERR(kModule, "progress body rejected (%u bytes)", static_cast<unsigned>(body.size()));
    return false;
  }

  std::string response;
  return client.putJson(kProgressPutPath, body, response).status == bookorbit::Status::Ok;
}

bool BookOrbitProgressPhase::pull(const std::string& md5, const float localPercentage,
                                  bookorbit::ResolvedProgress& out) {
  std::string body;
  if (client.get(bookorbit::progressGetPath(md5), body).status != bookorbit::Status::Ok) return false;

  bookorbit::ProgressRecord remote;
  if (!bookorbit::decodeProgressResponse(body, remote)) return false;

  bookorbit::XPointer target;
  bool resolved = false;
  float resolvedPercentage = 0.0f;
  int resolvedSpine = -1;
  uint32_t resolvedOffset = 0;
  if (bookorbit::parseXPointer(remote.progress, target)) {
    // Legacy-DOM xpointers carry crengine's synthetic boxing steps, which have
    // no counterpart in the source XHTML we are about to walk.
    bookorbit::stripSyntheticSteps(target);

    const int spineIndex = target.docFragment - 1;
    std::string xhtml;
    if (readSpineItem(spineIndex, xhtml)) {
      uint32_t offset = 0;
      uint32_t length = 0;
      if (bookorbit::resolveXPointerToOffset(xhtml, target, offset) && bookorbit::visibleTextLength(xhtml, length) &&
          length > 0) {
        resolved = true;
        resolvedSpine = spineIndex;
        resolvedOffset = offset;
        resolvedPercentage = spinePercentage(spineIndex, static_cast<float>(offset) / static_cast<float>(length));
      }
    }
  }

  out = bookorbit::chooseRemoteProgress(remote, resolved, resolvedPercentage, localPercentage);
  // The spine and offset are what makes the landing applicable rather than
  // merely describable; they were being computed and discarded.
  if (out.source == bookorbit::ProgressSource::Xpointer) {
    out.spineIndex = resolvedSpine;
    out.visibleTextOffset = resolvedOffset;
  }
  return out.source != bookorbit::ProgressSource::None;
}

bool BookOrbitProgressPhase::pushBulk(const std::vector<bookorbit::BulkProgressItem>& items,
                                      std::vector<std::string>& unmatched) {
  unmatched.clear();
  if (items.empty()) return true;

  // encodeBulkProgress splits at the server's 100-item cap, so the loop walks
  // the input rather than assuming one request is enough.
  size_t offset = 0;
  while (offset < items.size()) {
    size_t consumed = 0;
    const std::string body = bookorbit::encodeBulkProgress(items, offset, consumed);
    if (body.empty() || consumed == 0) {
      LOG_ERR(kModule, "bulk progress encode stalled at %u", static_cast<unsigned>(offset));
      return false;
    }

    std::string response;
    // /koreader/plugin/* bodies must carry the device fields; PluginDeviceDto
    // rejects the request with 400 without them.
    if (client.postJson(kBulkProgressPath, client.withDeviceFields(body, nowUnix), response).status !=
        bookorbit::Status::Ok) {
      return false;
    }

    std::vector<std::string> batchUnmatched;
    if (!bookorbit::decodeBulkProgressResponse(response, batchUnmatched)) return false;
    unmatched.insert(unmatched.end(), batchUnmatched.begin(), batchUnmatched.end());

    offset += consumed;
  }
  return true;
}
