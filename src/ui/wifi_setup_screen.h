#pragma once

#include <Arduino.h>

namespace wifi_setup_screen {

// Build the screen and show it. Pass the SoftAP SSID and password
// (empty pass = open AP) — used to encode the WIFI: payload of the QR.
void show(const String& ap_ssid, const String& ap_pass);

// Refresh dynamic fields (status line). Safe to call when the screen
// isn't current — it just no-ops.
void tick();

}  // namespace wifi_setup_screen
