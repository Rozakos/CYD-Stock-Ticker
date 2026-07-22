# Project brief: CYD Social Follower Tracker

I want to build firmware for an **ESP32-2432S028R "Cheap Yellow Display" (CYD)** —
a 320×240 touchscreen — that shows **subscriber / follower counts** for accounts
across **YouTube, Instagram, TikTok, Twitch, and X/Twitter**. Think of it as a
desk-top "social stats ticker": a list of accounts I follow (mine + others), each
with its platform icon, current follower count, and how it's changed; tap one to
see a history chart of follower growth over time.

This is a brand-new project, but I previously built a **stock ticker** on the exact
same hardware and the architecture worked extremely well. Please reuse that proven
architecture and just swap the domain (stock quotes → social follower counts).
Everything below describes that architecture so you don't have to rediscover it.

**Key requirement: platforms are NOT hardcoded.** I want to add new platforms of my
choosing at runtime — through the web UI (and ideally a phone app later) — WITHOUT
reflashing the firmware. So the firmware must be **platform-agnostic**: to it, a
"platform" is just a string ID plus some display metadata (name, brand color, icon)
that it learns from the server. Adding a new platform should be a server-side change
that simply *appears* in the device's platform picker. YouTube/Instagram/TikTok/
Twitch/X are just the first platforms I'll configure — not a closed set baked into
the binary.

---

## Hardware

- Board: **ESP32-2432S028R**, the **dual-USB revision** (USB-C + micro-USB).
  This revision uses an **ST7789** 240×320 panel (landscape = 320×240). The older
  single-micro-USB rev uses ILI9341 — design for ST7789 but leave the panel class
  easy to swap.
- Touch: **XPT2046** on a separate SPI bus.
- Pinout (already standard for this board):
  | Function | Pin | | Function | Pin |
  |---|---|---|---|---|
  | TFT MOSI | 13 | | Touch MOSI | 32 |
  | TFT MISO | 12 | | Touch MISO | 39 |
  | TFT SCLK | 14 | | Touch SCLK | 25 |
  | TFT CS | 15 | | Touch CS | 33 |
  | TFT DC | 2 | | Touch IRQ | 36 |
  | TFT BL | 21 | | | |

## Stack

- **PlatformIO**, board `esp32dev`, **Arduino-ESP32** (espressif32 @ ^6.7.0).
- **LovyanGFX** ^1.2.0 (ST7789 + XPT2046).
- **LVGL** ^9.2 (resolves to 9.5). Config in `include/lv_conf.h`, with
  `LV_CONF_INCLUDE_SIMPLE` + `-I include` so that file is the single source of truth.
- **ArduinoJson** ^7.2 with streaming field filters (so response bodies never fully
  land in RAM).
- **ESPAsyncWebServer** + **AsyncTCP** for the on-device `/settings` web admin.
- Partitions: `min_spiffs.csv`, filesystem **LittleFS** (~192 KB — tiny, treat it as
  precious; see memory notes).
- `monitor_speed = 115200`, `upload_speed = 921600`.

## Architecture invariants (carry these over verbatim — they're hard-won)

- **Two FreeRTOS tasks, pinned**: `uiTask` on core 1 (LVGL only), `netTask` on
  core 0 (WiFi + HTTPS + web admin). The UI must NEVER block on the network. They
  share a data store + settings store behind a mutex. LVGL is single-threaded behind
  its own mutex.
- **LVGL OS**: set `LV_USE_OS = LV_OS_NONE`. `LV_OS_FREERTOS` pulls in `lv_freertos.c`
  which `#include "atomic.h"` — missing on ESP-IDF. (Footgun.)
- **Display color**: `cfg.invert = false` on the ST7789, AND pass `swap = true` to
  `writePixels(...)` in the LVGL flush callback because `LV_COLOR_16_SWAP = 0`
  (LVGL writes little-endian RGB565; the panel wants big-endian). Setting
  `invert = true` XORs every pixel → green becomes pink, red becomes cyan. Do NOT
  set it even if a guide tells you to.
- **HTTPS**: `WiFiClientSecure::setInsecure()` + `http.useHTTP10(true)` +
  `Accept-Encoding: identity`. These avoid chunked-transfer / gzip breakage seen in
  production. Don't "clean them up."
