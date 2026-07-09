#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp32-hal.h>      // disableCore0WDT
#include <esp_bt.h>         // esp_bt_mem_release
#include <lvgl.h>
#include <time.h>

#include "config.h"
#include "display/fs_littlefs.h"
#include "display/lgfx_cyd.hpp"
#include "display/lvgl_bridge.h"
#include "net/ble_provisioning.h"
#include "net/captive_portal.h"
#include "net/mdns_svc.h"
#include "net/quote_fetcher.h"
#include "net/quote_store.h"
#include "net/web_admin.h"
#include "net/wifi_mgr.h"
#include "secrets.h"
#include "settings/settings_store.h"
#include "ui/detail_screen.h"
#include "ui/list_screen.h"
#include "ui/logos.h"
#include "ui/settings_screen.h"
#include "ui/styles.h"
#include "ui/wifi_setup_screen.h"

namespace {

LGFX_CYD       g_gfx;
SettingsStore  g_settings;
QuoteStore     g_quoteStore;
SemaphoreHandle_t g_lvglMu = nullptr;

void uiTask(void*) {
  xSemaphoreTake(g_lvglMu, portMAX_DELAY);
  styles::init();
  logos::prepareRuntimeDecoder();
  // Decode the cached logo PNGs NOW, while the heap still has its pristine
  // boot-time contiguous block — once the TLS session and the row widgets
  // carve it up, runtime decodes mostly fail and rows fall back to badges.
  logos::prewarmRuntimeCache(g_settings.symbols());
  settings_screen::init(&g_settings);
  list_screen::build(&g_quoteStore, &g_settings);
  logos::releaseRuntimeDecoder();
  // Initial screen is chosen by netTask once it knows STA vs AP state;
  // until then show a blank list screen.
  lv_screen_load(list_screen::screen());
  xSemaphoreGive(g_lvglMu);

  uint32_t lastPoll = 0;
  for (;;) {
    xSemaphoreTake(g_lvglMu, portMAX_DELAY);
    uint32_t wait = lv_timer_handler();
    if (millis() - lastPoll > 250) {
      list_screen::tick();
      detail_screen::tick();
      settings_screen::tick();
      wifi_setup_screen::tick();
      lastPoll = millis();
    }
    xSemaphoreGive(g_lvglMu);
    vTaskDelay(pdMS_TO_TICKS(wait > 30 ? 30 : (wait ? wait : 5)));
  }
}

void netTask(void*) {
  // HTTPS calls to rozakos.eu can block core 0 for several seconds during
  // the TLS handshake, which starves IDLE0 and trips the task watchdog.
  // Detaching IDLE0 from the WDT is the canonical fix for tasks that make
  // blocking network calls outside the Arduino loop() task.
  disableCore0WDT();

  // BLE provisioning runs ONLY on unprovisioned boots. NimBLE and mbedTLS
  // cannot coexist on this part: with the BLE stack resident there is never
  // a contiguous ~17 KB block for the TLS buffers (quotes fail with
  // MBEDTLS_ERR_SSL_ALLOC_FAILED), and even esp_bt_mem_release() hands the
  // controller's RAM back as fragmented sub-16 KB heap regions. A
  // provisioned device is discoverable over mDNS/LAN instead (mdns_svc),
  // and a WiFi reset (status-bar tap) reboots unprovisioned, which brings
  // BLE back. On unprovisioned boots, bring BLE up first so the device is
  // already advertising (and answering the app) while the blocking STA
  // join below runs — there are no quotes to fetch without WiFi, so the
  // TLS starvation doesn't matter until provisioning completes (see the
  // reboot in the lifecycle block below).
  // BLE runs only on unprovisioned boots; on provisioned boots setup()
  // already released the BT controller's RAM to the heap (see setup()).
  if (g_settings.wifiSsid().length() == 0) ble_prov::begin();
  uint32_t ble_window_start = millis();
  bool     ble_client_was   = false;   // tracks a central connect→disconnect edge

  wifi_mgr::begin(g_settings);
  if (wifi_mgr::connected()) {
    // Already provisioned: seed "connected" so an app that subscribes during
    // the boot window immediately learns the IP (advertising stays up for the
    // window — see the lifecycle block in the loop).
    ble_prov::setStatus("connected", wifi_mgr::ip(), "");
  }

  bool ap_mode_was = wifi_mgr::apActive();
  if (ap_mode_was) {
    captive_portal::begin(g_settings);
    xSemaphoreTake(g_lvglMu, portMAX_DELAY);
    wifi_setup_screen::show(wifi_mgr::apSsid(), wifi_mgr::apPass());
    xSemaphoreGive(g_lvglMu);
  }

  bool sta_services_up = false;
  auto bringUpStaServices = [&] {
    if (sta_services_up) return;
    // NTP first — configTime() sets the clock to UTC and resets TZ to UTC
    // internally, so our local TZ must be applied AFTER it. (Doing it before
    // was clobbered, leaving localtime_r on UTC — the 1D axis showed 13:30
    // instead of 16:30 EET.)
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    setenv("TZ", cfg::TIME_TZ, 1);
    tzset();
    web_admin::begin(&g_settings);
    mdns_svc::begin();
    xSemaphoreTake(g_lvglMu, portMAX_DELAY);
    lv_screen_load(list_screen::screen());
    xSemaphoreGive(g_lvglMu);
    sta_services_up = true;
  };

  if (wifi_mgr::connected()) bringUpStaServices();

  uint32_t lastFetch = 0;
  // Transport watchdog: consecutive fetch cycles in which not a single HTTP
  // request got a server response (any status counts, even 401 — only pure
  // transport death trips this). lwIP can wedge with the WiFi link still up
  // (e.g. TCP PCB exhaustion after a burst of web-UI connections): inbound
  // HTTP stops answering AND outbound connects time out, and it never
  // self-heals. A clean reboot is the only reliable unwedge; boots are
  // cheap (settings + logo cache persist, prewarm remounts the icons).
  uint32_t deadFetchCycles = 0;
  for (;;) {
    captive_portal::loop();

    // --- BLE provisioning: a phone wrote WiFi credentials -----------------
    // The NimBLE callback only stashes the creds; do the (blocking) join here
    // on the net task, which owns the radio. Always emit a terminal status so
    // the app never hangs.
    String bleSsid, blePass;
    if (ble_prov::active() && ble_prov::takePendingCreds(bleSsid, blePass)) {
      log_i("[ble] provisioning join ssid='%s'", bleSsid.c_str());
      ble_prov::setStatus("connecting", "", "");
      if (wifi_mgr::connect(bleSsid, blePass)) {
        g_settings.setWifi(bleSsid, blePass);   // persist -> auto-reconnect on boot
        ble_prov::setStatus("connected", wifi_mgr::ip(), "");
        // The setup AP was already dropped by connect(); also free the portal's
        // DNS/web server if it was running (unprovisioned-boot path).
        captive_portal::end();
        ap_mode_was = false;                     // we're on STA now
      } else {
        ble_prov::setStatus("failed", "", wifi_mgr::lastFailMessage());
        // The join dropped any setup AP — rebuild the captive-portal fallback
        // so a user not using the app can still provision.
        captive_portal::end();
        wifi_mgr::startApFallback();
        captive_portal::begin(g_settings);
        xSemaphoreTake(g_lvglMu, portMAX_DELAY);
        wifi_setup_screen::show(wifi_mgr::apSsid(), wifi_mgr::apPass());
        xSemaphoreGive(g_lvglMu);
        ap_mode_was = true;
      }
    }

    if (wifi_mgr::connected()) {
      bringUpStaServices();
      if (lastFetch == 0 ||
          millis() - lastFetch > g_settings.refreshSeconds() * 1000UL) {
        if (fetcher::fetchQuotes(g_settings, g_quoteStore)) {
          lastFetch = millis();
          if (fetcher::sawHttpResponseThisCycle()) {
            deadFetchCycles = 0;
          } else if (++deadFetchCycles >= 5) {
            log_e("[net] no HTTP response for %u cycles with WiFi up — "
                  "TCP stack wedged, restarting", (unsigned)deadFetchCycles);
            delay(200);
            ESP.restart();
          }
        } else {
          lastFetch = millis() - (g_settings.refreshSeconds() * 1000UL) + 5000;
        }
      }
      HistoryRequest hr;
      if (g_quoteStore.takePendingHistory(hr)) {
        fetcher::fetchHistory(g_settings, g_quoteStore,
                              hr.symbol, hr.range, hr.gen);
      }
      // The UI defers runtime logo decodes when the largest contiguous heap
      // block is too small — usually because our persistent TLS session holds
      // ~40 KB of it. Drop the session so the block recovers; list_screen's
      // retry sweep mounts the logo within seconds and the next fetch
      // reconnects transparently.
      if (logos::consumeDecodeStarved()) fetcher::releaseApiConnection();
    } else if (wifi_mgr::apActive() && !ap_mode_was) {
      // Reconnect attempt failed and we fell back to AP — re-arm the portal.
      ap_mode_was = true;
      captive_portal::begin(g_settings);
      xSemaphoreTake(g_lvglMu, portMAX_DELAY);
      wifi_setup_screen::show(wifi_mgr::apSsid(), wifi_mgr::apPass());
      xSemaphoreGive(g_lvglMu);
    } else if (wifi_mgr::connected() && ap_mode_was) {
      // STA came up after a captive-portal save.
      ap_mode_was = false;
    }

    // --- BLE advertising lifecycle ----------------------------------------
    if (ble_prov::active()) {
      bool window_open = (millis() - ble_window_start) < cfg::BLE_SETUP_WINDOW_MS;
      bool wifi_up     = wifi_mgr::connected();
      bool provisioned = g_settings.wifiSsid().length() > 0;
      bool client_now  = ble_prov::clientConnected();
      // The app reads status then disconnects (protocol flow step 6); that
      // falling edge means the session is done.
      bool client_left = ble_client_was && !client_now;
      ble_client_was   = client_now;

      if (wifi_up && !client_now && (!window_open || client_left)) {
        // Online with no app attached — either the setup window closed or
        // the provisioning app just finished. BLE only runs on
        // unprovisioned boots, so reaching here means WiFi credentials
        // were just provisioned (BLE or captive portal) this boot. The
        // released BT RAM is too fragmented for TLS (see setup comment),
        // so quotes can't start in this boot — restart into a clean
        // provisioned boot: BLE stays down and the ticker gets the full
        // heap. Credentials are already persisted.
        ble_prov::end();
        log_i("[ble] provisioning complete — rebooting into ticker mode");
        delay(200);
        ESP.restart();
      } else {
        // Stay discoverable while a user could still need us (unprovisioned,
        // or inside the boot/setup window). Drop advertising only once an app
        // is on the link AND WiFi is up — it gets status over that connection,
        // which honors "stop advertising once connected" without stranding an
        // app that hasn't yet discovered an already-provisioned device.
        bool want_adv = (!provisioned || window_open) && !(wifi_up && client_now);
        if (want_adv) ble_prov::startAdvertising();
        else          ble_prov::stopAdvertising();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(50);
  log_i("CYD stock ticker starting");

  if (!LittleFS.begin(true)) {
    log_e("LittleFS mount failed");
  }
  // One-shot wipe of stale cached logos when the logo-cache version changes,
  // so they re-download at the current server quality (the on-disk cache is
  // keyed only by 48x48 dims, so a same-size quality bump is otherwise unseen).
  fetcher::purgeStaleLogoCache(cfg::LOGO_CACHE_VERSION);
  g_settings.begin(API_TOKEN_SEED, WIFI_SSID, WIFI_PASS);
  g_quoteStore.begin();

  // Provisioned boot: BLE will never run (netTask skips ble_prov::begin),
  // but linking NimBLE statically reserves the BT controller's ~50 KB of
  // DRAM regardless. Hand it back to the heap NOW, before the tasks start —
  // the returned regions are fragmented (sub-16 KB), and doing this before
  // logos::prewarmRuntimeCache lets the ~9 KB logo bitmaps settle into
  // those fragments (TLSF good-fit) instead of carving up the big
  // contiguous block that mbedTLS needs for the TLS buffers.
  if (g_settings.wifiSsid().length() > 0) {
    esp_bt_mem_release(ESP_BT_MODE_BTDM);
  }

  g_gfx.init();
  g_gfx.setRotation(1);   // landscape, USB-C/header at left
  g_gfx.setBrightness(255);
  g_gfx.fillScreen(0x0000);

  lv_init();
  bridge::init(&g_gfx);
  fs_littlefs::init();

  g_lvglMu = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(uiTask,  "ui",  8192,  nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  vTaskDelete(nullptr);
}
