#pragma once
#include <SecureHttpClient.h>

#include <cstddef>
#include <string>

#include "BookOrbitError.h"
#include "CatalogDownload.h"
#include "CatalogThumbnail.h"

/**
 * Streams a catalog file or thumbnail straight into a PartFileWriter.
 *
 * Never buffers a body: SecureHttpClient's DataCallback GET overload hands
 * each socket chunk to the writer, which appends it to the .part file. Peak RAM
 * is the client's own 2 KB chunk regardless of file size, which is what keeps a
 * large book safe on an ESP32-C3.
 *
 * Redirects are followed up to bookorbit::kMaxRedirectHops and only within the
 * request's own origin; a cross-origin or scheme-downgrading hop is refused
 * rather than followed, because the auth headers travel with the request.
 */
class BookOrbitStreamDownloader {
 public:
  // Plain function pointer rather than std::function: this is called once per
  // socket chunk, and AGENTS.md asks hot paths to avoid std::function.
  using ProgressFn = bool (*)(void* ctx, size_t downloaded, size_t total);

  BookOrbitStreamDownloader(const char* caCertPem, std::string username, std::string userkey)
      : caCertPem_(caCertPem), username_(std::move(username)), userkey_(std::move(userkey)) {}

  bookorbit::Error downloadTo(const std::string& url, bookorbit::PartFileWriter& writer, size_t expectedBytes,
                              ProgressFn progress, void* progressCtx);

  bookorbit::Error downloadThumbnail(const std::string& url, bookorbit::ThumbnailGate& gate);

 private:
  void configure(freeink::SecureHttpClient& http, const std::string& url, const char* accept);

  const char* caCertPem_;
  std::string username_;
  std::string userkey_;
};
