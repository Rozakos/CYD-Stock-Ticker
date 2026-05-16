# AGENTS.md — handoff notes for Claude / coding agents

This file gives the next session enough context to keep going without
re-deriving everything from scratch. Treat it as living docs — keep it short
and current; delete stale notes rather than leaving them.

## What this project is

ESP32-2432S028R "Cheap Yellow Display" firmware: a 320×240 stock ticker.
List screen of symbols, detail screen with a chart, on-device settings info
screen, and a small web admin at `/settings` for editing the API bearer
token / symbols / refresh interval. Data source is a self-hosted yfinance
proxy at `https://rozakos.eu/stocks/api/v1` (repo: Rozakos/stock-api).

## Stack & invariants

- **PlatformIO** env `cyd`, board `esp32dev`, Arduino-ESP32 framework.
- **LVGL 9.5** (pulled by `lvgl @ ^9.2.2` — caret resolves up). LVGL config
  lives in `include/lv_conf.h`; the project uses `LV_CONF_INCLUDE_SIMPLE` and
  `-I include` so this file is the single source of truth.
- **LovyanGFX 1.2.x** for ST7789 + XPT2046 (dual-USB ESP32-2432S028R
  revision; original single-micro-USB rev is ILI9341 — see "Known
  pitfalls" below). The `Touch_XPT2046` driver carries its own SPI pin
  config in this version — there is *no* `setBus()` method. See
  `src/display/lgfx_cyd.hpp`.
- **RGB565 byte order**: `LV_COLOR_16_SWAP 0` means LVGL writes
  little-endian RGB565 bytes to the flush buffer. Pass `swap = true` to
  `LGFX::writePixels(...)` so LovyanGFX byte-swaps to big-endian (MSB-first)
  before sending over SPI. Invariant: `LV_COLOR_16_SWAP=0` → `swap=true`.
  See `src/display/lvgl_bridge.cpp::flush_cb`.
- **ST7789 invert flag**: `cfg.invert = false` is correct for this board.
  `invert = true` sends the ST7789 INVON command which XORs every pixel with
  0xFFFF — producing complement colors (green → pink, red → cyan). Do NOT
  set `invert = true` even if it seems like it might be needed; it will
  silently invert every color channel. See `src/display/lgfx_cyd.hpp`.
- **Two FreeRTOS tasks**, pinned: `uiTask` on core 1 (LVGL), `netTask` on
  core 0 (WiFi + HTTPS + web admin). They share `QuoteStore` and
  `SettingsStore` under a mutex. LVGL itself is single-threaded behind
  `g_lvglMu`; LVGL's own OSAL is set to `LV_OS_NONE` because ESP-IDF doesn't
  ship the `atomic.h` that LVGL's FreeRTOS OSAL expects.
- **JSON parsing**: ArduinoJson v7 with field filters so the streamed
  body never lands fully in RAM. `/stock/{sym}` filter keeps `last`,
  `change_pct`, `closes`; `/history/{sym}` filter keeps `points[].last`.
- **HTTPS**: `WiFiClientSecure::setInsecure()` + `http.useHTTP10(true)` +
  `Accept-Encoding: identity`. Don't change these without testing — they
  are workarounds for actual breakage we've hit in production.
- **stock-api auth**: every request sends `Authorization: Bearer <token>`
  AND a non-empty `User-Agent` (`cfg::API_USER_AGENT`). The UA is *not*
  cosmetic — Cloudflare bot-fight at the edge drops empty/default UAs
  before the request reaches the FastAPI app.

## Memory budget (the part that bites)

Last clean build: **RAM 34.7%**, **Flash 78.8%**. DRAM is the constraint when
adding LVGL features. Two main levers:

- `LINES` in `src/display/lvgl_bridge.cpp` — partial-render flush buffer
  height. Currently 16 (two buffers of 320×16×2 = ~20 KB total).
- `LV_MEM_SIZE` in `include/lv_conf.h` — LVGL heap. Currently 32 KB.

If a future change overflows DRAM (link error
`region 'dram0_0_seg' overflowed`), drop `LINES` or `LV_MEM_SIZE` before
adding bigger fonts. Fonts go in flash, not DRAM — adding a Montserrat size
costs flash, not DRAM.

## Source layout

```
src/
  main.cpp                       FreeRTOS tasks, mutex, LittleFS, LVGL init
  config.h                       SCREEN_W/H, HISTORY_POINTS, SPARKLINE_POINTS
  secrets.h                      gitignored — seed bearer token + fallback WiFi creds
  secrets_example.h              template

  display/
    lgfx_cyd.hpp                 LovyanGFX panel + touch wiring
    lvgl_bridge.{h,cpp}          LVGL flush_cb, touch_cb, tick_cb, buffers
    fs_littlefs.{h,cpp}          LVGL FS driver -> LittleFS, drive 'L'

  net/
    quote_store.{h,cpp}          Quote{symbol,last,changePct,fresh,sparkline}
                                 + History{symbol,closes}. Mutex-guarded.
    quote_fetcher.{h,cpp}        fetchQuotes / fetchHistory. Filtered JSON.
    wifi_mgr.{h,cpp}             STA connect + open-AP fallback (CYD-Setup-XXXX)
    captive_portal.{h,cpp}       DNS hijack + AsyncWebServer /save for AP mode
    web_admin.{h,cpp}            AsyncWebServer at /settings (basic auth)

  settings/
    settings_store.{h,cpp}       LittleFS-backed runtime settings (incl. wifi creds)

  ui/
    styles.{h,cpp}               colors, fonts, brand_color(symbol) lookup
    logos.{h,cpp}                PNG via LVGL/lodepng with badge fallback
    list_screen.{h,cpp}          status bar (wifi/title/clock/gear) + rows
    detail_screen.{h,cpp}        header card + chart card + stats
    settings_screen.{h,cpp}      read-only on-device info screen
    wifi_setup_screen.{h,cpp}    fullscreen QR + instructions shown in AP mode
```

## How features wire together

- **Brand colors**: `styles::brand_color(symbol)` first hits `kBrandTable`,
  then falls back to an FNV hash into `kFallback`. Used by both the badge and
  (optionally) the chart line.
- **Sparklines on list rows**: derived from the same `fetchQuotes` response.
  The fetcher requests `limit=SPARKLINE_POINTS` daily bars per symbol and the
  last N closes get stored on `Quote::sparkline`. The list draws them with
  `lv_line` (one per row) — no extra API calls.
- **Detail history**: separate on-demand fetch (`limit=HISTORY_POINTS`) when
  a row is tapped. Pending request flows UI -> store -> net via
  `requestHistory` / `takePendingHistory`.
- **Logos**: `logos::make(parent, sym, size)` first looks up an embedded
  ARGB8888 `lv_image_dsc_t` via `logos_data::find(symbol)`
  (`src/ui/logos_data.{cpp,h}`, generated from `data/logos/*.png` by
  `sim/build_logo_arrays.py`). If the symbol isn't bundled, it falls
  back to a runtime LittleFS PNG read (`L:/logos/<SYMBOL>.png`), then to
  a circular brand-colored letter badge. The embedded path works
  identically on the device and in the sim because it skips the LVGL FS
  + lodepng pipeline that the sim's software flush doesn't render
  visually. To add or refresh logos: drop PNGs into `data/logos/` and
  run `python sim/build_logo_arrays.py` from the project root.
- **Settings screen**: read-only. Editing happens in the web admin (no
  on-device keyboard). The screen pulls live WiFi info each tick and
  exposes the `http://<ip>/` URL so the user knows where to go.

## Build / verify

```
pio run -e cyd                     # compile + link
pio run -e cyd -t upload           # flash firmware
pio run -e cyd -t uploadfs         # flash LittleFS (settings + logos)
pio device monitor                 # serial @ 115200
```

PIO path on Windows: `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`.

## Conventions

- C++17, two-space indent, `lower_snake` namespaces, `CamelCase` types,
  `g_` prefix for file-locals.
- Comments only when the *why* is non-obvious. Avoid restating what the
  code does.
- Don't introduce new abstractions for hypothetical needs. The codebase
  prefers one explicit way per feature.
- LVGL widget construction: when overriding most of a style, call
  `lv_obj_remove_style_all(obj)` first, then layer the project's named
  styles from `styles.cpp`.
- Event bubbling: child widgets inside a clickable card should set
  `LV_OBJ_FLAG_EVENT_BUBBLE` so taps reach the parent.

## Known pitfalls / footguns

- **`LV_USE_SPINNER` requires `LV_USE_ARC`** in LVGL 9.5. Both are on.
- **`LV_USE_OS LV_OS_FREERTOS`** pulls in `lv_freertos.c` which
  `#include "atomic.h"` — missing on ESP-IDF. Stay on `LV_OS_NONE`.
- **`Touch_XPT2046::setBus()` does not exist** in this LovyanGFX. SPI pins
  go directly into the touch config struct.
- **Quote/History String + Arduino.h**: any header that uses `String` must
  `#include <Arduino.h>` itself; don't rely on the includer.
- **DRAM-tight links**: see the memory budget section above before adding
  fonts/widgets.

## LVGL desktop simulator

`sim/` is a host build that compiles the project's UI sources (`src/ui/*`,
`quote_store`, `settings_store`) against a thin Arduino shim and an SDL2 +
software-rendered LVGL display. Two modes:

