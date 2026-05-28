#include "wifi_mgr.h"

#include <WiFi.h>

#include "../settings/settings_store.h"

namespace wifi_mgr {

namespace {

String g_ap_ssid;
String g_ap_pass;
bool   g_ap_active = false;

// STA join progress. int-sized; written by the net task, read by the UI task
// (same loose cross-task pattern as connected()/ip()).
volatile StaStatus g_sta_status = StaStatus::Idle;
volatile int       g_last_fail  = 0;   // WiFi.status() captured at last failure

// CYD-Setup-AB12 — last two MAC bytes give a stable, board-unique suffix.
String makeApSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[24];
  snprintf(buf, sizeof(buf), "CYD-Setup-%02X%02X", mac[4], mac[5]);
  return String(buf);
}

bool tryStaConnect(const String& ssid, const String& pass) {
  if (!ssid.length()) {
    log_i("[wifi] no STA ssid configured");
    return false;
  }
  log_i("[wifi] trying STA ssid='%s' pass_len=%u",
        ssid.c_str(), (unsigned)pass.length());
  g_sta_status = StaStatus::Connecting;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
    g_sta_status = StaStatus::Connected;
    log_i("[wifi] STA connected ip=%s rssi=%d",
          WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    g_last_fail  = (int)WiFi.status();
    g_sta_status = StaStatus::Failed;
    log_w("[wifi] STA connect failed status=%d", (int)WiFi.status());
  }
  return ok;
}

void startAp() {
  g_ap_ssid = makeApSsid();
  // Open AP. WPA on the setup AP would force the user to type a password
  // *before* they could open the captive portal to type their real one —
  // worse UX than a transient open AP for setup only.
  g_ap_pass = "";
  WiFi.mode(WIFI_AP);
  log_i("[wifi] starting setup AP ssid='%s'", g_ap_ssid.c_str());
  bool ok = WiFi.softAP(g_ap_ssid.c_str(), g_ap_pass.length() ? g_ap_pass.c_str() : nullptr);
  g_ap_active = true;
  log_i("[wifi] setup AP %s ip=%s",
        ok ? "started" : "failed", WiFi.softAPIP().toString().c_str());
}

}  // namespace

void begin(SettingsStore& settings) {
  if (tryStaConnect(settings.wifiSsid(), settings.wifiPass())) return;
  startAp();
}

void retrySta(SettingsStore& settings) {
  log_i("[wifi] retry STA requested; stopping setup AP");
  WiFi.softAPdisconnect(true);
  g_ap_active = false;
  if (!tryStaConnect(settings.wifiSsid(), settings.wifiPass())) {
    // Fall back to AP again so the user can retry.
    startAp();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
bool apActive()  { return g_ap_active && !connected(); }

StaStatus staStatus() { return g_sta_status; }

const char* lastFailMessage() {
  switch (g_last_fail) {
    case WL_NO_SSID_AVAIL:   return "Network not found";
    case WL_CONNECT_FAILED:  return "Wrong password";
    case WL_CONNECTION_LOST: return "Connection lost";
    case WL_DISCONNECTED:    return "Wrong password or weak signal";
    default:                 return "Couldn't connect";
  }
}

String ip() {
  if (connected()) return WiFi.localIP().toString();
  if (apActive())  return WiFi.softAPIP().toString();
  return String("0.0.0.0");
}

String apSsid() { return apActive() ? g_ap_ssid : String(); }
String apPass() { return apActive() ? g_ap_pass : String(); }

}  // namespace wifi_mgr
