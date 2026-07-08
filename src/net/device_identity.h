#pragma once

#include <Arduino.h>
#include <esp_mac.h>

// Stable, board-unique identity strings shared by the HTTP device-info
// endpoint and the mDNS responder. Derived from the STA MAC in efuse, so
// they are valid before (and independent of) any WiFi join — the same
// property ble_provisioning relies on for its CYD-Ticker-XXXX name.
namespace device_identity {

inline void staMac(uint8_t mac[6]) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

inline String macString() {
  uint8_t mac[6] = {0};
  staMac(mac);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Lowercase so it doubles as the mDNS hostname ("cyd-ticker-ab12.local").
// Same last-2-MAC-bytes suffix as the BLE name and setup-AP SSID.
inline String deviceId() {
  uint8_t mac[6] = {0};
  staMac(mac);
  char buf[20];
  snprintf(buf, sizeof(buf), "cyd-ticker-%02x%02x", mac[4], mac[5]);
  return String(buf);
}

}  // namespace device_identity
