#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "BookOrbitError.h"
#include "CatalogApi.h"

namespace bookorbit {

// A server that keeps rejecting the cursor must not spin a battery device.
// Three restarts is generous for a library edited mid-run and still bounded.
constexpr uint32_t kMaxManifestRestarts = 3;

struct ManifestItem {
  uint32_t bookId = 0;
  uint32_t fileId = 0;
  uint32_t fileBytes = 0;
  std::string hash;  // server-side partial MD5; matches what P0 match-check uses
  std::string title;
  std::string filename;
  std::string format;
};

struct ManifestPage {
  std::vector<ManifestItem> items;
  bool hasNext = false;
  bool restartRequired = false;
  std::string nextCursor;
  std::string manifestVersion;
};

// Streaming decode of one /catalog/manifest page. Resets `out`.
bool decodeManifestPage(std::string_view json, ManifestPage& out);

// Cursor-paginated walk over the bulk manifest. Only one page is ever held:
// the enumerator keeps a cursor string, not an accumulated list.
class ManifestEnumerator {
 public:
  ManifestEnumerator(CatalogApi& api, std::string deviceId) : api_(api), deviceId_(std::move(deviceId)) {}

  // Fetches the next page. When the server answers restartRequired the cursor
  // is dropped and enumeration restarts from the beginning inside this same
  // call — safe, because books already on device are never re-transferred.
  // Bounded by kMaxManifestRestarts, after which {ClientError, 409} is
  // returned and the walk stops.
  Error next(ManifestPage& out);

  bool hasNext() const { return hasNext_; }
  uint32_t restartCount() const { return restarts_; }
  const std::string& manifestVersion() const { return manifestVersion_; }

  void reset();

 private:
  Error fetch(ManifestPage& out);

  CatalogApi& api_;
  std::string deviceId_;
  std::string cursor_;
  std::string manifestVersion_;
  bool hasNext_ = true;  // true until a page says otherwise
  bool started_ = false;
  uint32_t restarts_ = 0;
};

}  // namespace bookorbit
