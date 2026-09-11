#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

/**
 * BookOrbit server credentials, base URL and root certificate.
 *
 * Follows KOReaderCredentialStore exactly: the password is XOR-obfuscated with
 * the device MAC and base64-encoded before it reaches the card. That is not
 * encryption — it keeps credentials from being readable at a glance and ties
 * them to this device.
 *
 * Kept separate from the KOReader store rather than extending it, so a user can
 * run CrossPoint sync and BookOrbit against different servers, and so upstream
 * changes to the KOReader store never conflict here.
 */
class BookOrbitCredentialStore : public PersistableStore<BookOrbitCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;  // normalized to end in /api/v1 on write
  std::string rootCaPem;  // single PEM root; empty disables BookOrbit entirely
  std::string deviceId;   // generated once, then never regenerated — see below
  bool enabled = false;

  BookOrbitCredentialStore() = default;
  ~BookOrbitCredentialStore() = default;

  friend class PersistableStore<BookOrbitCredentialStore>;

 public:
  static const char* getFilePath() { return "/.crosspoint/bookorbit.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const;
  const std::string& getPassword() const;
  // Lowercase hex md5(password) — the x-auth-key every request carries.
  std::string getMd5Password() const;

  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const;

  void setRootCaPem(const std::string& pem);
  const std::string& getRootCaPem() const;

  void setEnabled(bool value);
  bool isEnabled() const;

  /**
   * Stable per-device identifier.
   *
   * The server deduplicates page-stat events on
   * (userId, bookFileId, deviceId, page, startTime). A device that regenerates
   * its id re-inserts its entire reading history as new rows and silently
   * doubles the recorded reading time, so this is generated once from the MAC
   * and persisted; it must never be regenerated on reboot or reflash.
   */
  const std::string& getDeviceId();

  bool hasCredentials() const;
};

#define BOOKORBIT_STORE BookOrbitCredentialStore::getInstance()
