#pragma once

#include <cstdint>

namespace cfg {

inline constexpr const char* RAPID_HOST  = "yahoo-finance15.p.rapidapi.com";
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
