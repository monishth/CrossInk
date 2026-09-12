#include "BookOrbitStreamDownloader.h"

#include <Logging.h>

#include "SameOrigin.h"

namespace {
constexpr const char* kTag = "BODL";

struct ProgressBridge {
  BookOrbitStreamDownloader::ProgressFn fn = nullptr;
  void* ctx = nullptr;
};

}  // namespace

void BookOrbitStreamDownloader::configure(freeink::SecureHttpClient& http, const std::string& url, const char* accept) {
  // The spec's second correction: certificates are verified against a
  // user-supplied PEM root. setInsecure() is never called here.
  http.setCACert(caCertPem_);
  http.setFollowRedirects(bookorbit::kMaxRedirectHops);
  http.setAllowRedirectDowngrade(false);
  http.setUserAgent("CrossInk-BookOrbit/1.0");
  http.begin(url);
  http.addHeader("accept", accept);
  http.addHeader("x-auth-user", username_);
  http.addHeader("x-auth-key", userkey_);
}

bookorbit::Error BookOrbitStreamDownloader::downloadTo(const std::string& url, bookorbit::PartFileWriter& writer,
                                                       const size_t expectedBytes, const ProgressFn progress,
                                                       void* progressCtx) {
  if (bookorbit::originOf(url).empty()) {
    LOG_ERR(kTag, "Refusing a non-absolute download URL");
    return {bookorbit::Status::ClientError, 0};
  }

  freeink::SecureHttpClient http;
  configure(http, url, "application/octet-stream");

  ProgressBridge bridge{progress, progressCtx};
  if (progress != nullptr) {
    http.setProgressCallback([&bridge, expectedBytes](const size_t downloaded, const size_t total) {
      return bridge.fn(bridge.ctx, downloaded, total > 0 ? total : expectedBytes);
    });
  }

  if (!writer.begin()) {
    http.end();
    return {bookorbit::Status::ClientError, 0};
  }

  bool aborted = false;
  const int status = http.GET(
      [&writer, &aborted](const uint8_t* data, const size_t len) {
        if (!writer.onData(data, len)) {
          aborted = true;
          return false;
        }
        return true;
      },
      [&aborted] { return aborted; });
  http.end();

  if (aborted) {
    writer.abandon();
    // The cap is the only abort this layer raises on its own; a cancel comes
    // through the progress callback and is reported the same way.
    // "Aborted" alone hides whether the cap tripped or the card refused the
    // write, which are different problems with different fixes.
    LOG_ERR(kTag, "Download aborted after %u bytes (%s)", static_cast<unsigned>(writer.bytes()),
            writer.capExceeded() ? "size cap exceeded" : "write failed");
    return {bookorbit::Status::ClientError, status};
  }
  if (status < 0) {
    writer.abandon();
    return {bookorbit::Status::Transport, 0};
  }
  if (status < 200 || status >= 300) {
    // A 3xx reaching here means following stopped: either the hop budget ran
    // out or SecureHttpClient refused a downgrade.
    writer.abandon();
    return bookorbit::classify(status, false);
  }
  return {bookorbit::Status::Ok, status};
}

bookorbit::Error BookOrbitStreamDownloader::downloadThumbnail(const std::string& url, bookorbit::ThumbnailGate& gate) {
  freeink::SecureHttpClient http;
  configure(http, url, bookorbit::kThumbnailAccept);

  bool aborted = false;
  bool headersChecked = false;
  const int status = http.GET(
      [&gate, &http, &aborted, &headersChecked](const uint8_t* data, const size_t len) {
        if (!headersChecked) {
          headersChecked = true;
          // Checked on the first chunk, which is the first point the response
          // headers are available; the gate has opened no file before this.
          if (!gate.onHeaders(http.getHeader("content-type"))) {
            aborted = true;
            return false;
          }
        }
        if (!gate.onData(data, len)) {
          aborted = true;
          return false;
        }
        return true;
      },
      [&aborted] { return aborted; });
  http.end();

  if (aborted) {
    LOG_ERR(kTag, "Thumbnail refused or capped");
    return {bookorbit::Status::ClientError, status};
  }
  if (status < 0) return {bookorbit::Status::Transport, 0};
  return bookorbit::classify(status, false);
}
