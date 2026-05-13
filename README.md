# CYD Stock Ticker

Stock ticker firmware for the ESP32-2432S028R "Cheap Yellow Display".

- **Stack**: PlatformIO + Arduino-ESP32, LovyanGFX, LVGL 9.x, ArduinoJson v7, ESPAsyncWebServer.
- **Tasks**: LVGL UI pinned to core 1, networking (WiFi + HTTPS + web admin) on core 0. UI never blocks on the network.
- **Display**: ILI9341 240×320, landscape (320×240). Touch via XPT2046 on a separate SPI bus.
- **Data source**: RapidAPI `yahoo-finance15.p.rapidapi.com`.

## UI tour

- **List screen** — one row per symbol with the company logo (or a brand-colored letter badge), a 10-point sparkline, current price, and percent change with a colored up/down arrow. Auto-scrolls every 4 s when the list overflows.
- **Detail screen** — tap any row. 48 px logo, large price, percent change, area-filled trend chart (last 30 daily closes by default), plus LOW / RANGE / HIGH stats. Tap anywhere to go back.
- **Status bar** — wifi indicator (green + RSSI dBm when connected, red "no link" otherwise), section title, last-update timestamp, and a gear icon on the right that opens the settings info screen.
- **Settings info screen** — tap the gear. Read-only view of network state (SSID, IP, signal), refresh interval, symbols, and API-key status, plus the `http://<ip>/` URL to open the editable web admin. Tap anywhere to go back.

## Build & flash

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or CLI).
2. Copy `src/secrets_example.h` to `src/secrets.h` and fill in `WIFI_SSID`, `WIFI_PASS`, and `RAPID_KEY_SEED`. `src/secrets.h` is gitignored.
3. `pio run -t upload` to build & flash. `pio device monitor` to watch logs.
4. (Optional) drop PNG logos into `data/logos/<SYMBOL>.png` and run `pio run -t uploadfs` to flash the filesystem. Symbols without a PNG fall back to a brand-colored badge automatically; this folder can stay empty.

The default partition is `min_spiffs.csv` (1.9 MB app, OTA-capable, ~190 KB
LittleFS). LittleFS holds the runtime settings (`/settings.json`) and the
optional `/logos/` directory.

## First-boot key seeding

`RAPID_KEY_SEED` in `secrets.h` is only used when no `/settings.json` exists in
LittleFS. After first boot, the runtime key in LittleFS takes over and is
managed via the `/settings` web UI. Once the device is provisioned you can
blank `RAPID_KEY_SEED = ""` and reflash to produce a key-free image.

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

Default credentials: `admin / admin` (change them in the form on first visit).

The form lets you change:

- RapidAPI key (leave blank to keep current)
- Symbols (comma-separated, e.g. `AAPL,MSFT,NVDA`)
- Refresh interval in seconds (minimum 15)
- Admin username / password

Changes persist to `/settings.json` and apply on the next refresh cycle — no
reboot required.

## Adding / removing tickers

Use the web UI: `Symbols` field, comma-separated, e.g.
`AAPL,MSFT,NVDA,TSLA,GOOG,AMZN`. Save. Next refresh picks up the new list.

Brand colors for badges are baked into `src/ui/styles.cpp`
(`kBrandTable` — AAPL, MSFT, NVDA, TSLA, GOOG, AMZN, META, NFLX, AMD, INTC,
IBM, …). Unknown symbols get a stable color from an FNV hash, so a new ticker
still looks coherent without code changes.

## Logos

The list and detail screens try to load `L:/logos/<SYMBOL>.png` from LittleFS
(LVGL FS driver registered in `src/display/fs_littlefs.cpp`, decoded via
`LV_USE_LODEPNG`). When a PNG is missing the widget draws a circular badge in
the symbol's brand color with the first 1-2 letters.

Place PNGs in `data/logos/` (uppercase filename, e.g. `AAPL.png`).
Recommended size: 48×48 or 64×64, transparent background, < ~4 KB each.
Upload with `pio run -t uploadfs`.

## RapidAPI endpoint

Both list and detail views use the single endpoint:

```
/api/v2/markets/stock/history
```

- **List**: per symbol, `?symbol=X&interval=1d&limit=10`. The last 10 closes
  populate the sparkline; the last two derive `last` and `changePct`.
- **Detail**: `?symbol=X&interval=1d&limit=30` for the 30-bar trend chart.

The HTTP client sets `useHTTP10(true)` and `Accept-Encoding: identity` to
avoid chunked transfer and gzip — both have bitten this endpoint in prior
deployments. The JSON parser uses a `close`-only filter and accepts three
known response shapes (`body[]`, `data.items[]`, `data.prices[]`) and both
numeric and string close values.

To switch to hourly bars (if your RapidAPI subscription supports it), edit
the `interval=1d` queries in `src/net/quote_fetcher.cpp` to `interval=1h`.

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
- **Touch off-center**: tweak `x_min/x_max/y_min/y_max` in `lgfx_cyd.hpp`.
- **HTTP errors**: check serial for `HTTP <code>`. 401 = bad/missing API key;
  404 = wrong endpoint path (see above); 429 = rate limited.
- **`/settings` not reachable**: the IP is printed at boot and on the settings
  info screen on-device. The device must be on the same LAN as your browser.
- **Logos not showing**: confirm `data/logos/<SYMBOL>.png` exists, then run
  `pio run -t uploadfs`. The fallback badge always renders, so a blank/empty
  `data/logos/` is a valid configuration.
- **Out-of-memory at link time after adding fonts/widgets**: the LVGL flush
  buffer (`LINES` in `src/display/lvgl_bridge.cpp`) and `LV_MEM_SIZE` in
  `include/lv_conf.h` are the two main DRAM levers.
