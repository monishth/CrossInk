#include "SimulatorHttpTransport.h"

#ifdef SIMULATOR

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr long kTimeoutSeconds = 20;

size_t appendBody(char* data, const size_t size, const size_t nmemb, void* userp) {
  const size_t total = size * nmemb;
  static_cast<std::string*>(userp)->append(data, total);
  return total;
}

bool isHttps(const std::string& url) { return url.rfind("https://", 0) == 0; }

// A PEM has to reach libcurl as a file. The simulator already writes under
// ./fs_, so the certificate lands beside everything else it persists.
std::string writeTempCaFile(const std::string& pem) {
  const std::string path = "./fs_/.crosspoint/bookorbit_ca.pem";
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) return {};
  const size_t written = std::fwrite(pem.data(), 1, pem.size(), file);
  std::fclose(file);
  return written == pem.size() ? path : std::string{};
}

}  // namespace

bookorbit::HttpResponse SimulatorHttpTransport::send(const bookorbit::HttpRequest& request) {
  // Same fail-closed rule as the device transport: never send credentials over
  // an unverifiable TLS connection.
  if (isHttps(request.url) && rootCaPem_.empty()) {
    std::fprintf(stderr, "[SIM][BORB] refusing https request with no root certificate\n");
    return {0, true, ""};
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) return {0, true, ""};

  std::string body;
  struct curl_slist* headers = nullptr;
  for (const auto& header : request.headers) {
    headers = curl_slist_append(headers, (header.first + ": " + header.second).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  // Redirects are handled by the caller on device; keep the shapes identical.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

  std::string caPath;
  if (!rootCaPem_.empty()) {
    caPath = writeTempCaFile(rootCaPem_);
    if (!caPath.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  }

  if (request.method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
  } else if (request.method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    if (!request.body.empty()) {
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }
  }

  const CURLcode code = curl_easy_perform(curl);

  bookorbit::HttpResponse response;
  if (code != CURLE_OK) {
    // Transport failure: no HTTP status exists, and the error taxonomy treats
    // this as abort-the-whole-sync, exactly as on device.
    std::fprintf(stderr, "[SIM][BORB] %s %s transport error: %s\n", request.method.c_str(), request.url.c_str(),
                 curl_easy_strerror(code));
    response.transportFailed = true;
  } else {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    response.body = std::move(body);
    std::fprintf(stderr, "[SIM][BORB] %s %s -> %ld (%u bytes)\n", request.method.c_str(), request.url.c_str(), status,
                 static_cast<unsigned>(response.body.size()));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

#endif  // SIMULATOR
