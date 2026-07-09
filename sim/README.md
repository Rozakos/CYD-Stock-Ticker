# LVGL desktop simulator

A host build of the project's UI screens so changes can be previewed without
flashing the device. Used by Claude to "see" what the UI looks like after a
change (the headless mode dumps a PNG that the agent then reads).

## Toolchain

Requires a host C++ toolchain. The repo's flow is MSYS2 + MinGW + SDL2:

```powershell
winget install -e --id MSYS2.MSYS2
C:\msys64\usr\bin\bash -lc "pacman -Sy --noconfirm"
C:\msys64\usr\bin\bash -lc "pacman -S --needed --noconfirm `
  mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja `
  mingw-w64-x86_64-SDL2 mingw-w64-x86_64-pkgconf"
```

## Build & run

```powershell
.\sim\build_sim.ps1

cd sim/build
.\cyd_sim.exe --window                                # interactive SDL window
.\cyd_sim.exe --headless --screen=list     --out=list.png
.\cyd_sim.exe --headless --screen=detail   --symbol=AAPL --out=detail.png
.\cyd_sim.exe --headless --screen=settings --out=settings.png
.\cyd_sim.exe --web-settings=settings_web.html
```

Flags:

- `--window` / `--headless` — pick a display mode (default `--window`).
- `--screen=list|detail|settings` — which screen to render.
- `--symbol=AAPL` — symbol to show in detail mode.
- `--out=path.png` — write a PNG snapshot after warmup (works in either mode).
- `--ticks=N` — warmup frames before the snapshot (default 10).
- `--click=X,Y` / `--longpress=X,Y` — inject a tap / long press after warmup
  (long press holds 600 ms; LVGL's threshold is 400 ms).
- `--intraday` — swap the seeded history for 1-minute-spaced intraday data
  (exercises the HH:MM X-axis formatter).
- `--data=path` — root directory for LittleFS read overlay + LVGL drive `L:`.
  Defaults to `../../data` (i.e. project root `data/`) which matches running
  from `sim/build/`.
- `--web-settings=path` renders the same Rozakos-branded web admin HTML used
  by firmware `/settings` to a file, then exits. Open the generated HTML in a
  browser to review the settings form, symbol add/delete, and the Shares
  (holdings) column.

## How it's wired

- **Same screen sources as the firmware.** `src/ui/*.cpp` and
  `src/net/quote_store.cpp` and `src/settings/settings_store.cpp` are
  compiled into the sim verbatim. Display, networking, and main are
  swapped out.
- **Same web-admin renderer as the firmware.** `--web-settings=...` calls
  `src/net/web_admin_page.h`, which is also used by `src/net/web_admin.cpp`.
  Keep web-admin markup there so firmware and browser previews do not drift.
- **Arduino shim** lives in `sim/compat/` (`Arduino.h`, `WiFi.h`,
  `LittleFS.h`, `freertos/*`). Only the surface actually used by the
  files above is implemented. `String`, `millis`, `log_i`, FreeRTOS
  semaphores, a host-backed `LittleFS`, and a `WiFi` that always
  reports connected. Settings are persisted to `sim_data/settings.json`
  (gitignored) so changes survive between runs.
- **LVGL display** is software-rendered into an RGB565 framebuffer.
  In `--window` mode the framebuffer is also blitted into an SDL2
  texture at 2× scale. In `--headless` mode SDL is never initialized.
- **LVGL filesystem driver** registers drive letter `L:` and maps it
  to `--data` so `L:/logos/AAPL.png` lookups hit the project's
  `data/logos/`.
- **LV_MEM_SIZE** is bumped to 512 KB for the sim (vs the firmware's
  32 KB) via a CMake `-D` override. The shim has plenty of host DRAM
  and the firmware budget would crash the sim once both list and
  another screen are alive at once.
- **Fake data previews the session-aware UI.** `fake_data::seed` marks NVDA
  post-market and TSLA pre-market (`Quote::session`), so the list renders
  the AFTER HOURS night palette + status-bar crescent out of the box, and
  `sim_main` seeds sample holdings (AAPL ×10, NVDA ×2) once so the portfolio
  total + day P/L readout shows in the bar's right slot. History responses
  are stamped with the request generation (`HistoryRequest::gen`), matching
  the firmware's silent-refresh contract.

## Known issues

- **Logos render via the in-memory ARGB path** (LovyanGFX pngle, same as
  firmware) when a matching PNG exists under the `--data` overlay
  (`<data>/logos/<SYMBOL>.png`). The project's `data/` is deliberately
  near-empty, so out of the box every symbol shows the letter badge —
  drop PNGs into a scratch dir and pass `--data=` to preview real logos.
- **`list_screen::tick()` is only called when the list screen is
  active.** If we tick it while a different screen is current, the
  sim segfaults inside the row-rebuild path. Cause not yet root-caused
  — the firmware always has the list screen current when its tick runs,
  so this hasn't bitten the device.

## Layout bugs found via the sim

- **Settings screen — symbol list overflows the card** and overlaps the
  "Configure at http://..." footer when the symbols string is long
  (e.g. the default `AAPL,MSFT,NVDA,TSLA,GOOG`). Visible in
  `sim/build/settings.png`.
