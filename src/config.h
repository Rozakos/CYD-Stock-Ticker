#pragma once

#include <cstdint>

namespace cfg {

// Self-hosted yfinance proxy at rozakos.eu. Bearer-token auth; Cloudflare
// bot-fight blocks empty/default User-Agents, so requests MUST send a
// non-empty UA — see API_USER_AGENT.
inline constexpr const char* API_BASE       = "https://rozakos.eu/stocks/api/v1";
inline constexpr const char* API_USER_AGENT = "CYD-Stock-Ticker/1.0 (ESP32)";

inline constexpr const char* DEFAULT_SYMBOLS =
    "AAPL,MSFT,NVDA,TSLA,GOOG";

inline constexpr uint32_t DEFAULT_REFRESH_SECONDS = 60;
inline constexpr uint32_t MIN_REFRESH_SECONDS     = 15;

inline constexpr const char* DEFAULT_ADMIN_USER = "admin";
inline constexpr const char* DEFAULT_ADMIN_PASS = "admin";

inline constexpr const char* SETTINGS_PATH = "/settings.json";

inline constexpr uint16_t SCREEN_W = 320;
inline constexpr uint16_t SCREEN_H = 240;

inline constexpr uint16_t HISTORY_POINTS  = 30;
inline constexpr uint16_t SPARKLINE_POINTS = 10;

}  // namespace cfg
