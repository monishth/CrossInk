#include "BookOrbitCredentialStore.h"

#include <Logging.h>
#include <MD5Builder.h>
#include <ObfuscationUtils.h>
#include <WiFi.h>

#include "BookOrbitUrl.h"

namespace {
constexpr char kModule[] = "BORB";
constexpr uint8_t CONFIG_VERSION = 1;
}  // namespace

void BookOrbitCredentialStore::toJson(JsonDocument& doc) const {
  doc["cfgVersion"] = CONFIG_VERSION;
  // Serialize members directly rather than through getters: the getters
  // lazy-load, and saveToFile() already holds the store mutex.
  doc["username"] = username;
  doc["password_obf"] = obfuscation::obfuscateToBase64(password);
  doc["serverUrl"] = serverUrl;
  doc["rootCaPem"] = rootCaPem;
  doc["deviceId"] = deviceId;
  doc["enabled"] = enabled;
}

bool BookOrbitCredentialStore::fromJson(JsonVariantConst doc) {
  username = doc["username"] | "";

  obfuscation::DecodeStatus status = obfuscation::DecodeStatus::INVALID;
  password = obfuscation::deobfuscateFromBase64(doc["password_obf"] | "", &status);
  if (status == obfuscation::DecodeStatus::INVALID) {
    password.clear();
    LOG_ERR(kModule, "ignoring unreadable BookOrbit password");
  }

  serverUrl = doc["serverUrl"] | "";
  rootCaPem = doc["rootCaPem"] | "";
  deviceId = doc["deviceId"] | "";
  enabled = doc["enabled"] | false;
  return true;
}

void BookOrbitCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  ensureLoaded();
  username = user;
  password = pass;
}

const std::string& BookOrbitCredentialStore::getUsername() const {
  ensureLoaded();
  return username;
}

const std::string& BookOrbitCredentialStore::getPassword() const {
  ensureLoaded();
  return password;
}

std::string BookOrbitCredentialStore::getMd5Password() const {
  ensureLoaded();
  if (password.empty()) return "";

  MD5Builder md5;
  md5.begin();
  md5.add(password.c_str());
  md5.calculate();
  return md5.toString().c_str();
}

void BookOrbitCredentialStore::setServerUrl(const std::string& url) {
  ensureLoaded();
  // Store the normalized form so every caller sees a base ending in /api/v1.
  serverUrl = bookorbit::normalizeServerUrl(url);
}

const std::string& BookOrbitCredentialStore::getServerUrl() const {
  ensureLoaded();
  return serverUrl;
}

void BookOrbitCredentialStore::setRootCaPem(const std::string& pem) {
  ensureLoaded();
  rootCaPem = pem;
}

const std::string& BookOrbitCredentialStore::getRootCaPem() const {
  ensureLoaded();
  return rootCaPem;
}

void BookOrbitCredentialStore::setEnabled(const bool value) {
  ensureLoaded();
  enabled = value;
}

bool BookOrbitCredentialStore::isEnabled() const {
  ensureLoaded();
  return enabled;
}

const std::string& BookOrbitCredentialStore::getDeviceId() {
  ensureLoaded();
  if (!deviceId.empty()) return deviceId;

  // Generated exactly once. The MAC is stable across reboots and reflashes,
  // which is the property that matters: the server's page-stat dedup key
  // includes deviceId, so a device that ever changes its id re-inserts its
  // whole history as new rows and doubles the recorded reading time.
  uint8_t mac[6] = {0};
  WiFi.macAddress(mac);

  char buffer[32];
  snprintf(buffer, sizeof(buffer), "crossink-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  deviceId = buffer;
  // Unlike the plain setters (whose callers persist explicitly), this must hit
  // the card immediately: a reboot before the next save would mint a different
  // id and duplicate the device's whole stats history server-side.
  if (!saveToFile()) {
    LOG_ERR(kModule, "failed to persist generated device id");
  }
  LOG_INF(kModule, "generated device id %s", deviceId.c_str());
  return deviceId;
}

bool BookOrbitCredentialStore::hasCredentials() const {
  ensureLoaded();
  return !username.empty() && !password.empty() && !serverUrl.empty();
}
