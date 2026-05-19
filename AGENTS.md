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

Last clean build: **RAM 36.0%**, **Flash 94.5%**. DRAM is the constraint when
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
    web_admin.{h,cpp}            AsyncWebServer at /settings (no auth)

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
- **Detail history / range contract**: range buttons request
  `/history/{SYMBOL}?range=<token>` where tokens are `1d`, `5d`, `1w`,
  `1mo`, and `6mo`. The API server is responsible for selecting the
  correct date/window and returning ordered `points[]` with `ts` and `last`
  plus top-level `interval` (`intraday` or `daily`). The firmware does not
  calculate which dates belong to "1W" or fetch from Google/Yahoo directly.
  It calculates the displayed detail gain/loss locally from the returned
  points only: `(last_point.last - first_point.last) / first_point.last *
  100`. Pending request flows UI -> store -> net via `requestHistory` /
  `takePendingHistory`; `History::range` must match the pending tab before
  the UI renders it.
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

## Debugging Range Percentages

If a tab like `1W` on `AMD` shows a surprising gain/loss, first inspect the
serial logs rather than changing the UI math. The firmware logs the exact
history data it used:

```
[AMD] history 1w: interval=daily pts=... first=... last=... change=... ts=.....
[ui] AMD 1w rendered: first=... last=... change=... pts=...
```

Interpretation:

- If fetcher and UI logs match, the device is calculating correctly from the
  API payload; check the stock-api server's `range=1w` window/adjusted price
  semantics.
- If fetcher and UI logs differ, suspect stale cached history, generation
  cancellation, or `History::range` matching.
- For `1M`, a recent mismatch example was device showing `+90.04%` while
  Google showed roughly `+53.12%`; that kind of spread is likely a server
  history window or price-adjustment issue unless the serial first/last
  values differ from what the API returns.

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
- `--web-settings=foo.html`: renders the same `/settings` browser UI used by
  firmware to a standalone HTML file. Open it in a browser to review web admin
  changes without flashing.

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

- **Runtime logo fetch + diagnostic mode** (2026-05, Codex): firmware now
  downloads missing, non-embedded logos from `/logo/{SYMBOL}` into
  LittleFS as `/logos/<SYMBOL>.png`, then the existing UI fallback renders
  them via `L:/logos/<SYMBOL>.png`. Embedded symbols are skipped so flash
  logos do not waste API calls. Runtime logo rows rebuild their logo widget
  on quote refresh so a badge can switch to a cached PNG without reboot.
  **Temporary diagnostic is enabled** in
  `src/net/quote_fetcher.cpp::kLogoApiTestMode`, appending `?test=1` to
  non-embedded logo requests. Revert this to `false` after confirming
  whether the CYD renders the stock-api synthetic red/green/blue PNG.
  Runtime PNGs are wrapped in a neutral gray circular backing with a light
  border while debugging logo contrast/rendering. Serial logs of interest:
  `[SYMBOL] logo cached: ...`, `[logo] SYMBOL LittleFS ... dims=...`, and
  `[logo] SYMBOL runtime image mounted ...`.
- **Captive portal watchdog fix + onboarding logs** (2026-05, Codex):
  moved `WiFi.scanNetworks()` out of AsyncWebServer request callbacks after
  phones joining `CYD-Setup-*` triggered `async_tcp` task watchdog resets.
  The portal now scans once before `server.begin()` and logs AP/STA
  transitions, captive probes, `/`, `/save`, and reconnect attempts.
- **Touch restore + range tap fix + local time + NOK/TTWO logos** (2026-05,
  Codex): reverted the CYD touch calibration to the known-working mirrored
  X/Y values (`x_min=3900`, `x_max=300`, `y_min=3850`, `y_max=200`) after a
  normal-X experiment broke device touch. The detail range row remains
  `1D / 5D / 1W / 1M / 6M` with 1M default; firmware reverses only the
  range-button `user_data` mapping under non-sim builds so physical mirrored
  taps request the visual button. Sim builds keep natural left-to-right
  mapping; verified injected sim taps on 5D and 6M select the matching
  buttons. Clock now sets `TZ=EET-2EEST,M3.5.0/3,M10.5.0/4` before NTP, so
  the status bar uses EET/EEST local time instead of UTC. Added embedded NOK
  (2023 wordmark from Wikimedia, not the old Nokia logo) and TTWO logos via
  `data/logos/*.png` + regenerated `src/ui/logos_data.cpp`; flash is now
  **95.4%**, so prefer a server-backed cached logo fetcher before embedding
  many more symbols. Plain PowerShell did not have `cmake` on PATH, so
  `sim/build_sim.ps1` now drives `C:\msys64\msys2_shell.cmd` directly and
  `sim/README.md` uses that as the default sim build command. Sim note:
  range-click injection works after `sim_lvgl_bridge.cpp` now advances time
  between press/release frames; list-row injected navigation still appears
  to hit a separate sim-only hang and was not used as the flashing gate.
