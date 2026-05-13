#pragma once

// Copy this file to src/secrets.h and fill in real values.
// src/secrets.h is gitignored.

inline constexpr const char* WIFI_SSID = "your-ssid";
inline constexpr const char* WIFI_PASS = "your-password";

// Seed RapidAPI key used only on first boot if no /settings.json exists in
// LittleFS. After first boot the runtime key in LittleFS takes over — manage
// it via the /settings web UI. Blank this string for a key-free firmware
// image after you've provisioned the device once.
inline constexpr const char* RAPID_KEY_SEED = "";
