# AGENTS.md — handoff notes for Claude / coding agents

This file gives the next session enough context to keep going without
re-deriving everything from scratch. Treat it as living docs — keep it short
and current; delete stale notes rather than leaving them.

## What this project is

ESP32-2432S028R "Cheap Yellow Display" firmware: a 320×240 stock ticker.
List screen of symbols, detail screen with a chart, on-device settings info
screen, and a small web admin at `/settings` for editing the RapidAPI key /
symbols / refresh interval.

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
- **RGB565 byte order**: `LV_COLOR_16_SWAP = 0` in `lv_conf.h`, so the
  flush callback must pass `swap = true` to `LGFX::writePixels(...)`.
  Without it the panel renders garbage that looks like noise. See
  `src/display/lvgl_bridge.cpp::flush_cb`.
- **Two FreeRTOS tasks**, pinned: `uiTask` on core 1 (LVGL), `netTask` on
  core 0 (WiFi + HTTPS + web admin). They share `QuoteStore` and
  `SettingsStore` under a mutex. LVGL itself is single-threaded behind
  `g_lvglMu`; LVGL's own OSAL is set to `LV_OS_NONE` because ESP-IDF doesn't
  ship the `atomic.h` that LVGL's FreeRTOS OSAL expects.
- **JSON parsing**: ArduinoJson v7 with a `close`-only filter to keep the
  streamed body off the heap. Tolerates three response shapes: `body[]`,
  `data.items[]`, `data.prices[]`. Both numeric and string close values are
  accepted.
- **HTTPS**: `WiFiClientSecure::setInsecure()` + `http.useHTTP10(true)` +
  `Accept-Encoding: identity`. Don't change these without testing — they
  are workarounds for actual breakage on this RapidAPI host.

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
  secrets.h                      gitignored — real WiFi/RapidAPI key
  secrets_example.h              template

  display/
    lgfx_cyd.hpp                 LovyanGFX panel + touch wiring
    lvgl_bridge.{h,cpp}          LVGL flush_cb, touch_cb, tick_cb, buffers
    fs_littlefs.{h,cpp}          LVGL FS driver -> LittleFS, drive 'L'

  net/
    quote_store.{h,cpp}          Quote{symbol,last,changePct,fresh,sparkline}
                                 + History{symbol,closes}. Mutex-guarded.
    quote_fetcher.{h,cpp}        fetchQuotes / fetchHistory. Filtered JSON.
    wifi_mgr.{h,cpp}             non-blocking connect
    web_admin.{h,cpp}            AsyncWebServer at /settings (basic auth)

  settings/
    settings_store.{h,cpp}       LittleFS-backed runtime settings

  ui/
    styles.{h,cpp}               colors, fonts, brand_color(symbol) lookup
    logos.{h,cpp}                PNG via LVGL/lodepng with badge fallback
    list_screen.{h,cpp}          status bar (wifi/title/clock/gear) + rows
    detail_screen.{h,cpp}        header card + chart card + stats
    settings_screen.{h,cpp}      read-only on-device info screen
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
- **Logos**: `logos::make(parent, sym, size)` tries
  `L:/logos/<SYMBOL>.png` (LVGL FS driver -> LittleFS); on miss it draws a
  circular brand-colored badge with letters. Logo PNGs live in
  `data/logos/` and are uploaded with `pio run -t uploadfs`. The folder may
  be empty.
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

Two sim caveats worth knowing:

- **`LV_MEM_SIZE` is wrapped in `#ifndef`** in `lv_conf.h` so the sim
  CMake can override it (sim uses 512 KB; device stays at 32 KB).
- **PNG logos render as letter-badges in the sim only.** The LVGL FS
  driver opens the files (verified), lodepng is linked, but the decoded
  image doesn't appear. On-device PNG rendering is unaffected. Not yet
  diagnosed — probably a color-format mismatch in the sim's flush path.

## Recently shipped (most recent first)

- **LVGL desktop simulator** (2026-05): added `sim/` with CMake/MSYS2/SDL2
  build that compiles the UI sources against an Arduino shim and renders
  to either an SDL window or PNG screenshots. Headless mode lets the
  agent verify UI changes by reading the dumped PNG instead of flashing.
  Surfaced a real layout bug — long symbols string overflows the settings
  screen card and overlaps the footer URL.
- **Display driver fix for dual-USB CYD revision** (2026-05): identified
  the board as ESP32-2432S028R v2/v3 (USB-C + micro-USB), switched
  `lgfx::Panel_ILI9341` → `lgfx::Panel_ST7789` with `invert = true` in
  `src/display/lgfx_cyd.hpp`, and added the missing RGB565 byte-swap
  (`writePixels(..., true)`) in `src/display/lvgl_bridge.cpp` to fix
  "scrambled noise" rendering. **Still open from that session**: touch
  X-axis is mirrored (touching one side registers on the opposite side)
  — fix is to swap `x_min` ↔ `x_max` in the touch config, or set the
  touch `offset_rotation` to match. Display quality also reported as
  "doesn't look great" so RGB order / further panel tuning may still be
  needed.
- On-device read-only settings info screen + gear button in status bar; wifi
  glyph is now color-coded and shows RSSI dBm.
- Logo widget (PNG from LittleFS via custom LVGL FS driver, brand-colored
  letter badge fallback), full UI polish pass: sparklines on list rows,
  area-fill chart on detail with high/low/range stats, broader font ramp.
- Pre-existing build issues uncovered in the polish pass and fixed:
  `Touch_XPT2046::setBus()` removed, `Arduino.h` added to `quote_fetcher.h`,
  `LV_USE_ARC` enabled, `LV_USE_OS` switched to `LV_OS_NONE`.

## Likely next asks

- Bundle some real logo PNGs in `data/logos/`. The infrastructure is in
  place; what's missing is licensed art.
- Long-press a row to mark it as a "favorite" / pin to top.
- Show a tiny clock (NTP-synced) in the status bar.
- Switch to hourly bars (`interval=1h`) on a long-press of the chart.
- Per-symbol price-alert thresholds in the settings form.

Don't pre-build any of these unless asked — list them so the next session
has a head start.
