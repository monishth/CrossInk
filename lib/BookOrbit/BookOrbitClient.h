#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "BookOrbitError.h"
#include "IHttpTransport.h"

namespace bookorbit {

// Stays under the server's 1 MiB body limit, as bookorbit_api.lua does.
inline constexpr size_t kMaxBodyBytes = 900 * 1024;

struct DeviceIdentity {
  std::string deviceId;
  std::string deviceModel;
  std::string pluginVersion;
};

// Assembles authenticated BookOrbit requests. Knows nothing about TLS or
// sockets — that is the injected IHttpTransport's job.
class BookOrbitClient {
 public:
  BookOrbitClient(IHttpTransport& transport, std::string baseUrl, std::string username, std::string userkey,
                  DeviceIdentity identity);

  Error get(std::string_view path, std::string& outBody);
  Error postJson(std::string_view path, std::string_view json, std::string& outBody);
  Error putJson(std::string_view path, std::string_view json, std::string& outBody);

  // Merges deviceId/deviceModel/pluginVersion/deviceTime into a JSON object
  // body, as bookorbit_api.lua's withDevice() does. Every /koreader/plugin/*
  // POST carries these.
  // Convenience over withDevice() using this client's own identity and a
  // unix timestamp rendered as the server's local "%Y-%m-%d %H:%M:%S". Every
  // /koreader/plugin/* POST body must go through this.
  std::string withDeviceFields(std::string_view bodyJson, uint32_t nowUnix) const;

  static std::string withDevice(std::string_view bodyJson, const DeviceIdentity& identity, std::string_view deviceTime);

 private:
  Error send(std::string_view method, std::string_view path, std::string_view json, std::string& outBody);

  IHttpTransport& transport;
  std::string baseUrl;
  std::string username;
  std::string userkey;
  DeviceIdentity identity;
};

}  // namespace bookorbit