- **Detail range gain/loss fix** (2026-05, Codex): detail header percent and
  up/down colour now come from the selected history window's first-to-last
  change, not the quote's published day `change_pct`. This means tapping
  `5D`, `1W`, `1M`, or `6M` displays that range's gain/loss. Follow-up
  fix: `History` now carries the API `range`, and the detail screen renders
  cached history only when both `symbol` and `range` match the pending tab;
  this prevents stale same-symbol history from being rendered under a newly
  selected tab before the network task returns. Serial diagnostics now log
  both fetcher-side and UI-side first/last/change values:
  `history <range>: interval=... pts=... first=... last=... change=...`.
  The sim services pending history requests with per-range fake data so this
  can be visually verified before flashing. Screenshots generated in
  `sim/build/`: `range_guard_5d.png` (`-2.28%`),
  `range_guard_1w.png` (`+2.27%`), `range_guard_1m.png` (`+4.70%`), and
  `range_guard_6m.png` (`-6.44%`). If `History` or other shared structs
  change again, run `.\sim\build_sim.ps1 -Clean` to avoid stale sim objects.
- **Rozakos-branded `/settings` web admin + auth removal** (2026-05,
  Codex): ported the ESP8266 reference UI styling into
  `src/net/web_admin.cpp`: dark Rozakos Industries header, inline robot SVG,
  split-pill ROZAKOS|INDUSTRIES wordmark, stock datalist autocomplete, and
  matching footer. The UI now mirrors the ESP8266 add/remove flow for
  symbols: a table with per-row delete buttons plus an Add symbol form
  (`POST /add`, `POST /delete`), while the underlying setting remains the
  same symbols CSV. The shared markup lives in `src/net/web_admin_page.h`;
  firmware streams it from `web_admin.cpp`, and the sim can dump it with
  `cyd_sim.exe --web-settings=settings_web.html`. The settings form is
  refresh interval (minimum 15 s) and API bearer token only. HTTP basic auth
  was removed entirely: no
  `request->authenticate(...)`, no admin fields in the form, and
  `SettingsStore` no longer reads/writes `admin_user` / `admin_pass` (legacy
  keys in existing `/settings.json` are ignored). GET `/` now 303-redirects
  to `/settings`; POST `/settings` saves then 303-redirects back. Verified
  with `pio run -e cyd` (RAM 36.0%, Flash 94.5%),
  `cmake --build sim/build --target cyd_sim`, and
  `cyd_sim.exe --web-settings=settings_web.html`.
- **Range buttons + interval-aware X axis** (2026-05): detail screen
  gained a 5-button `lv_buttonmatrix` (1D / 5D / 1W / 1M / 6M) between
  the header and the chart. Default checked button is **1M**;
  `lv_buttonmatrix_set_one_checked(true)` so the framework manages the
  mutually-exclusive checked state. Active-button bg colour follows the
  up/down accent (refreshed in render_history).
  - `History` gained `interval` ("intraday" | "daily"). Fetcher requests
    `/history/{sym}?range=<value>` (the hardcoded `days=2` is gone) and
    parses both the points array AND the top-level `interval` field.
  - In-flight cancellation works via a generation counter:
    `QuoteStore::requestHistory` returns a `gen`, the fetcher captures
    it, and right before storing the result it re-checks
    `historyGenCurrent(gen)` — a later requestHistory call has already
    bumped the counter, so the old result is silently dropped.
  - X-axis formatter: `intraday` → `HH:MM` (gmtime, no local TZ on
    device), anything else → `DD MMM` (existing English month table).
  - Loading state: `lv_spinner` over the chart at 50 % opacity while
    a fetch is in flight. Error state: `"no data"` red label at chart
    centre when `QuoteStore::historyError()` is true. Buttons remain
    interactive so the user can tap another range to retry.
  - Layout: header collapsed to 36 px (logo 32, single-line:
    logo / symbol / price / change% / back hint) so the 24 px button
    row fits without dropping the chart below ~140 px. CARD_Y / CARD_H
    recomputed from the new constants.
  - Sim: `sim/fake_data.cpp` gained `seed_intraday()` and sim_main
    exposes `--intraday` to swap the AAPL history for 1-minute-spaced
    intraday data, useful for visually verifying the HH:MM formatter.
    The `--click` post-tick loop is now gated by `args.screen` (same
    fix as the main loop) so injected clicks on the detail screen
    don't trip the `list_screen::tick()` segfault.


- **Chart fill rewrite + PCHIP** (2026-05): two follow-up commits.
  - *Single-gradient area fill* (`7a4a85b`): the per-trapezoid
    gradients with bbox-scaled stops still produced visible vertical
    striping because LVGL's renderer quantises opacity per-column at
    trapezoid seams. Replaced with **one** `lv_draw_rect` covering
    the whole plot area (line_color @ top → transparent @ bottom)
    plus N solid-color "erase" trapezoids in the card's bg color
    above the line. One gradient, one continuous interpolation, no
    seams.
  - *PCHIP / Fritsch-Carlson interpolation*: replaced uniform
    Catmull-Rom with monotone cubic Hermite. Tangents at sign
    changes between adjacent secants are clamped to 0, and the
    Fritsch-Carlson α²+β²≤9 constraint guarantees no overshoot or
    undershoot on monotonic runs — kills the wiggles on noisy
    daily-close series. Function renamed to
    `util::monotone_cubic_interpolate`; same input/output API
    (factor=5, `(n-1)*factor+1` outputs). Unit tests at
    `test/test_native/` extended with monotonicity assertions on
    both increasing and decreasing inputs, plus a line-exact check.
    Static slope/secant buffers add ~2 KB DRAM (RAM 35.4% → 36.0%).
