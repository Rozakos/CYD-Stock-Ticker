#pragma once

#include <Arduino.h>

class SettingsStore;

namespace wifi_mgr {

// If settings has a saved SSID, attempts STA connect (up to ~15s).
// On failure or when no SSID is configured, starts an open AP named
// CYD-Setup-XXXX (last two MAC bytes) so the captive portal can run.
void begin(SettingsStore& settings);

// Try the configured creds again from STA mode. Called after the captive
// portal saves new credentials. Non-blocking; poll `connected()`.
void retrySta(SettingsStore& settings);

bool   connected();   // STA WL_CONNECTED
bool   apActive();    // soft-AP mode is up
String ip();          // STA IP if connected, else AP IP
String apSsid();      // the SoftAP SSID currently advertised (empty if AP off)
String apPass();      // the SoftAP password (empty for open networks)

}  // namespace wifi_mgr
