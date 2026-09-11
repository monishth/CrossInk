#include "BookOrbitClient.h"

#include <ctime>
#include <utility>

#include "BookOrbitUrl.h"

namespace bookorbit {
namespace {

// Appends "key":"value" to an object body that is known to be valid JSON.
void appendField(std::string& target, const std::string_view key, const std::string_view value, const bool needsComma) {
  if (needsComma) target += ',';
  target += '"';
  target.append(key);
  target += "\":\"";
  target.append(value);
  target += '"';
}

}  // namespace

BookOrbitClient::BookOrbitClient(IHttpTransport& httpTransport, std::string base, std::string user, std::string key,
                                 DeviceIdentity device)
    : transport(httpTransport),
      baseUrl(std::move(base)),
      username(std::move(user)),
      userkey(std::move(key)),
      identity(std::move(device)) {}

Error BookOrbitClient::get(const std::string_view path, std::string& outBody) { return send("GET", path, {}, outBody); }

Error BookOrbitClient::postJson(const std::string_view path, const std::string_view json, std::string& outBody) {
  return send("POST", path, json, outBody);
}

Error BookOrbitClient::putJson(const std::string_view path, const std::string_view json, std::string& outBody) {
  return send("PUT", path, json, outBody);
}

Error BookOrbitClient::send(const std::string_view method, const std::string_view path, const std::string_view json,
                            std::string& outBody) {
  if (json.size() > kMaxBodyBytes) {
    return {Status::BodyTooLarge, 0};
  }

  HttpRequest request;
  request.method = std::string(method);
  request.url = joinPath(baseUrl, path);
  request.headers.emplace_back("accept", "application/json");
  request.headers.emplace_back("x-auth-user", username);
  request.headers.emplace_back("x-auth-key", userkey);
  if (!json.empty()) {
    request.headers.emplace_back("content-type", "application/json");
    request.body = std::string(json);
  }

  const HttpResponse response = transport.send(request);
  outBody = response.body;
  return classify(response.status, response.transportFailed);
}

std::string BookOrbitClient::withDeviceFields(const std::string_view bodyJson, const uint32_t nowUnix) const {
  char stamp[20] = {0};
  const std::time_t when = static_cast<std::time_t>(nowUnix);
  std::tm parts{};
#if defined(_WIN32)
  localtime_s(&parts, &when);
#else
  localtime_r(&when, &parts);
#endif
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &parts);
  return withDevice(bodyJson, identity, stamp);
}

std::string BookOrbitClient::withDevice(const std::string_view bodyJson, const DeviceIdentity& identity,
                                        const std::string_view deviceTime) {
  // bodyJson is always an object produced by our own encoders.
  std::string merged(bodyJson);
  if (merged.size() < 2 || merged.front() != '{' || merged.back() != '}') {
    return merged;
  }

  const bool hadFields = merged.size() > 2;
  merged.pop_back();  // drop the closing brace
  appendField(merged, "deviceId", identity.deviceId, hadFields);
  appendField(merged, "deviceModel", identity.deviceModel, true);
  appendField(merged, "pluginVersion", identity.pluginVersion, true);
  appendField(merged, "deviceTime", deviceTime, true);
  merged += '}';
  return merged;
}

}  // namespace bookorbit
