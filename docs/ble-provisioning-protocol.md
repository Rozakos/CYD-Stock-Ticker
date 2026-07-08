# BLE WiFi Provisioning Protocol

Contract between the Rozakos Home Android app and the CYD Stock Ticker
firmware. The app implements the **central** (client); the firmware implements
the **peripheral** (GATT server). Both sides must use the exact UUIDs and JSON
shapes below.

App side of record: `app/src/main/java/com/rozakos/home/data/ble/BleProvisioningProtocol.kt`.
Firmware side: `src/net/ble_provisioning.cpp` (NimBLE-Arduino), driven from the
net task in `src/main.cpp`.

## Advertising

- Advertise as a connectable BLE peripheral.
- Local name: `CYD-Ticker-XXXX` (suffix = last 2 bytes of the STA MAC, hex).
- Advertise the 128-bit **service UUID** below so the app can filter scans (the
  app scans with a `ServiceUuid` filter and ignores everything else). The
  service UUID rides in the advertisement payload; the local name rides in the
  scan response (a 128-bit UUID + flags nearly fills the 31-byte adv packet).
- Advertise only on UNPROVISIONED boots (no stored WiFi credentials). A
  provisioned device never starts BLE — NimBLE's RAM footprint starves the
  ticker's TLS client (see AGENTS.md "NimBLE and mbedTLS cannot coexist") —
  and is discoverable over mDNS/LAN instead (`_rozakos._tcp` +
  `GET /api/device-info`). To re-provision over BLE, the user taps the WiFi
  icon on the status bar, which forgets the network and reboots the device
  unprovisioned.

## GATT service

Service UUID: `5f1d0001-7b3a-4c2e-9d8f-1a2b3c4d5e6f`

| Characteristic | UUID | Properties | Payload |
|---|---|---|---|
| Credentials | `5f1d0002-7b3a-4c2e-9d8f-1a2b3c4d5e6f` | Write, Write No Response | UTF-8 JSON `{"ssid":"..","pass":".."}` |
| Status | `5f1d0003-7b3a-4c2e-9d8f-1a2b3c4d5e6f` | Read, Notify (+ CCCD) | UTF-8 JSON `{"state":"..","ip":"..","reason":".."}` |
| Device info | `5f1d0004-7b3a-4c2e-9d8f-1a2b3c4d5e6f` | Read | UTF-8 JSON `{"name":"..","type":"..","fw":"..","mac":".."}` |

CCCD (notifications) descriptor: standard `0x2902` (auto-created by NimBLE for
the Notify property).

### Status states

- `idle` — booted, no attempt yet.
- `connecting` — credentials received, joining WiFi.
- `connected` — joined; `ip` MUST be the device's DHCP IPv4 address (e.g.
  `192.168.1.50`).
- `failed` — could not join; `reason` is a short human-readable message.

`ip` is only required for `connected`. `reason` is only required for `failed`.
(The firmware always emits all three keys, using empty strings where N/A.)

### Device info fields

- `name` — default display name, e.g. `CYD Stock Ticker` (app pre-fills this).
- `type` — must be `cyd_stock_ticker` (matches the app's `DeviceTypeId`).
- `fw` — firmware version string (`cfg::FW_VERSION`).
- `mac` — station MAC.

## Flow (app → device)

1. App connects, negotiates MTU (requests 247).
2. App reads **Device info** (best-effort).
3. App subscribes to **Status** notifications (writes CCCD). On subscribe the
   device immediately pushes the current status. (Already-provisioned devices
   don't advertise BLE — the app finds those over mDNS/LAN.)
4. App writes **Credentials**.
5. Device attempts WiFi, notifying `connecting` → then `connected` (with `ip`)
   or `failed` (with `reason`).
6. App records the reported IP and disconnects.

## Notes

- Support an ATT MTU of at least 247 so a full credentials JSON arrives in one
  write.
- Persist credentials and auto-reconnect on boot. The firmware stores them in
  `/settings.json` (LittleFS) via `SettingsStore::setWifi`, which is the same
  store `wifi_mgr::begin()` reads on boot — so there is a single source of
  truth and the existing captive-portal path stays compatible. Credentials are
  persisted only after a successful join.
- Always emit a terminal `connected`/`failed` so the app doesn't hang (the app
  also times out after 90 s; the firmware's join attempt is bounded to ~15 s).
- The captive-portal AP (`CYD-Setup-XXXX`) remains as a fallback path; BLE is
  additive. A failed BLE join rebuilds the AP + portal so a non-app user can
  still provision.
- After the setup window closes (or the app reads its terminal status and
  disconnects), if WiFi is connected and no app is attached, the firmware
  tears down the BLE stack and **reboots** into a provisioned boot — BLE
  stays down and the ticker gets the full heap (the released BT RAM is too
  fragmented for the TLS buffers, so limping on without a reboot would leave
  quotes dead). The app must therefore finish the flow — read the terminal
  `connected`/`failed` status and disconnect — before expecting the device
  at its reported IP; the reboot follows within seconds and the device
  answers on mDNS + HTTP shortly after.
