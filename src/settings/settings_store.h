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
  String        wifiSsid() const;
  String        wifiPass() const;

  // Favourites — symbols the user pinned to the top of the list via a
  // long-press on the row. Stored as an upper-case CSV in
  // /settings.json under "favourites". Order is preserved (first-pinned
  // sorts first within the favourites block).
  std::vector<String> favourites() const;
  bool isFavourite(const String& symbol) const;
  // Adds the symbol to favourites if absent, removes it if present.
  // Returns the new state (true = is now favourite). Persists to LittleFS.
  bool toggleFavourite(const String& symbol);

  // Shares owned per symbol ("holdings") — drives the portfolio total +
  // day P/L readout in the status bar. Keyed by upper-case symbol and
  // persisted in /settings.json under "shares" as a "SYM=QTY,..." CSV.
  // Returns 0 for symbols with no holding.
  float shares(const String& symbol) const;
  // qty <= 0 removes the entry. Persists to LittleFS.
  void  setShares(const String& symbol, float qty);

  // Atomic update from the web form. Persists to LittleFS.
  void update(const String& apiKey,
              uint32_t refreshSeconds,
              const String& symbolsCsv);

  // Captive portal saves the user's home WiFi here.
  void setWifi(const String& ssid, const String& pass);

 private:
  void load(const char* seedKey, const char* seedWifiSsid, const char* seedWifiPass);
  void save() const;

  mutable SemaphoreHandle_t _mu = nullptr;
  String   _apiKey;
  uint32_t _refresh = 60;
  String   _symbolsCsv;
  String   _favouritesCsv;
  String   _sharesCsv;   // "AAPL=10,MSFT=2.5" — see shares()/setShares()
  String   _wifiSsid;
  String   _wifiPass;
};
