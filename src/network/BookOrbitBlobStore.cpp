#include "BookOrbitBlobStore.h"

#include <Logging.h>

namespace {

constexpr char kModule[] = "BORB";

// string_view is not null-terminated, and every HAL/SdFat entry point takes a
// C string. One small owning copy per call is the price of that boundary.
std::string cstr(const std::string_view path) { return std::string(path); }

}  // namespace

bool BookOrbitBlobStore::read(const std::string_view path, std::vector<uint8_t>& out) {
  out.clear();
  const std::string target = cstr(path);

  FsFile file;
  if (!Storage.openFileForRead(kModule, target, file)) {
    return false;  // absent is an ordinary outcome; the caller decides
  }

  const size_t size = file.size();
  if (size == 0) {
    file.close();
    return true;
  }

  out.resize(size);
  const int got = file.read(out.data(), size);
  file.close();

  if (got < 0 || static_cast<size_t>(got) != size) {
    LOG_ERR(kModule, "short read on %s (%d of %u)", target.c_str(), got, static_cast<unsigned>(size));
    out.clear();
    return false;
  }
  return true;
}

bool BookOrbitBlobStore::write(const std::string_view path, const uint8_t* data, const size_t len) {
  const std::string target = cstr(path);

  FsFile file;
  if (!Storage.openFileForWrite(kModule, target, file)) {
    LOG_ERR(kModule, "cannot open %s for write", target.c_str());
    return false;
  }

  const size_t written = len == 0 ? 0 : file.write(data, len);
  // Durability before the caller believes the write happened: AtomicBlobWriter
  // rotates and renames on the strength of this returning true.
  const bool flushed = file.sync();
  file.close();

  if (written != len || !flushed) {
    LOG_ERR(kModule, "short write on %s (%u of %u)", target.c_str(), static_cast<unsigned>(written),
            static_cast<unsigned>(len));
    return false;
  }
  return true;
}

bool BookOrbitBlobStore::append(const std::string_view path, const uint8_t* data, const size_t len) {
  if (len == 0) return true;
  const std::string target = cstr(path);

  // openFileForWrite truncates; the event log must only ever grow, so open the
  // raw handle with O_APPEND instead.
  FsFile file = Storage.open(target.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR(kModule, "cannot open %s for append", target.c_str());
    return false;
  }

  const size_t written = file.write(data, len);
  const bool flushed = file.sync();
  file.close();

  if (written != len || !flushed) {
    LOG_ERR(kModule, "short append on %s (%u of %u)", target.c_str(), static_cast<unsigned>(written),
            static_cast<unsigned>(len));
    return false;
  }
  return true;
}

bool BookOrbitBlobStore::rename(const std::string_view from, const std::string_view to) {
  const std::string source = cstr(from);
  const std::string target = cstr(to);
  if (!Storage.rename(source.c_str(), target.c_str())) {
    LOG_ERR(kModule, "rename %s -> %s failed", source.c_str(), target.c_str());
    return false;
  }
  return true;
}

bool BookOrbitBlobStore::remove(const std::string_view path) {
  const std::string target = cstr(path);
  return Storage.remove(target.c_str());
}

bool BookOrbitBlobStore::exists(const std::string_view path) {
  const std::string target = cstr(path);
  return Storage.exists(target.c_str());
}
