#pragma once

#include <cstdint>

namespace cfg {

// Self-hosted yfinance proxy at rozakos.eu. Bearer-token auth; Cloudflare
// bot-fight blocks empty/default User-Agents, so requests MUST send a
// non-empty UA — see API_USER_AGENT.
inline constexpr const char* API_BASE       = "https://rozakos.eu/stocks/api/v1";
inline constexpr const char* API_USER_AGENT = "CYD-Stock-Ticker/1.0 (ESP32)";
// Eastern European time with DST: UTC+2 winter, UTC+3 summer.
inline constexpr const char* TIME_TZ        = "EET-2EEST,M3.5.0/3,M10.5.0/4";

inline constexpr const char* DEFAULT_SYMBOLS =
    "AAPL,MSFT,NVDA,TSLA";   // 4 fits the screen without auto-scroll

inline constexpr uint32_t DEFAULT_REFRESH_SECONDS = 20;
inline constexpr uint32_t MIN_REFRESH_SECONDS     = 15;

inline constexpr const char* SETTINGS_PATH = "/settings.json";

inline constexpr uint16_t SCREEN_W = 320;
inline constexpr uint16_t SCREEN_H = 240;

inline constexpr uint16_t HISTORY_POINTS  = 30;
inline constexpr uint16_t SPARKLINE_POINTS = 10;

// Diagnostic switch for the runtime-logo pipeline. When true, the fetcher
// appends ?test=1 to /logo/<SYMBOL> — the API returns a synthetic 64×64
// RGBA PNG (red bg, green diagonal stripe, blue centre dot, Cache-Control:
// no-store). LittleFS cache is bypassed on the fetch side so we never
// reuse a stale file. The UI still decodes that cached PNG through the
// runtime in-memory ARGB path, so this isolates fetch/cache/decode from
// any per-symbol logo content issue.
inline constexpr bool LOGO_TEST_MODE = false;

}  // namespace cfg
