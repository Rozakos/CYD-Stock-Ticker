#include "settings_store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "../config.h"
#include "../display/fs_littlefs.h"

namespace {

std::vector<String> split_csv(const String& s) {
  std::vector<String> out;
  int start = 0;
  while (start < (int)s.length()) {
    int comma = s.indexOf(',', start);
    if (comma < 0) comma = s.length();
    String tok = s.substring(start, comma);
    tok.trim();
    tok.toUpperCase();
    if (tok.length()) out.push_back(tok);
    start = comma + 1;
  }
  return out;
}

}  // namespace

void SettingsStore::begin(const char* seedKey,
                          const char* seedWifiSsid,
                          const char* seedWifiPass) {
  _mu = xSemaphoreCreateMutex();
  load(seedKey, seedWifiSsid, seedWifiPass);
}

void SettingsStore::load(const char* seedKey,
                         const char* seedWifiSsid,
                         const char* seedWifiPass) {
  bool needSave = false;
  auto defaults = [&] {
    _apiKey     = seedKey ? seedKey : "";
    _refresh    = cfg::DEFAULT_REFRESH_SECONDS;
    _symbolsCsv = cfg::DEFAULT_SYMBOLS;
    _wifiSsid   = seedWifiSsid ? seedWifiSsid : "";
    _wifiPass   = seedWifiPass ? seedWifiPass : "";
  };

  fs_littlefs::Guard g;
  if (!LittleFS.exists(cfg::SETTINGS_PATH)) {
    defaults();
    needSave = true;
  } else {
    File f = LittleFS.open(cfg::SETTINGS_PATH, "r");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      defaults();
      needSave = true;
    } else {
      _apiKey        = doc["api_key"]     | (seedKey ? seedKey : "");
      _refresh       = doc["refresh_s"]   | cfg::DEFAULT_REFRESH_SECONDS;
      _symbolsCsv    = doc["symbols"]     | cfg::DEFAULT_SYMBOLS;
      _favouritesCsv = doc["favourites"]  | "";
      _sharesCsv     = doc["shares"]      | "";
      _wifiSsid      = doc["wifi_ssid"]   | (seedWifiSsid ? seedWifiSsid : "");
      _wifiPass      = doc["wifi_pass"]   | (seedWifiPass ? seedWifiPass : "");
    }
  }

  if (_refresh < cfg::MIN_REFRESH_SECONDS) _refresh = cfg::MIN_REFRESH_SECONDS;
  if (needSave) save();
}

void SettingsStore::save() const {
  JsonDocument doc;
  doc["api_key"]    = _apiKey;
  doc["refresh_s"]  = _refresh;
  doc["symbols"]    = _symbolsCsv;
  doc["favourites"] = _favouritesCsv;
  doc["shares"]     = _sharesCsv;
  doc["wifi_ssid"]  = _wifiSsid;
  doc["wifi_pass"]  = _wifiPass;

  fs_littlefs::Guard g;
  File f = LittleFS.open(cfg::SETTINGS_PATH, "w");
  serializeJson(doc, f);
  f.close();
}

String SettingsStore::apiKey() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String v = _apiKey;
  xSemaphoreGive(_mu);
  return v;
}

uint32_t SettingsStore::refreshSeconds() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  uint32_t v = _refresh;
  xSemaphoreGive(_mu);
  return v;
}

std::vector<String> SettingsStore::symbols() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String csv = _symbolsCsv;
  xSemaphoreGive(_mu);
  return split_csv(csv);
}

std::vector<String> SettingsStore::favourites() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String csv = _favouritesCsv;
  xSemaphoreGive(_mu);
  return split_csv(csv);
}

bool SettingsStore::isFavourite(const String& symbol) const {
  String upper = symbol;
  upper.trim();
  upper.toUpperCase();
  if (!upper.length()) return false;
  for (const auto& f : favourites()) {
    if (f == upper) return true;
  }
  return false;
}

bool SettingsStore::toggleFavourite(const String& symbol) {
  String upper = symbol;
  upper.trim();
  upper.toUpperCase();
  if (!upper.length()) return false;

  xSemaphoreTake(_mu, portMAX_DELAY);
  std::vector<String> favs = split_csv(_favouritesCsv);
  bool was = false;
  for (auto it = favs.begin(); it != favs.end(); ++it) {
    if (*it == upper) { was = true; favs.erase(it); break; }
  }
  if (!was) favs.push_back(upper);

  String csv;
  for (size_t i = 0; i < favs.size(); ++i) {
    if (i) csv += ',';
    csv += favs[i];
  }
  _favouritesCsv = csv;
  save();
  xSemaphoreGive(_mu);
  return !was;
}

float SettingsStore::shares(const String& symbol) const {
  String upper = symbol;
  upper.trim();
  upper.toUpperCase();
  if (!upper.length()) return 0.0f;

  xSemaphoreTake(_mu, portMAX_DELAY);
  String csv = _sharesCsv;
  xSemaphoreGive(_mu);

  for (const auto& tok : split_csv(csv)) {
    int eq = tok.indexOf('=');
    if (eq <= 0) continue;
    if (tok.substring(0, eq) == upper) {
      float qty = tok.substring(eq + 1).toFloat();
      return qty > 0.0f ? qty : 0.0f;
    }
  }
  return 0.0f;
}

void SettingsStore::setShares(const String& symbol, float qty) {
  String upper = symbol;
  upper.trim();
  upper.toUpperCase();
  if (!upper.length()) return;

  xSemaphoreTake(_mu, portMAX_DELAY);
  String csv;
  for (const auto& tok : split_csv(_sharesCsv)) {
    int eq = tok.indexOf('=');
    if (eq <= 0 || tok.substring(0, eq) == upper) continue;  // drop old entry
    if (csv.length()) csv += ',';
    csv += tok;
  }
  if (qty > 0.0f) {
    if (csv.length()) csv += ',';
    // Two decimals covers fractional shares without dragging float noise
    // into the CSV.
    csv += upper + "=" + String(qty, 2);
  }
  _sharesCsv = csv;
  save();
  xSemaphoreGive(_mu);
}

String SettingsStore::wifiSsid() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String v = _wifiSsid;
  xSemaphoreGive(_mu);
  return v;
}

String SettingsStore::wifiPass() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String v = _wifiPass;
  xSemaphoreGive(_mu);
  return v;
}

void SettingsStore::setWifi(const String& ssid, const String& pass) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _wifiSsid = ssid;
  _wifiPass = pass;
  save();
  xSemaphoreGive(_mu);
}

void SettingsStore::update(const String& apiKey,
                           uint32_t refreshSeconds,
                           const String& symbolsCsv) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _apiKey = apiKey;
  _refresh   = refreshSeconds < cfg::MIN_REFRESH_SECONDS
                   ? cfg::MIN_REFRESH_SECONDS
                   : refreshSeconds;
  _symbolsCsv = symbolsCsv;
  save();
  xSemaphoreGive(_mu);
}
