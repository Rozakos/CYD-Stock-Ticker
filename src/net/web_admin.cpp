#include "web_admin.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "../config.h"
#include "../settings/settings_store.h"
#include "device_identity.h"
#include "web_admin_page.h"

namespace {

AsyncWebServer  g_server(80);
SettingsStore*  g_settings = nullptr;

void redirect_to_settings(AsyncWebServerRequest* req) {
  AsyncWebServerResponse* response = req->beginResponse(303, "text/plain", "");
  response->addHeader("Location", "/settings");
  req->send(response);
}

void save_symbols(const std::vector<String>& syms) {
  g_settings->update(g_settings->apiKey(),
                     g_settings->refreshSeconds(),
                     web_admin_page::symbols_csv(syms));
}

// The settings page is built in one pre-sized response buffer.
// RESPONSE_BUF: big enough for the whole page (~9 KB at a handful of
// symbols) so the cbuf never grows — the default 1460-byte buffer grew by
// realloc-and-copy on every write, transiently needing old+new at once,
// which threw std::bad_alloc -> abort() on the tight steady-state heap.
// HEAP_FLOOR: even the single upfront allocation can fail when a request
// lands mid-quote-fetch (TLS/JSON transients dip the heap hard); operator
// new's bad_alloc is uncatchable here (-fno-exceptions core), so refuse
// politely with a 503 + Retry-After instead of rebooting the device.
// Sized against the measured steady-state heap: with the TLS session and
// logo cache resident the largest free block idles ~14-16 KB, so the buffer
// (page is ~9 KB; grows ~350 B per symbol row) plus slack must stay under
// that or the guard 503s every request. Revisit if the page outgrows 10 KB.
constexpr size_t RESPONSE_BUF = 10 * 1024;
constexpr size_t HEAP_FLOOR   = RESPONSE_BUF + 2 * 1024;  // + String churn

void send_settings_page(AsyncWebServerRequest* req) {
  if (ESP.getMaxAllocHeap() < HEAP_FLOOR) {
    AsyncWebServerResponse* busy =
        req->beginResponse(503, "text/plain", "busy — refresh in a moment");
    busy->addHeader("Retry-After", "2");
    req->send(busy);
    return;
  }
  AsyncResponseStream* response =
      req->beginResponseStream("text/html", RESPONSE_BUF);
  web_admin_page::write_settings_page(*response, *g_settings);
  req->send(response);
}

}  // namespace

namespace web_admin {

void begin(SettingsStore* settings) {
  g_settings = settings;

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    redirect_to_settings(req);
  });

  g_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    send_settings_page(req);
  });

  g_server.on("/settings", HTTP_POST, [](AsyncWebServerRequest* req) {
    String api  = req->hasParam("api_key", true)   ? req->getParam("api_key", true)->value()   : "";
    String ref  = req->hasParam("refresh_s", true) ? req->getParam("refresh_s", true)->value() : String(cfg::DEFAULT_REFRESH_SECONDS);
    api.trim();
    g_settings->update(api, ref.toInt(), web_admin_page::symbols_csv(*g_settings));
    redirect_to_settings(req);
  });

  g_server.on("/add", HTTP_POST, [](AsyncWebServerRequest* req) {
    String sym = req->hasParam("symbol", true) ? req->getParam("symbol", true)->value() : "";
    sym.trim();
    sym.toUpperCase();
    if (sym.length() > 0 && sym.length() <= 12) {
      auto syms = g_settings->symbols();
      bool exists = false;
      for (const auto& existing : syms) {
        if (existing == sym) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        syms.push_back(sym);
        save_symbols(syms);
      }
    }
    redirect_to_settings(req);
  });

  g_server.on("/shares", HTTP_POST, [](AsyncWebServerRequest* req) {
    String sym = req->hasParam("symbol", true) ? req->getParam("symbol", true)->value() : "";
    String qty = req->hasParam("qty", true)    ? req->getParam("qty", true)->value()    : "";
    sym.trim();
    if (sym.length() > 0 && sym.length() <= 12) {
      g_settings->setShares(sym, qty.toFloat());   // <= 0 clears the holding
    }
    redirect_to_settings(req);
  });

  g_server.on("/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    int idx = req->hasParam("i", true) ? req->getParam("i", true)->value().toInt() : -1;
    auto syms = g_settings->symbols();
    if (idx >= 0 && (size_t)idx < syms.size()) {
      syms.erase(syms.begin() + idx);
      save_symbols(syms);
    }
    redirect_to_settings(req);
  });

  // Machine-readable identity for the Rozakos Home app's LAN discovery
  // (mDNS confirm + subnet-scan probe). Contract:
  // docs/device-discovery-protocol.md — keep fields in sync with the app.
  g_server.on("/api/device-info", HTTP_GET, [](AsyncWebServerRequest* req) {
    JsonDocument d;
    d["id"]        = device_identity::deviceId();
    d["name"]      = "CYD Stock Ticker";
    d["type"]      = "cyd_stock_ticker";
    d["fw"]        = cfg::FW_VERSION;
    d["mac"]       = device_identity::macString();
    d["webUiPath"] = "/settings";
    JsonArray caps = d["capabilities"].to<JsonArray>();
    caps.add("web_ui");
    caps.add("settings");
    String out;
    serializeJson(d, out);
    req->send(200, "application/json", out);
  });

  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  g_server.begin();
}

}  // namespace web_admin
