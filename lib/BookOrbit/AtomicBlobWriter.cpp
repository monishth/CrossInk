#include "AtomicBlobWriter.h"

#include <string>

namespace bookorbit {
namespace {

std::string withSuffix(const std::string_view path, const char* suffix) { return std::string(path) + suffix; }

}  // namespace

bool atomicWriteBlob(IBlobStore& store, const std::string_view path, const uint8_t* data, const size_t len) {
  const std::string tmp = withSuffix(path, ".tmp");
  const std::string bak = withSuffix(path, ".bak");

  if (!store.write(tmp, data, len)) {
    store.remove(tmp);
    return false;
  }

  std::vector<uint8_t> verify;
  if (!store.read(tmp, verify) || verify.size() != len) {
    store.remove(tmp);
    return false;
  }

  if (store.exists(path)) {
    store.remove(bak);
    // A failed rotation is not fatal: the primary is still good and the
    // rename below is what actually publishes the new version.
    store.rename(path, bak);
  }

  if (!store.rename(tmp, path)) {
    store.remove(tmp);
    return false;
  }
  return true;
}

bool readBlobWithBackup(IBlobStore& store, const std::string_view path, std::vector<uint8_t>& out) {
  if (store.read(path, out)) return true;
  return store.read(withSuffix(path, ".bak"), out);
}

}  // namespace bookorbit
