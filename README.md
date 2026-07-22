# CYD Stock Ticker

Stock ticker firmware for the ESP32-2432S028R "Cheap Yellow Display".

Architecture overview: [`docs/firmware-architecture.html`](docs/firmware-architecture.html).

- **Stack**: PlatformIO + Arduino-ESP32, LovyanGFX, LVGL 9.x, ArduinoJson v7, ESPAsyncWebServer.
- **Tasks**: LVGL UI pinned to core 1, networking (WiFi + HTTPS + web admin) on core 0. UI never blocks on the network.
- **Display**: ST7789 240×320, landscape (320×240), display inversion OFF (`invert = false`). Touch via XPT2046 on a separate SPI bus. Targets the dual-USB (USB-C + micro-USB) ESP32-2432S028R revision; the original single-micro-USB rev ships with an ILI9341 — see `src/display/lgfx_cyd.hpp` if you're on that one.
- **Data source**: self-hosted yfinance proxy at `https://rozakos.eu/stocks/api/v1` (bearer-token auth). Repo: [Rozakos/stock-api](https://github.com/Rozakos/stock-api).

## UI tour

- **List screen** — one row per symbol with the company logo (or a brand-colored letter badge), a 10-point sparkline, current price, and percent change with a colored up/down arrow. During pre/after-market the row headlines the extended-hours print and its change vs the regular close (Revolut-style). Auto-scrolls every 4 s when the list overflows. Outside regular hours the whole screen shifts to a purple-tinted night palette.
- **Detail screen** — tap any row. Logo, large price (live, extended-hours-aware), percent change, and an area-filled trend chart with Y-axis price ticks, X-axis time/date ticks, and a current-price marker dot. A range selector (1D / 1W / 1M / 6M / 1Y / 5Y / Max) sits above the chart; **1D is the default and renders progressively** — the X axis spans the whole **extended-hours window (04:00–20:00 ET)** with faint divider lines at the regular open/close, its four time ticks snapped to the window edges and those dividers, and the line fills only the elapsed part of the day. Before the regular session opens this shows the live pre-market move; without the extended window (older server) it falls back to the regular session alone. 1D colors by day change vs the previous close (a gap-down day that climbs off the open still reads red); other ranges color first-to-last of the window. The displayed range silently re-fetches whenever a quote refresh lands, so the chart tracks the live session. A **back button** at the top-right returns to the list; tapping the chart does nothing.
- **Status bar** — wifi indicator (green glyph when connected, red "no link" otherwise; tap it to forget the network and reboot into WiFi setup); a session-aware center title (PREMARKET / MARKET OPEN / AFTER HOURS / MARKET CLOSED from the API's `market_state`, with an amber sun disc while open and a crescent moon otherwise; static "MARKETS" fallback when the API omits the field); the right slot shows the portfolio total with day P/L when share quantities are configured (green/red, extended-hours-aware vs previous close), an amber "stale Ns" warning when no refresh lands for two intervals (+5 s), or the last-update HH:MM:SS clock otherwise; and a gear icon that opens the settings screen.
- **Settings screen** — tap the gear. Read-only view of network state (SSID, IP, signal), refresh interval, symbols, and API-key status, plus the `http://<ip>/` URL to open the editable web admin. A **back button** at the top-right returns to the list.
- **WiFi setup screen** — shown on first boot (or when no saved network connects): a QR code to join the device's setup AP plus a captive-portal page to enter credentials. Live status (Connecting… / Connected / failure reason) is shown on-device so no serial console is needed. Unprovisioned boots also advertise a BLE GATT provisioning service (`CYD-Ticker-XXXX`) for the Rozakos Home app — see `docs/ble-provisioning-protocol.md`. Once provisioning succeeds the device reboots into ticker mode; provisioned boots never start BLE (NimBLE's RAM footprint starves the TLS client — see AGENTS.md) and are discoverable over mDNS instead (`cyd-ticker-xxxx.local`, `_rozakos._tcp`, plus `GET /api/device-info`).

## Build & flash

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Copy `src/secrets_example.h` to `src/secrets.h` and fill in `WIFI_SSID`, `WIFI_PASS`, and `API_TOKEN_SEED` (bearer token for the stock-api). `src/secrets.h` is gitignored.
3. `pio run -e cyd -t upload` to build & flash. `pio device monitor` to watch logs.

The partition table is `huge_app.csv` (3 MB app, no OTA, ~896 KB LittleFS) —
the NimBLE stack pushed the image past min_spiffs' 1.9 MB app slot. LittleFS
holds the runtime settings (`/settings.json`) and the runtime-fetched
`/logos/` cache. Keep `data/` near-empty so `uploadfs` does not consume the
filesystem before the device can cache logos. NOTE: switching partition
tables requires a full flash erase on the next upload — cached logos and
`/settings.json` are wiped and regenerate on first boot.

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

The page is rendered as a chunked HTTP response fed straight into AsyncTCP's
buffer, so serving it needs no large contiguous allocation and works no matter
how fragmented the heap is. `write_settings_page` is a linear, non-resumable
writer, so each chunk re-runs it from the top and discards the bytes already
sent — O(n²) in page size, but that's a handful of string-literal re-renders
for a ~9 KB page and it costs zero heap. Keep the writer free of side effects
(it runs several times per request) and prefer `Writer&` `print()` calls over
building intermediate `String`s.

The form lets you change:

- Refresh interval in seconds (minimum 15)
- API bearer token
- Symbols through an add/delete table
- Shares owned per symbol (the table's Shares column; 0 or blank clears the
  holding) — drives the status bar's portfolio total + day P/L readout

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
an in-memory ARGB8888 image with LVGL's bundled lodepng — the same render path
the embedded logos use (LVGL's file-backed PNG path mis-draws on this panel). When no logo
is available the widget draws a circular badge in the symbol's brand color
with the first 1-2 letters.

Downloads are hardened: a logo is cached only if the PNG is complete (ends with
the `IEND` marker), so a truncated transfer can't wedge a symbol on a black
icon, and an incomplete or wrong-size cache file is re-fetched. Missing logos
retry every refresh cycle.

Each runtime logo is held decoded in RAM (~6.4 KB at 40×40 ARGB), so to avoid
heap exhaustion (and the reboots it caused) the firmware caps resident runtime
logos at `MAX_RUNTIME_LOGOS` (4) and skips a logo download when the largest
free heap block is too small. Symbols past the cap show a letter badge (which
costs no heap). For symbols you always want a real logo for, embed them at
compile time (see below) — embedded logos live in flash and don't count against
the RAM cap.

Cached logos are decoded **at boot** (`logos::prewarmRuntimeCache`), before
the first TLS connection — a decode needs ~45 KB of contiguous heap
transiently, which only reliably exists in the pristine boot-time heap. A
symbol added mid-run downloads its PNG on the next refresh but shows a letter
badge until the next reboot prewarms it.

To add or refresh an embedded logo, put its source PNG in `sim/logo_src/` and
run `python sim/build_logo_arrays.py`. Embedded logos increase firmware flash
usage, so the bundled set is intentionally limited to AMD.

## stock-api endpoints

Base URL: `https://rozakos.eu/stocks/api/v1`. Both calls send
`Authorization: Bearer <token>` and a non-empty `User-Agent`
(`CYD-Stock-Ticker/1.0 (ESP32)`) — Cloudflare bot-fight blocks empty UAs.

- **List**: `GET /stocks?symbols=AMD,NVDA,...` in batches of up to 16 symbols.
  Response `quotes[]` entries include `symbol`, pre-computed `last` and
  `change_pct`, plus a `closes[]` array used directly for the row sparkline.
  Extended-hours fields: `market_state` (`PRE`|`REGULAR`|`POST`|`CLOSED`)
  drives the status-bar session title and night palette;
  `pre_market`/`pre_market_change_pct` and `post_market`/
  `post_market_change_pct` are numeric only during their session and feed the
  rows' extended-hours prices (change measured vs the regular close). Results
  are matched by symbol, so omitted or reordered entries do not shift data
  onto the wrong row. Server-side cached 10 min.
- **Detail**: `GET /history/{symbol}?range=<range>&limit=<n>` for the trend
  chart. `range` ∈ {`1d`, `1w`, `1mo`, `6mo`, `1y`, `5y`, `max`}. Response
  carries `interval` (`intraday`|`daily`) and `points[]` of `{ts, last}`
  (epoch seconds UTC); the firmware downsamples to `HISTORY_POINTS`,
  preserving the first/last point so the displayed % change matches the
  window. `range=1d` is sent with `&prepost=1`, which widens the response to
  the 04:00–20:00 ET window and adds `window_open`/`window_close` (epoch s,
  the X-axis bounds), `prev_close` (day-change baseline for the header) and
  `market_state`. `session_open`/`session_close` (epoch s) remain the regular
  open/close and become the divider lines inside that window. Without the
  window fields the chart spans the regular session alone; without either the
  firmware assumes a 6.5 h session. Sending `prepost=1` matters beyond the
  wider axis: before the regular session opens there is no REGULAR data for
  today, so the plain call returns **yesterday's** session instead. The
  server must apply `limit` for **all** ranges (notably `max`) so the payload
  stays small enough for the ESP32 to parse. Requires server Postgres history
  (`DATABASE_URL`); otherwise 503.

The HTTP client keeps one HTTP/1.1 TLS connection alive across quote, logo, and
history requests. It sends `Accept-Encoding: identity`, consumes exact
`Content-Length` bodies where required, and reconnects once if a reused socket
is stale. TLS verifies the API server against the pinned GTS WE1 intermediate
in `src/net/tls_ca_cert.h`; `cfg::API_TLS_VERIFY` defaults to `true`. JSON
parsing uses an ArduinoJson `Filter` so streamed JSON bodies never land fully
in RAM.

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

  Certificate failures usually mean the device clock did not synchronize or
  the API's certificate chain changed. Update `src/net/tls_ca_cert.h` rather
  than disabling verification; `cfg::API_TLS_VERIFY = false` is intended only
  for temporary diagnosis.

  Smoke-test from any machine to isolate device vs server:
  ```
  curl -H "Authorization: Bearer <token>" \
       -H "User-Agent: stock-ticker/1.0" \
       'https://rozakos.eu/stocks/api/v1/stocks?symbols=AMD'
  ```
- **`/settings` not reachable**: the IP is printed at boot and on the settings
  info screen on-device. The device must be on the same LAN as your browser.
  The page is streamed to the client in chunks, so it no longer needs a large
  contiguous buffer and no longer returns `503 busy` on a fragmented heap.
  If the device stops answering entirely while WiFi
  is up, the net task's transport watchdog reboots it automatically after
  ~5 fetch cycles with no HTTP responses (lwIP can wedge its TCP stack after
  a burst of connections; a reboot is the only reliable unwedge).
- **Logos not showing**: check serial for `[logo]` download/cache/decode logs
  and confirm LittleFS has free space. The fallback badge always renders. A
  `runtime decode deferred (maxalloc=...)` line means the contiguous-heap gate
  blocked a mid-run decode — expected for symbols added since the last boot;
  a reboot prewarms them from the cache.
- **`SSL - Memory allocation failed` on every fetch**: the heap has no
  contiguous block for the TLS buffers. Check the per-cycle
  `[heap] pre-fetch free=... largest=...` serial line; `largest` must stay
  well above ~17 KB. If this appears on an unprovisioned boot it's expected —
  BLE provisioning owns the RAM until setup completes and the device reboots.
- **Out-of-memory at link time after adding fonts/widgets**: the LVGL flush
  buffer (`LINES` in `src/display/lvgl_bridge.cpp`) and `LV_MEM_SIZE` in
  `include/lv_conf.h` are the two main DRAM levers.
