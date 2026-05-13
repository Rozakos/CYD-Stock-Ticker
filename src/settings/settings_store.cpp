#include "settings_store.h"

#include <ArduinoJson.h>
#include <LittleFS.h>

#include "../config.h"

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

void SettingsStore::begin(const char* seedKey) {
  _mu = xSemaphoreCreateMutex();
  load(seedKey);
}

void SettingsStore::load(const char* seedKey) {
  bool needSave = false;

  if (!LittleFS.exists(cfg::SETTINGS_PATH)) {
    _apiKey     = seedKey ? seedKey : "";
    _refresh    = cfg::DEFAULT_REFRESH_SECONDS;
    _symbolsCsv = cfg::DEFAULT_SYMBOLS;
    _adminUser  = cfg::DEFAULT_ADMIN_USER;
    _adminPass  = cfg::DEFAULT_ADMIN_PASS;
    needSave = true;
  } else {
    File f = LittleFS.open(cfg::SETTINGS_PATH, "r");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
      _apiKey     = seedKey ? seedKey : "";
      _refresh    = cfg::DEFAULT_REFRESH_SECONDS;
      _symbolsCsv = cfg::DEFAULT_SYMBOLS;
      _adminUser  = cfg::DEFAULT_ADMIN_USER;
      _adminPass  = cfg::DEFAULT_ADMIN_PASS;
      needSave = true;
    } else {
      _apiKey     = doc["api_key"]     | (seedKey ? seedKey : "");
      _refresh    = doc["refresh_s"]   | cfg::DEFAULT_REFRESH_SECONDS;
      _symbolsCsv = doc["symbols"]     | cfg::DEFAULT_SYMBOLS;
      _adminUser  = doc["admin_user"]  | cfg::DEFAULT_ADMIN_USER;
      _adminPass  = doc["admin_pass"]  | cfg::DEFAULT_ADMIN_PASS;
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
  doc["admin_user"] = _adminUser;
  doc["admin_pass"] = _adminPass;

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

String SettingsStore::adminUser() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String v = _adminUser;
  xSemaphoreGive(_mu);
  return v;
}

String SettingsStore::adminPass() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  String v = _adminPass;
  xSemaphoreGive(_mu);
  return v;
}

void SettingsStore::update(const String& apiKey,
                           uint32_t refreshSeconds,
                           const String& symbolsCsv,
                           const String& adminUser,
                           const String& adminPass) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  if (apiKey.length())    _apiKey     = apiKey;
  _refresh   = refreshSeconds < cfg::MIN_REFRESH_SECONDS
                   ? cfg::MIN_REFRESH_SECONDS
                   : refreshSeconds;
  if (symbolsCsv.length()) _symbolsCsv = symbolsCsv;
  if (adminUser.length())  _adminUser  = adminUser;
  if (adminPass.length())  _adminPass  = adminPass;
  save();
  xSemaphoreGive(_mu);
}