- **Memory budget is the real constraint.** DRAM is tight after WiFi+TLS — largest
  contiguous block can drop to ~40 KB during a TLS handshake. Two main levers: the
  LVGL flush buffer height (`LINES`, ~16 lines of 320px) and `LV_MEM_SIZE`
  (~32 KB on device). Wrap `LV_MEM_SIZE` in `#ifndef` so the simulator can override
  it. Fonts cost flash, not DRAM. Keep any embedded image assets to an absolute
  minimum — prefer fetching at runtime.
- **Touch_XPT2046 has no `setBus()`** in this LovyanGFX version; SPI pins go directly
  into the touch config struct.
- Any header that uses Arduino `String` must `#include <Arduino.h>` itself.

---

## The data problem (this is the most important design decision)

Stock data came from a **self-hosted proxy API** (FastAPI on my server at
`rozakos.eu`, behind Cloudflare, bearer-token auth, required non-empty `User-Agent`).
The ESP32 never talked to the upstream data source directly — it only ever hit my
clean little JSON API. **Do the same here.** The ESP32 should call ONE self-hosted
API that returns normalized JSON; the messy per-platform auth lives on the server.

Reasons this matters even more for social platforms than for stocks:
- **YouTube** and **Twitch** have official APIs but need API keys / OAuth — server-side.
- **Instagram** and **TikTok** have no clean public follower-count API; the server
  may need the Graph API (for business accounts), scraping, or third-party services.
  All of that belongs on the server, not the microcontroller.
- Rate limits, caching, and key rotation are far easier server-side.