- `--window`: SDL window for interactive testing.
- `--headless --out=foo.png`: renders a few frames and dumps a PNG. This is
  the path Claude uses to *see* UI changes without flashing the device.

Toolchain is MSYS2 + MinGW + SDL2 (see `sim/README.md`). Build with
`cmake -S sim -B sim/build -G Ninja && cmake --build sim/build`. Run from
`sim/build/`. The shim lives in `sim/compat/` and only covers what the
linked-in files actually use — grow it lazily.

One sim caveat worth knowing:

- **`LV_MEM_SIZE` is wrapped in `#ifndef`** in `lv_conf.h` so the sim
  CMake can override it (sim uses 512 KB; device stays at 32 KB).

(The previous PNG-via-LVGL-FS rendering gap was sidestepped by moving
logos to compile-time ARGB8888 C arrays — see the Logos section above.)

## Recently shipped (most recent first)

- **Detail chart polish — three commits** (2026-05):
  1. *Axis labels + grid* (`73f0a04`): Y-axis price labels at div lines 1/2/3
     (left edge, `styles::muted`); X-axis "DD MMM" date labels (Latin months,
     fixed table, no strftime) anchored at bottom-left / bottom-center /
     bottom-right. Div line count raised 3→5 for a finer grid.
     Chart expanded from 120→142 px (full card content area); stats row
     (LOW/RANGE/HIGH) removed. `pad_all=0` on chart aligns div lines to
     the pixel grid — do NOT remove this.  Note: LVGL's bundled Montserrat
     fonts only cover Latin glyphs; Greek months need a custom font build.
  2. *Gradient fill* (`c0ef0d9`): correct LVGL 9 pattern — `bg_grad_dir=VER`,
     `bg_main_stop=0`, `bg_grad_stop=255`, `bg_opa=LV_OPA_50`,
     `bg_grad_color=black`. Line color fades to transparent at bottom.
  3. *Catmull-Rom smooth curve* (`00e638b`): `src/util/interpolate.{h,cpp}`
     — uniform CR spline, factor=5, static buffer, no heap. History closes
     (30 pts) → ~146 smoothed points fed to the chart. Unit tests at
     `test/test_native/` pass via `pio test -e native` (needs MSYS2 gcc on
     PATH). `[env:native]` added to `platformio.ini`.
