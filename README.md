# CYD Stock Ticker

Stock ticker firmware for the ESP32-2432S028R "Cheap Yellow Display".

Architecture overview: [`docs/firmware-architecture.html`](docs/firmware-architecture.html).

- **Stack**: PlatformIO + Arduino-ESP32, LovyanGFX, LVGL 9.x, ArduinoJson v7, ESPAsyncWebServer.
- **Tasks**: LVGL UI pinned to core 1, networking (WiFi + HTTPS + web admin) on core 0. UI never blocks on the network.
- **Display**: ST7789 240×320, landscape (320×240), display inversion OFF (`invert = false`). Touch via XPT2046 on a separate SPI bus. Targets the dual-USB (USB-C + micro-USB) ESP32-2432S028R revision; the original single-micro-USB rev ships with an ILI9341 — see `src/display/lgfx_cyd.hpp` if you're on that one.
- **Data source**: self-hosted yfinance proxy at `https://rozakos.eu/stocks/api/v1` (bearer-token auth). Repo: [Rozakos/stock-api](https://github.com/Rozakos/stock-api).

## UI tour

- **List screen** — one row per symbol with the company logo (or a brand-colored letter badge), a 10-point sparkline, current price, and percent change with a colored up/down arrow. Auto-scrolls every 4 s when the list overflows.
- **Detail screen** — tap any row. 48 px logo, large price, percent change, and an area-filled trend chart with Y-axis price ticks, X-axis time/date ticks, and a current-price marker dot. A range selector (1D / 1W / 1M / 6M / 1Y / 5Y / Max) sits above the chart; **1D is the default and renders progressively** — the X axis spans the whole trading session and the line fills only the elapsed part of the day (Revolut-style). A **back button** at the top-right returns to the list; tapping the chart does nothing.
- **Status bar** — wifi indicator (green glyph when connected, red "no link" otherwise; tap it to forget the network and reboot into WiFi setup), section title, last-update timestamp, and a gear icon on the right that opens the settings screen.
- **Settings screen** — tap the gear. Read-only view of network state (SSID, IP, signal), refresh interval, symbols, and API-key status, plus the `http://<ip>/` URL to open the editable web admin. A **back button** at the top-right returns to the list.
- **WiFi setup screen** — shown on first boot (or when no saved network connects): a QR code to join the device's setup AP plus a captive-portal page to enter credentials. Live status (Connecting… / Connected / failure reason) is shown on-device so no serial console is needed.

## Build & flash

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Copy `src/secrets_example.h` to `src/secrets.h` and fill in `WIFI_SSID`, `WIFI_PASS`, and `API_TOKEN_SEED` (bearer token for the stock-api). `src/secrets.h` is gitignored.
3. `pio run -t upload` to build & flash. `pio device monitor` to watch logs.
4. (Optional) drop PNG logos into `data/logos/<SYMBOL>.png` and run `pio run -t uploadfs` to flash the filesystem. Symbols without a PNG fall back to a brand-colored badge automatically; this folder can stay empty.

The default partition is `min_spiffs.csv` (1.9 MB app, OTA-capable, ~190 KB
LittleFS). LittleFS holds the runtime settings (`/settings.json`) and the
optional `/logos/` directory.

## First-boot token seeding

`API_TOKEN_SEED` in `secrets.h` is only used when no `/settings.json` exists in
LittleFS. After first boot, the runtime token in LittleFS takes over and is
managed via the `/settings` web UI. Once the device is provisioned you can
blank `API_TOKEN_SEED = ""` and reflash to produce a token-free image.

If you want to re-seed, erase the filesystem:

```
pio run -t erase
pio run -t upload
```

## Web admin

After boot, the serial monitor (and the on-device settings screen) prints the
device IP. Open:

```
http://<device-ip>/settings
```

No login is required; `/settings` opens directly on the LAN. The page uses the
Rozakos Industries dark theme with the robot mark, wordmark, and footer.

The form lets you change:

- Refresh interval in seconds (minimum 15)
- API bearer token
- Symbols through an add/delete table

Changes persist to `/settings.json` and apply on the next refresh cycle — no
reboot required.

To preview the web admin without flashing, build the sim and run
`cyd_sim.exe --web-settings=settings_web.html` from `sim/build/`, then open the
generated HTML in a browser.

## Adding / removing tickers

Use the web UI: add a ticker in the `Add symbol` form, or delete it from the
symbols table. Next refresh picks up the new list.

Brand colors for badges are baked into `src/ui/styles.cpp`
(`kBrandTable` — AAPL, MSFT, NVDA, TSLA, GOOG, AMZN, META, NFLX, AMD, INTC,
IBM, …). Unknown symbols get a stable color from an FNV hash, so a new ticker
still looks coherent without code changes.

## Logos

AMD is compiled in as an ARGB8888 array (lives in flash, costs no heap).
For everything else the firmware fetches `GET /logo/{symbol}?size=48` on
demand, caches the PNG in LittleFS (`/logos/<SYMBOL>.png`), and decodes it to
an in-memory ARGB8888 image with pngle — the same render path the embedded
logos use (LVGL's file-backed PNG path mis-draws on this panel). When no logo
is available the widget draws a circular badge in the symbol's brand color
with the first 1-2 letters.

Downloads are hardened: a logo is cached only if the PNG is complete (ends with
the `IEND` marker), so a truncated transfer can't wedge a symbol on a black
icon, and an incomplete or wrong-size cache file is re-fetched. Missing logos
retry every refresh cycle.

Each runtime logo is held decoded in RAM (~9 KB at 48×48 ARGB), so to avoid
heap exhaustion (and the reboots it caused) the firmware caps resident runtime
logos at `MAX_RUNTIME_LOGOS` (6) and skips a logo download when the largest
free heap block is too small. Symbols past the cap show a letter badge (which
costs no heap). For symbols you always want a real logo for, embed them at
compile time (see below) — embedded logos live in flash and don't count against
the RAM cap.

You can also pre-seed logos by dropping PNGs in `data/logos/` (uppercase
filename, e.g. `AAPL.png`, 48×48 transparent) and running `pio run -t uploadfs`.

## stock-api endpoints

Base URL: `https://rozakos.eu/stocks/api/v1`. Both calls send
`Authorization: Bearer <token>` and a non-empty `User-Agent`
(`CYD-Stock-Ticker/1.0 (ESP32)`) — Cloudflare bot-fight blocks empty UAs.

- **List**: `GET /stock/{symbol}` per symbol. Response includes pre-computed
  `last` and `change_pct` plus a `closes[]` array (up to 5 daily closes,
  oldest→newest) used directly for the row sparkline. Server-side cached
  10 min.
- **Detail**: `GET /history/{symbol}?range=<range>&limit=<n>` for the trend
  chart. `range` ∈ {`1d`, `1w`, `1mo`, `6mo`, `1y`, `5y`, `max`}. Response
  carries `interval` (`intraday`|`daily`) and `points[]` of `{ts, last}`
  (epoch seconds UTC); the firmware downsamples to `HISTORY_POINTS`,
  preserving the first/last point so the displayed % change matches the
  window. For `range=1d` the response should also include
  `session_open`/`session_close` (epoch s) so the chart spans the full
  trading session; without them the firmware assumes a 6.5 h session. The
  server must apply `limit` for **all** ranges (notably `max`) so the payload
  stays small enough for the ESP32 to parse. Requires server Postgres history
  (`DATABASE_URL`); otherwise 503.

The HTTP client uses `useHTTP10(true)` + `Accept-Encoding: identity` to
avoid chunked transfer and gzip — workarounds carried over from the
previous backend. JSON parsing uses an ArduinoJson `Filter` so the
streamed body never lands fully in RAM.

## Hardware pinout (already wired in firmware)

| Function | Pin |
| -------- | --- |
| TFT MOSI | 13 |
| TFT MISO | 12 |
| TFT SCLK | 14 |
| TFT CS   | 15 |
| TFT DC   | 2  |
| TFT BL   | 21 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch SCLK | 25 |
| Touch CS   | 33 |
| Touch IRQ  | 36 |

LDR (GPIO 34) and RGB LED (GPIO 4/16/17) are not yet used.

## Troubleshooting

- **White screen, no draw**: confirm `board_build.partitions = min_spiffs.csv`
  is in effect and the build didn't run out of IRAM. Lower the SPI clock in
  `src/display/lgfx_cyd.hpp` from `40000000` to `27000000` if you see tearing.
- **Screen is scrambled / "noise" / wrong colors**: you likely have the
  wrong panel driver/inversion for your CYD revision. This dual-USB board uses
  ST7789 with `invert = false` (setting `invert = true` tints green→pink and
  red→cyan); the original single-micro-USB uses `Panel_ILI9341`. Swap the
  class and the `invert` flag in
  `src/display/lgfx_cyd.hpp`. Also confirm `flush_cb` in
  `src/display/lvgl_bridge.cpp` is calling `writePixels(..., true)` — the
  third arg byte-swaps RGB565 for MSB-first SPI panels (needed when
  `LV_COLOR_16_SWAP = 0` in `include/lv_conf.h`).
- **Touch off-center / mirrored**: this panel runs landscape
  (`setRotation(1)`) while the touch stays `offset_rotation=0`, so the touch's
  raw axes are rotated 90° vs the screen — `cfg.x_min/x_max` controls
  screen-**vertical** and `cfg.y_min/y_max` controls screen-**horizontal**. To
  flip left/right reverse the Y ends; to flip up/down reverse the X ends.
- **HTTP errors**: check serial for `HTTP <code> <url>`.

  | Code | Meaning |
  | ---- | ------- |
  | 400  | Symbol not in the NASDAQ+NYSE allowlist (refreshed daily server-side). Add it to `EXTRA_SYMBOLS` on the server if it's a valid ticker the universe doesn't carry (crypto, indices). |
  | 401  | Bad or missing bearer token. Rotate it via `/settings`. |
  | 403  | Most likely the User-Agent (Cloudflare bot-fight). `cfg::API_USER_AGENT` must be set before `http.GET()` — already wired in `quote_fetcher.cpp`. |
  | 502  | Yahoo upstream failed and the server had no cached fallback for this symbol. Usually transient; the next refresh works. |
  | 503  | `/history` is unreachable because the server's `DATABASE_URL` isn't configured. List screen still works. |

  Smoke-test from any machine to isolate device vs server:
  ```
  curl -H "Authorization: Bearer <token>" \
       -H "User-Agent: stock-ticker/1.0" \
       https://rozakos.eu/stocks/api/v1/stock/AMD
  ```
- **`/settings` not reachable**: the IP is printed at boot and on the settings
  info screen on-device. The device must be on the same LAN as your browser.
- **Logos not showing**: confirm `data/logos/<SYMBOL>.png` exists, then run
  `pio run -t uploadfs`. The fallback badge always renders, so a blank/empty
  `data/logos/` is a valid configuration.
- **Out-of-memory at link time after adding fonts/widgets**: the LVGL flush
  buffer (`LINES` in `src/display/lvgl_bridge.cpp`) and `LV_MEM_SIZE` in
  `include/lv_conf.h` are the two main DRAM levers.