- **Second chart bug-fix pass** (2026-05): three more issues from
  visual review of the prior fix.
  - *Cohesive area-fill polygon*: stride=1 (every interpolated point is
    a polygon vertex) so the top edge of the fill is exactly the
    Catmull-Rom line, not a stepped chord skipping points. Per-triangle
    gradient stops are sampled from a single global ramp `opa_at(y) =
    OPA_TOP * (bot - y) / bot`, so the local gradient inside each
    trapezoid matches its neighbour's at the seam — no vertical
    stripes or bands.
  - *Marker label clamp + top-band rule*: right edge clamped to
    `plot_w - 2`. If the last data point's `tip.y` lands in the top
    `MARKER_TOP_BAND_PCT` (=15%) of the plot area, the label drops
    BELOW the dot instead of going next to it — keeps it clear of the
    top Y-tick label.
  - *Y labels as chart siblings*: chart `pad_left` was unreliable
    because chart-CHILD positions are relative to the content area,
    which `pad_left` also shifts — labels ended up inside the plot.
    Y and X labels are now children of the **card**, positioned in
    card-local coords. The chart itself is sized to `(plot_w =
    CHART_W - gutter)` × `(plot_h = CHART_H - pad_btm)` and pushed
    right by `gutter`, so the label strips live entirely outside its
    bounding box. The marker stays a chart child since it consumes
    `lv_chart_get_point_pos_by_id` coords directly.
- **Chart polish bug-fix pass** (2026-05): single follow-up commit fixing
  six visible issues from the four polish commits below.
  - *Nice-step Y ticks*: `pick_step()` picks from {0.5,1,2,5,10,20,25,50,
    100,200,500,1000,2000,5000} so range/step ≤ 5, then snaps min/max
    outwards to multiples of the step. Pool of 8 hidden Y labels reused
    per render. AAPL now shows 195/190/185/180 instead of raw
    893.6/885.8/877.9.
  - *Per-point X-axis timestamps*: `History` gained `std::vector<time_t>
    timestamps`; the fetcher captures `points[].ts` and `fake_data`
    populates daily-spaced epochs in the sim. Three X ticks at native
    indices [0, n/3, 2n/3] each read their own epoch so dates differ.
    Rightmost (n-1) tick is intentionally omitted — the current-price
    marker always lands there and a tick label would collide.
  - *Gradient area fill*: LVGL 9 line charts ignore `LV_PART_ITEMS`
    bg-gradient styles (that part is for bar charts). Fallback is a
    `LV_EVENT_DRAW_MAIN_BEGIN` callback that draws ~50 vertical-gradient
    trapezoids (split into pairs of triangles via `lv_draw_triangle`,
    `dsc.grad.dir = LV_GRAD_DIR_VER`) under the line. Point positions
    are precomputed in `render_history` (`g_fill_x[]`/`g_fill_y[]`) so
    the draw callback never calls `lv_chart_get_point_pos_by_id` —
    doing so during DRAW_MAIN_BEGIN tripped a heap-corruption guard on
    Windows builds.
  - *Y/X label gutters*: chart gets dynamic `pad_left = widest_y_label
    + 4` and `pad_bottom = x_label_height + 2` so Y labels live in a
    clean left strip and X labels in a bottom strip — neither overlaps
    the plot, and the bottom Y label no longer collides with the
    bottom-left X label.
- **Detail chart polish — four commits** (2026-05):
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
  4. *Current-price marker*: 8 px filled circle (`lv_obj`, `LV_RADIUS_CIRCLE`)
     + price label, both as chart children. Positioned via
     `lv_chart_get_point_pos_by_id(g_chart, g_ser, out_n-1, &tip)`. Must call
     `lv_obj_update_layout(g_chart)` before the lookup (the chart's content
     width is otherwise stale) AND un-hide the label before reading its
     size. Dot is clamped inside `[0, CHART_W-8] × [0, CHART_H-8]` so the
     right-edge "latest point" case doesn't render a half-circle clipped
     against the chart border. Label sits to the left of the dot by
     default, flips to the right only when there isn't room on the left.
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

- Long-press a row to mark it as a "favorite" / pin to top.
- Switch to hourly bars (`interval=1h`) on a long-press of the chart.
- Per-symbol price-alert thresholds in the settings form.
- Greek month abbreviations: need Montserrat 12/14 rebuilt with Greek Unicode
  range (U+0370–U+03FF) via the LVGL font converter. Flash at 94% — tight.

Don't pre-build any of these unless asked — list them so the next session
has a head start.
