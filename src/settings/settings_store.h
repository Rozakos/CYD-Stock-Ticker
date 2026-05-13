#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <vector>

class SettingsStore {
 public:
  void begin(const char* seedKey);

  String        apiKey() const;
  uint32_t      refreshSeconds() const;
  std::vector<String> symbols() const;
  String        adminUser() const;
  String        adminPass() const;

  // Atomic update from the web form. Persists to LittleFS.
  void update(const String& apiKey,
              uint32_t refreshSeconds,
              const String& symbolsCsv,
              const String& adminUser,
              const String& adminPass);

 private:
  void load(const char* seedKey);
  void save() const;

  mutable SemaphoreHandle_t _mu = nullptr;
  String   _apiKey;
  uint32_t _refresh = 60;
  String   _symbolsCsv;
  String   _adminUser;
  String   _adminPass;
};
