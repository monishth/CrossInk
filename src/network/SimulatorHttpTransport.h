#pragma once

#ifdef SIMULATOR

#include <string>

#include "IHttpTransport.h"

/**
 * IHttpTransport over libcurl, for the desktop simulator.
 *
 * The simulator's bundled SecureHttpClient is a thin stub with no streaming
 * GET, no sendRequest() and no response-header access, so the wolfSSL-backed
 * BookOrbitHttpTransport cannot compile against it. But the simulator runs on a
 * machine with a perfectly good network stack — the interface exists precisely
 * so a different implementation can be swapped in, and this is that.
 *
 * Security behaviour deliberately matches the device transport: an https URL
 * with no configured root certificate is refused rather than silently
 * downgraded, so the simulator cannot teach a laxer habit than the hardware.
 * Plain http is permitted, which is what makes a local server usable.
 */
class SimulatorHttpTransport final : public bookorbit::IHttpTransport {
 public:
  explicit SimulatorHttpTransport(std::string rootCaPem) : rootCaPem_(std::move(rootCaPem)) {}

  bookorbit::HttpResponse send(const bookorbit::HttpRequest& request) override;

 private:
  std::string rootCaPem_;
};

#endif  // SIMULATOR
