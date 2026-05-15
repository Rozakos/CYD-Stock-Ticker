#pragma once

// Copy this file to src/secrets.h and fill in real values.
// src/secrets.h is gitignored.

inline constexpr const char* WIFI_SSID = "your-ssid";
inline constexpr const char* WIFI_PASS = "your-password";

// Seed bearer token for the self-hosted stock API. Used only on first boot
// if no /settings.json exists in LittleFS. After first boot the runtime token
// in LittleFS takes over — manage it via the /settings web UI. Blank this
// string for a token-free firmware image after you've provisioned the device
// once.
inline constexpr const char* API_TOKEN_SEED = "";
