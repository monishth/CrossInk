#include "SimulatorStreamDownloader.h"

#ifdef SIMULATOR

#include <curl/curl.h>

#include <cstdio>
#include <string>

namespace {

constexpr long kTimeoutSeconds = 120;  // a book can be several MB

struct SinkContext {
  bookorbit::PartFileWriter* writer = nullptr;
  SimulatorStreamDownloader::ProgressFn progress = nullptr;
  void* progressCtx = nullptr;
  size_t expected = 0;
  size_t received = 0;
  bool aborted = false;
};

size_t onChunk(char* data, const size_t size, const size_t nmemb, void* userp) {
  auto* ctx = static_cast<SinkContext*>(userp);
  const size_t total = size * nmemb;

  // Straight from the socket into the .part file; nothing is buffered whole.
  if (!ctx->writer->onData(reinterpret_cast<const uint8_t*>(data), total)) {
    ctx->aborted = true;
    return 0;  // a short write tells libcurl to abort the transfer
  }

  ctx->received += total;
  if (ctx->progress != nullptr && !ctx->progress(ctx->progressCtx, ctx->received, ctx->expected)) {
    ctx->aborted = true;
    return 0;
  }
  return total;
}

bool isHttps(const std::string& url) { return url.rfind("https://", 0) == 0; }

}  // namespace

bookorbit::Error SimulatorStreamDownloader::downloadTo(const std::string& url, bookorbit::PartFileWriter& writer,
                                                       const size_t expectedBytes, const ProgressFn progress,
                                                       void* progressCtx) {
  // Same fail-closed rule as everywhere else: no certificate, no https.
  const bool haveCa = caCertPem_ != nullptr && caCertPem_[0] != '\0';
  if (isHttps(url) && !haveCa) {
    std::fprintf(stderr, "[SIM][BORB] refusing https download with no root certificate\n");
    return {bookorbit::Status::Transport, 0};
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return {bookorbit::Status::Transport, 0};

  SinkContext ctx;
  ctx.writer = &writer;
  ctx.progress = progress;
  ctx.progressCtx = progressCtx;
  ctx.expected = expectedBytes;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "accept: application/octet-stream");
  headers = curl_slist_append(headers, ("x-auth-user: " + username_).c_str());
  headers = curl_slist_append(headers, ("x-auth-key: " + userkey_).c_str());

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onChunk);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // The device caps redirects at 5 and refuses cross-origin hops because the
  // auth headers travel with the request; libcurl enforces the same shape.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, haveCa ? "https" : "http,https");

  if (haveCa) {
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  }

  const CURLcode code = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  std::fprintf(stderr, "[SIM][BORB] GET %s -> %ld (%u bytes%s)\n", url.c_str(), status,
               static_cast<unsigned>(ctx.received), ctx.aborted ? ", aborted" : "");

  if (ctx.aborted) return {bookorbit::Status::Transport, static_cast<int>(status)};
  if (code != CURLE_OK) {
    std::fprintf(stderr, "[SIM][BORB] download transport error: %s\n", curl_easy_strerror(code));
    return {bookorbit::Status::Transport, static_cast<int>(status)};
  }
  return bookorbit::classify(static_cast<int>(status), false);
}

#endif  // SIMULATOR