- **Sim SDL2 link fix** (2026-05): cmake 4.x resolves bare `SDL2` to
  `SDL2.lib` (Windows import lib, does not exist); fixed by linking with
  the flag form `"-lSDL2"` so ld uses `libSDL2.dll.a`.  Must build the sim
  via MSYS2 shell (`C:\msys64\msys2_shell.cmd -mingw64 -defterm -no-start
  -c "cd ... && ninja -C sim/build cyd_sim"`); the PowerShell / bash tool
  sandbox blocks g++.exe from writing object files.
- **Embedded ARGB8888 logos** (2026-05): logos now render via compile-time
  C arrays (`src/ui/logos_data.{cpp,h}`, generated by
  `sim/build_logo_arrays.py` from `data/logos/*.png`). `logos::make` calls
  `logos_data::find(symbol)` first and only falls back to the LittleFS
  PNG path if no embedded entry exists. Flash usage jumps to ~94% (30
  logos × 9 KB = ~280 KB), still inside the 1.9 MB app slot. Surfaced
  during the no-logos debug: the LVGL FS / lodepng pipeline silently
  failed to *render* decoded PNGs in the sim's software flush path, even
  though `lv_image_get_src_width` reported correct 48×48 dimensions —
  embedding bypasses that whole path on both targets.
- **Badge background fix** (2026-05): `makeBadge` in `src/ui/logos.cpp` was
  calling `lv_obj_remove_style_all(badge)` before setting local `bg_opa` /
  `bg_color` styles. In LVGL 9 this stripped the theme's `bg_opa=LV_OPA_COVER`
  baseline; subsequent local `set_style_bg_opa` calls did not recover it,
  leaving every badge background transparent (only the letters were visible).
  Fix: remove the `lv_obj_remove_style_all` call so the theme baseline stays
  in place. Also added `lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE)` to
  suppress the default scroll-indicator on a plain `lv_obj_t`.
  Sim caveat: when logo PNGs are present in `data/logos/` the PNG path is
  taken and `makeBadge` is not called; pass `--data=/tmp` to force badge mode
  in the sim. PNG logos do not render visually in the sim (pre-existing
  color-format issue, on-device rendering is unaffected).
