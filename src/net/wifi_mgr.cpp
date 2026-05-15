#include "wifi_mgr.h"

#include <WiFi.h>

#include "../settings/settings_store.h"

namespace wifi_mgr {

namespace {

String g_ap_ssid;
String g_ap_pass;
bool   g_ap_active = false;

// CYD-Setup-AB12 — last two MAC bytes give a stable, board-unique suffix.
String makeApSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "CYD-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

bool tryStaConnect(const String& ssid, const String& pass) {
  if (!ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  return WiFi.status() == WL_CONNECTED;
}

void startAp() {
  g_ap_ssid = makeApSsid();
  // Open AP. WPA on the setup AP would force the user to type a password
  // *before* they could open the captive portal to type their real one —
  // worse UX than a transient open AP for setup only.
  g_ap_pass = "";
  WiFi.mode(WIFI_AP);
  WiFi.softAP(g_ap_ssid.c_str(), g_ap_pass.length() ? g_ap_pass.c_str() : nullptr);
  g_ap_active = true;
}

}  // namespace

void begin(SettingsStore& settings) {
  if (tryStaConnect(settings.wifiSsid(), settings.wifiPass())) return;
  startAp();
}

void retrySta(SettingsStore& settings) {
  WiFi.softAPdisconnect(true);
  g_ap_active = false;
  if (!tryStaConnect(settings.wifiSsid(), settings.wifiPass())) {
    // Fall back to AP again so the user can retry.
    startAp();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
bool apActive()  { return g_ap_active && !connected(); }

String ip() {
  if (connected()) return WiFi.localIP().toString();
  if (apActive())  return WiFi.softAPIP().toString();
  return String("0.0.0.0");
}

String apSsid() { return apActive() ? g_ap_ssid : String(); }
String apPass() { return apActive() ? g_ap_pass : String(); }

}  // namespace wifi_mgr
