#include "BookOrbitFileSink.h"

#include <Logging.h>

namespace {
constexpr const char* kTag = "BOFILE";
}

bool BookOrbitFileSink::open(const std::string_view path) {
  close();
  scratch_.assign(path);

  // SdFat will not create a file whose parent directory is missing, and a
  // download target is chosen by the caller rather than by the user browsing to
  // it — so the folder may genuinely not exist yet. Create it rather than
  // failing with an error that looks like a permissions or card problem.
  const auto lastSlash = scratch_.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    const std::string parent = scratch_.substr(0, lastSlash);
    if (!Storage.exists(parent.c_str()) && !Storage.mkdir(parent.c_str(), true)) {
      LOG_ERR(kTag, "Could not create %s", parent.c_str());
      return false;
    }
  }

  if (!Storage.remove(scratch_.c_str()) && Storage.exists(scratch_.c_str())) {
    LOG_ERR(kTag, "Could not clear %s", scratch_.c_str());
    return false;
  }
  file_ = Storage.open(scratch_.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file_) {
    LOG_ERR(kTag, "Could not open %s for writing", scratch_.c_str());
    return false;
  }
  open_ = true;
  return true;
}

bool BookOrbitFileSink::write(const uint8_t* data, const size_t len) {
  if (!open_) {
    LOG_ERR(kTag, "Write with no file open");
    return false;
  }
  if (file_.write(data, len) != len) {
    LOG_ERR(kTag, "Short write of %u bytes", static_cast<unsigned>(len));
    return false;
  }
  return true;
}

bool BookOrbitFileSink::close() {
  if (!open_) return true;
  file_.flush();
  file_.sync();
  file_.close();
  open_ = false;
  return true;
}

bool BookOrbitFileSink::publish(const std::string_view from, const std::string_view to) {
  close();
  const std::string source(from);
  const std::string target(to);
  // Remove any stale target first: rename over an existing name is not
  // guaranteed by SdFat.
  Storage.remove(target.c_str());
  if (!Storage.rename(source.c_str(), target.c_str())) {
    LOG_ERR(kTag, "Could not publish %s -> %s", source.c_str(), target.c_str());
    return false;
  }
  return true;
}

bool BookOrbitFileSink::remove(const std::string_view path) {
  const std::string target(path);
  return Storage.remove(target.c_str());
}

bool BookOrbitFileSink::exists(const std::string_view path) {
  const std::string target(path);
  return Storage.exists(target.c_str());
}
