#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

class SettingsStore {
 public:
  // seedWifiSsid/seedWifiPass are written to LittleFS only on first boot
  // (or when /settings.json is absent). After that the captive portal /
  // web admin owns these.
  void begin(const char* seedKey,
             const char* seedWifiSsid = nullptr,
             const char* seedWifiPass = nullptr);

  String        apiKey() const;
  uint32_t      refreshSeconds() const;
  std::vector<String> symbols() const;
  String        adminUser() const;
  String        adminPass() const;
  String        wifiSsid() const;
  String        wifiPass() const;

  // Atomic update from the web form. Persists to LittleFS.
  void update(const String& apiKey,
              uint32_t refreshSeconds,
              const String& symbolsCsv,
              const String& adminUser,
              const String& adminPass);

  // Captive portal saves the user's home WiFi here.
  void setWifi(const String& ssid, const String& pass);

 private:
  void load(const char* seedKey, const char* seedWifiSsid, const char* seedWifiPass);
  void save() const;

  mutable SemaphoreHandle_t _mu = nullptr;
  String   _apiKey;
  uint32_t _refresh = 60;
  String   _symbolsCsv;
  String   _adminUser;
  String   _adminPass;
  String   _wifiSsid;
  String   _wifiPass;
};
