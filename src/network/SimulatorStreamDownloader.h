#pragma once

#ifdef SIMULATOR

#include <cstddef>
#include <string>

#include "BookOrbitError.h"
#include "CatalogDownload.h"

/**
 * Streaming downloader over libcurl, for the desktop simulator.
 *
 * Mirrors BookOrbitStreamDownloader's shape exactly so the catalog activity
 * only has to pick a type, not branch. Body chunks go straight from the socket
 * into PartFileWriter, so memory use is independent of file size — the same
 * property that keeps a large book safe on the device.
 */
class SimulatorStreamDownloader {
 public:
  // Matches the device downloader's signature, including the plain function
  // pointer for progress (called once per chunk).
  using ProgressFn = bool (*)(void* ctx, size_t downloaded, size_t total);

  SimulatorStreamDownloader(const char* caCertPem, std::string username, std::string userkey)
      : caCertPem_(caCertPem), username_(std::move(username)), userkey_(std::move(userkey)) {}

  bookorbit::Error downloadTo(const std::string& url, bookorbit::PartFileWriter& writer, size_t expectedBytes,
                              ProgressFn progress, void* progressCtx);

 private:
  const char* caCertPem_;
  std::string username_;
  std::string userkey_;
};

#endif  // SIMULATOR