- **WiFi captive portal + QR onboarding** (2026-05): WiFi creds are now
  stored in `/settings.json` (`wifi_ssid`/`wifi_pass`) instead of being
  compile-time constants. `wifi_mgr::begin(SettingsStore&)` tries STA;
  on failure it opens an open AP named `CYD-Setup-<MAC>` and the
  device shows a fullscreen QR code (`WIFI:T:WPA;S:...;P:...;;`) so a
  phone can join in one scan. `captive_portal::begin()` runs a DNS
  hijack (port 53) + AsyncWebServer at `192.168.4.1` with a network
  picker; POST `/save` writes creds and triggers an STA retry. Probe
  URLs for Android (`/generate_204`), iOS (`/hotspot-detect.html`),
  Windows (`/connecttest.txt`) are wired so phones pop the captive
  sheet automatically. Requires `LV_USE_QRCODE 1` (+ `LV_USE_CANVAS 1`
  dep) in `lv_conf.h`. Existing devices upgrade smoothly: the loader
  falls back to `WIFI_SSID`/`WIFI_PASS` from `secrets.h` when
  `wifi_ssid` is absent from settings, so on-disk state from prior
  firmware just keeps working.
- **Swap data source from RapidAPI Yahoo to self-hosted stock-api**
  (2026-05): `src/net/quote_fetcher.cpp` now hits
  `https://rozakos.eu/stocks/api/v1` with bearer-token auth and a
  required non-empty User-Agent. `RAPID_KEY_SEED` → `API_TOKEN_SEED`
  in `secrets.h`. Quote response now includes pre-computed
  `last`/`change_pct` and a `closes[]` array used directly for the
  sparkline (5 daily closes vs the prior 10). History endpoint now
  returns minute bars (`/history/{sym}?days=1`) so the detail chart
  shows intraday rather than 30 daily closes — bump `days=` in
  `fetchHistory` to widen the window.
- **LVGL desktop simulator** (2026-05): added `sim/` with CMake/MSYS2/SDL2
  build that compiles the UI sources against an Arduino shim and renders
  to either an SDL window or PNG screenshots. Headless mode lets the
  agent verify UI changes by reading the dumped PNG instead of flashing.
  Surfaced a real layout bug — long symbols string overflows the settings
  screen card and overlaps the footer URL.
- **Color fix** (2026-05): `cfg.invert = false` in `lgfx_cyd.hpp` +
  `writePixels(swap=true)` in `lvgl_bridge.cpp`. The previous `invert=true`
  was sending the ST7789 INVON command which bit-inverted every pixel,
  turning green into pink and red into cyan. Removing it restores correct
  colors. Byte-swap stays `true` (`LV_COLOR_16_SWAP=0` → little-endian LVGL
  output requires a swap to match what the ST7789 SPI interface expects).
- **Display driver fix for dual-USB CYD revision** (2026-05): identified
  the board as ESP32-2432S028R v2/v3 (USB-C + micro-USB), switched
  `lgfx::Panel_ILI9341` → `lgfx::Panel_ST7789` in `src/display/lgfx_cyd.hpp`
  to fix scrambled-noise rendering. Touch axes corrected by swapping
  `x_min`/`x_max` and `y_min`/`y_max` in the XPT2046 config.
- On-device read-only settings info screen + gear button in status bar; wifi
  glyph is now color-coded and shows RSSI dBm.
- Logo widget (PNG from LittleFS via custom LVGL FS driver, brand-colored
  letter badge fallback), full UI polish pass: sparklines on list rows,
  area-fill chart on detail with high/low/range stats, broader font ramp.
- Pre-existing build issues uncovered in the polish pass and fixed:
  `Touch_XPT2046::setBus()` removed, `Arduino.h` added to `quote_fetcher.h`,
  `LV_USE_ARC` enabled, `LV_USE_OS` switched to `LV_OS_NONE`.

## Likely next asks

- **Fix the time display** (user asked but not yet implemented): the status
  bar clock (`list_screen.cpp`) shows NTP time via `time(nullptr)`. If it
  shows wrong time, check that `configTime(tz_offset_sec, 0, "pool.ntp.org")`
  is called in `main.cpp` after WiFi connects. Currently no timezone is set;
  device runs UTC. Fix: call `configTime(3*3600, 0, ...)` for UTC+3 (Greece).
- **Web admin access** (user asked): once the device is on WiFi, open a
  browser to `http://<device-ip>/settings`. The IP is shown on the on-device
  settings screen (gear icon in status bar). Basic-auth credentials are in
  `src/net/web_admin.cpp` (`admin` / `password` by default — change these).
- Long-press a row to mark it as a "favorite" / pin to top.
- Switch to hourly bars (`interval=1h`) on a long-press of the chart.
- Per-symbol price-alert thresholds in the settings form.
- Greek month abbreviations: need Montserrat 12/14 rebuilt with Greek Unicode
  range (U+0370–U+03FF) via the LVGL font converter. Flash at 94% — tight.

Don't pre-build any of these unless asked — list them so the next session
has a head start.
