#pragma once

class SettingsStore;

namespace captive_portal {

// Spins up DNS hijack (port 53) + AsyncWebServer setup page (port 80).
// Should be called only when wifi_mgr is in AP mode.
void begin(SettingsStore& settings);

// Stops the DNS server and web server. Safe to call repeatedly.
void end();

// Must be ticked from the net task to service DNS requests.
void loop();

}  // namespace captive_portal
