#pragma once

#include <string>

#include "IHttpTransport.h"

/**
 * IHttpTransport over the SDK's wolfSSL-backed SecureHttpClient.
 *
 * Certificate verification is mandatory. `setInsecure()` is never called, and a
 * request is refused outright when no root PEM has been configured — see the
 * note on fail-closed behaviour in the .cpp, which is not something the SDK
 * does for us.
 */
class BookOrbitHttpTransport final : public bookorbit::IHttpTransport {
 public:
  explicit BookOrbitHttpTransport(std::string rootCaPem) : rootCaPem_(std::move(rootCaPem)) {}

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override;

  void setRootCaPem(std::string pem) { rootCaPem_ = std::move(pem); }
  bool hasRootCa() const { return !rootCaPem_.empty(); }

 private:
  std::string rootCaPem_;
};