**Please design (but I'll implement separately) a small companion API**, mirroring my
stock-api shape. The server is the **single source of truth for which platforms
exist** so the device never needs reflashing to gain a new one. Propose endpoints
like:

- `GET /platforms` → `[ { "id": "youtube", "name": "YouTube",
  "color": "#FF0000", "icon_url": "/icon/youtube",
  "handle_hint": "channel ID or @handle" }, ... ]`. The device and web admin
  **discover the platform list from here** — this is what makes platforms
  add-able without a firmware change. Adding a platform server-side makes it show
  up in the picker automatically.
- `GET /social/{platform}/{handle}` → `{ "platform": "...", "handle": "...",
  "display_name": "...", "followers": 12345, "delta_24h": 87,
  "history": [ ints, oldest→newest ] }` for the list row (followers + small
  sparkline + change). Server-cached ~10 min.
- `GET /history/{platform}/{handle}?range=<token>` → ordered
  `{ "interval": "daily"|"hourly", "points": [ { "ts": epoch, "followers": int } ] }`
  for the detail chart. Range tokens (growth-first, matching the UI tabs): `1w`,
  `1mo`, `3mo`, `1y`, `max`. The **server** decides the window and returns ordered
  points; the firmware just renders what it gets and computes the displayed delta
  locally from first→last point.
- `GET /icon/{platform}` (or `/avatar/{platform}/{handle}`) → a small PNG
  (request `?size=48`), so account/platform icons are fetched at runtime instead of
  baked into flash. A brand-new platform's icon comes down the wire like any other.

All requests send `Authorization: Bearer <token>` + a non-empty `User-Agent`.
The bearer token is configurable at runtime via the web admin (see below). For now,
**stub/mock the API** so the firmware and simulator can be developed against fake
data; flag clearly anywhere a real endpoint is assumed. Treat `platform` as an opaque
string everywhere in the firmware — no enum, no `switch` on platform name, no
compile-time list.

### Launch set: YouTube + Twitch first

Build the server fetchers for **YouTube and Twitch only** to start — both have clean
official APIs (YouTube Data API v3 `channels.statistics.subscriberCount`; Twitch
Helix `channels`/`users` + followers endpoint, app-access OAuth token). Get the whole
pipeline working end-to-end (device → my API → these two) before anything else.
**Instagram, TikTok, X, Telegram, etc. come later**, fetched via scraping / third-party
sources behind the same `/social/...` contract. Because the firmware is platform-
agnostic and discovers platforms from `/platforms`, adding those later is a pure
server-side job — no firmware or web-admin change. So: make the API contract and the
`/platforms` mechanism solid now, implement just YouTube + Twitch behind it, and leave
the rest as documented stubs that return mock data.

---

## UI

Use the stock-ticker screens (list / detail / chart / status bar) as **structural
scaffolding**, but do NOT clone them 1:1 — followers behave differently from stock
prices, so the metrics each screen emphasizes must change. See "UI differences from a
stock ticker" below for the rationale; here is the target design:

- **List screen** — one row per tracked account: platform icon (or a brand-colored
  badge with the platform's initial as fallback), the handle / display name, a small
  follower-count sparkline (days/weeks, not minutes), the current follower count
  (formatted compactly: 1.2K / 3.4M), and — as the primary right-hand metric — the
  **absolute change over the chosen window** (e.g. `+1,240 this week`). A small
  colored up/down arrow is secondary (counts almost always rise, so % alone is
  uninformative). Auto-scroll when the list overflows. Long-press a row to pin it to
  the top; tap to open detail.
- **Detail screen** — tap a row: larger icon, big follower count, the selected range's
  **absolute delta** (`+5,310`) with growth rate as a subtle secondary line, an
  area-filled history chart (monotone-cubic / PCHIP smoothed so it doesn't wiggle),
  and a **growth-first** range button row (`1W / 1M / 3M / 1Y / All` —
  `lv_buttonmatrix`, one-checked, default `1M`). Drop the intraday `1D` tab (a day of
  follower data is a flat line). Replace the stock ticker's LOW/RANGE/HIGH stats with
  a **milestone block**: next round-number milestone, a progress bar/ring toward it
  (`92.1K → 100K`), and best-day / growth-rate stats. Tap anywhere to go back.
- **Hero / single-account mode** — a selectable full-screen view of ONE account
  (e.g. your own channel), since the device often sits on a desk showing a single
  number. Big centered follower count, today's / this-week's gain, the milestone
  progress ring, and a slim sparkline. Make it a mode the user can choose (e.g. set a
  "primary account" in the web admin, or long-press to promote a row to hero); the
  multi-account list remains the default. Build it as its own screen alongside list
  and detail.
- **Status bar** — WiFi indicator (green + RSSI dBm when connected, red otherwise),
  a title, last-update timestamp, and a gear icon opening a read-only settings info
  screen.
- **Settings info screen** — read-only: network state, refresh interval, tracked
  accounts, API-token status, and the `http://<ip>/settings` URL for the web admin.
- **Web admin** at `http://<device-ip>/settings` (no login, LAN only): edit refresh
  interval (min 15 s), API bearer token, and the **list of tracked accounts** via an
  add/delete table. The "add account" form has a **platform dropdown populated from
  `GET /platforms`** (not a hardcoded `<option>` list) plus a handle field. Persist
  accounts to `/settings.json` in LittleFS as `(platform, handle)` pairs; apply on
  next refresh, no reboot. Brand color for a row comes from the platform metadata the
  server provides (`color`), with a stable FNV-hash fallback for anything missing —
  so a new platform looks coherent the moment it's added, no firmware change. Also let
  the user pick a **primary account** here (drives the hero view) and the default list
  window for the `+N this week`-style delta.

## UI differences from a stock ticker (why we adapt, not clone)

The ticker's screens are tuned for fast, two-directional price movement; follower
counts behave differently, so the emphasis changes:

- **Counts move slowly and almost always go up.** A red/green % arrow — the loudest
  element on a stock row — is near-zero and almost always green here, i.e. the least
  informative thing on screen. Lead with **absolute delta over a window**
  (`+1,240 this week`); keep the arrow small and secondary.
- **The story is growth over time + milestones**, not the instantaneous number.
  Ranges are week/month/year scale; there is no useful intraday view, so drop `1D`.
- **Milestones matter** (100K / 1M "play button" culture). A progress ring toward the
  next round number replaces the stock LOW/RANGE/HIGH stats and is genuinely useful.
- **Fewer accounts, often one that matters most** (your own channel). Hence the
  full-screen hero mode — a stock ticker never needs that, a follower display does.
- **Refresh is minutes/hours, not seconds.** Keep the min refresh interval but expect
  the server's ~10-min cache to dominate; no need for aggressive polling.

## Platforms — data-driven, not hardcoded

An account is identified by `(platform, handle)` where `platform` is an opaque string.
The device knows nothing about specific platforms at compile time; it renders whatever
the `/platforms` endpoint advertises. **YouTube and Twitch ship first** (real server
fetchers); Instagram / TikTok / X / Telegram and others come later via scraping behind
the same contract. Adding any later platform must require **only** a server-side change
— it should then appear in the web admin's platform dropdown and render correctly on
the device with no reflash. Do not introduce a platform enum, a `switch (platform)`,
or a baked-in brand-color table in the firmware.

---

## WiFi onboarding (carry over)

WiFi creds stored in `/settings.json` (`wifi_ssid` / `wifi_pass`), with a
compile-time fallback in a gitignored `secrets.h`. On STA failure, open an AP named
`CYD-Setup-<MAC>`, show a fullscreen **WiFi QR code** + run a captive portal
(DNS hijack + AsyncWebServer at 192.168.4.1 with a network picker; POST `/save`
writes creds and retries STA). Wire the OS captive-probe URLs
(`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`).

## First-boot token seeding (carry over)

`API_TOKEN_SEED` in `secrets.h` is only used when no `/settings.json` exists; after
first boot the runtime token in LittleFS takes over via the web admin.

## Desktop simulator (please build this early — it's how you'll "see" the UI)

Add a `sim/` host build (CMake + MSYS2/MinGW + SDL2) that compiles the UI sources
against a thin Arduino shim and a software-rendered LVGL display. Modes:
`--window` (interactive), `--headless --out=foo.png` (dump a PNG so you can verify
UI changes without flashing), and `--web-settings=foo.html` (render the `/settings`
page to standalone HTML). Feed it fake follower data per range so charts/formatting
can be verified before flashing. Wrap `LV_MEM_SIZE` in `#ifndef` so the sim can use
a big heap while the device stays at 32 KB.

---

## Conventions

- C++17, two-space indent, `lower_snake` namespaces, `CamelCase` types, `g_` prefix
  for file-local globals. Comments only when the *why* is non-obvious.
- One explicit way per feature; don't add abstractions for hypothetical needs.
- LVGL: `lv_obj_remove_style_all(obj)` before layering named styles; set
  `LV_OBJ_FLAG_EVENT_BUBBLE` on children of clickable cards.
- Keep an `AGENTS.md` handoff file and a `README.md` current as you go.

## Suggested source layout (mirror the stock ticker)

```
src/
  main.cpp                 FreeRTOS tasks, mutex, LittleFS, LVGL init
  config.h                 SCREEN_W/H, HISTORY_POINTS, SPARKLINE_POINTS, API_BASE
  secrets.h / _example.h   gitignored seed token + fallback WiFi creds
  display/  lgfx_*.hpp, lvgl_bridge.*, fs_littlefs.*
  net/      account_store.*, social_fetcher.*, wifi_mgr.*, captive_portal.*, web_admin.*
  settings/ settings_store.*
  ui/       styles.*, icons.*, list_screen.*, detail_screen.*, hero_screen.*, settings_screen.*, wifi_setup_screen.*
  util/     interpolate.*  (PCHIP smoothing, unit-tested under test/test_native)
```

---

## How to start

1. Ask me anything ambiguous (e.g. whether the companion API already exists or should
   be stubbed, and whether the "app" for adding platforms/accounts is the on-device
   web admin alone or a separate phone app talking to the same server API). Launch
   platforms are decided: **YouTube + Twitch**, rest later via scraping.
2. Scaffold PlatformIO + `lv_conf.h` + the two-task skeleton + a single hardcoded
   fake account rendering on the list screen, and get it building.
3. Stand up the simulator so we can iterate on UI without flashing.
4. Then build out the fetcher against the (stubbed) social API, the detail chart, and
   the web admin.

Don't over-build ahead of these steps. Confirm the build compiles (`pio run -e cyd`)
and show me a simulator screenshot at each milestone.
