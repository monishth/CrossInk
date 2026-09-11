#include "BookOrbitHttpTransport.h"

#include <Logging.h>
#include <SecureHttpClient.h>

namespace {

constexpr char kModule[] = "BORB";

// The TLS handshake allocates; on the C3 it fails hard rather than gracefully.
// Same floors KOReaderSyncClient.cpp uses for the same wolfSSL stack.
constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 35000;
constexpr uint32_t MIN_MAX_ALLOC_HEAP_FOR_TLS = 20000;

constexpr uint32_t REQUEST_TIMEOUT_MS = 20000;

bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < MIN_MAX_ALLOC_HEAP_FOR_TLS) {
    LOG_ERR(kModule, "insufficient heap for TLS: %u free (need %u), %u max alloc (need %u)", freeHeap,
            MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, MIN_MAX_ALLOC_HEAP_FOR_TLS);
    return true;
  }
  return false;
}

bool isHttps(const std::string& url) { return url.rfind("https://", 0) == 0; }

}  // namespace

bookorbit::HttpResponse BookOrbitHttpTransport::send(const bookorbit::HttpRequest& request) {
  // Fail closed on an unverifiable connection.
  //
  // SecureClient only calls wolfSSL_CTX_set_verify() when _insecure is set, and
  // only loads a CA when _rootCA is non-null (SecureClient.cpp:85-90). With
  // neither — the default state — no verify mode is ever configured and wolfSSL
  // proceeds *unverified*, silently. So refusing here is not belt-and-braces:
  // it is the only thing standing between a missing PEM and credentials sent
  // over an unauthenticated channel.
  if (isHttps(request.url) && rootCaPem_.empty()) {
    LOG_ERR(kModule, "refusing https request with no root certificate configured");
    return {0, true, ""};
  }

  if (insufficientHeap()) {
    return {0, true, ""};
  }

  freeink::SecureHttpClient http;
  if (!rootCaPem_.empty()) {
    http.setCACert(rootCaPem_.c_str());
  }
  http.setTimeout(REQUEST_TIMEOUT_MS);

  if (!http.begin(request.url)) {
    LOG_ERR(kModule, "begin() failed for %s", request.url.c_str());
    return {0, true, ""};
  }

  for (const auto& header : request.headers) {
    http.addHeader(header.first, header.second);
  }

  int status = 0;
  if (request.method == "GET") {
    status = http.GET();
  } else if (request.method == "POST") {
    status = http.POST(request.body);
  } else {
    status = http.sendRequest(request.method.c_str(), request.body);
  }

  bookorbit::HttpResponse response;
  if (status < 0) {
    // Negative is a transport failure, not an HTTP status. The error taxonomy
    // treats these as abort-the-whole-sync, distinct from a numeric error.
    LOG_ERR(kModule, "%s %s transport error %d", request.method.c_str(), request.url.c_str(), status);
    response.transportFailed = true;
  } else {
    response.status = status;
    response.body = http.getString();
  }

  http.end();
  return response;
}
