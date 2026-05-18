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
- `--data=path` — root directory for LittleFS read overlay + LVGL drive `L:`.
  Defaults to `../../data` (i.e. project root `data/`) which matches running
  from `sim/build/`.
- `--web-settings=path` renders the same Rozakos-branded web admin HTML used
  by firmware `/settings` to a file, then exits. Open the generated HTML in a
  browser to review the add/delete symbol table and settings form.

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

## Known issues

- **PNG logos don't render in the sim**; the letter-badge fallback
  shows instead. The LVGL FS driver opens the PNG files (verified by
  trace), and lodepng is linked, but the decoded image doesn't appear.
  On-device logo rendering is unaffected. (TODO: diagnose; probably a
  color-format or draw-buffer mismatch in our software-only flush
  path.)
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
