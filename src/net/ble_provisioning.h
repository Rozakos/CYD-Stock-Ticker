#pragma once

#include <Arduino.h>

// BLE WiFi provisioning peripheral (GATT server) for the Rozakos Home Android
// app. Implements the contract in docs/ble-provisioning-protocol.md: a service
// with Credentials (write), Status (read/notify) and Device-info (read)
// characteristics. The NimBLE callbacks run on the BLE host task and only
// stash incoming credentials; the net task drives the actual WiFi join and
// publishes status, so all radio/WiFi work stays on the task that owns it.
namespace ble_prov {

// Initialises NimBLE, builds the GATT server, fills in the device-info
// characteristic and starts advertising. Safe no-op if already started.
void begin();

// True while the BLE stack is initialised (false before begin() / after end()).
bool active();

// True while a central (the phone app) is connected over BLE.
bool clientConnected();

// True while we are currently advertising.
bool advertising();

// Net task: pulls the most recently written credentials, if any. Returns true
// and fills ssid/pass when a write is pending (clearing the pending flag).
bool takePendingCreds(String& ssid, String& pass);

// Publishes a status update: caches the JSON for later Read/subscribe and
// notifies subscribed centrals. `state` is idle|connecting|connected|failed.
// `ip` is used for connected; `reason` for failed (pass "" otherwise).
void setStatus(const char* state, const String& ip, const char* reason);

// Advertising control (idempotent). startAdvertising is a no-op while a client
// is connected (NimBLE stops advertising on connect anyway).
void startAdvertising();
void stopAdvertising();

// Tears down the whole BLE stack and frees its RAM. After this active() is
// false; a reboot is needed to provision over BLE again.
void end();

}  // namespace ble_prov
