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

// Feeds the settings page into AsyncTCP's own chunk buffer, so rendering it
// needs no large contiguous allocation at all.
//
// This replaces a single pre-sized 10 KB response buffer guarded by a 12 KB
// max-alloc floor. That floor was unmeetable in practice: once the list rows
// and logo cache are resident the largest free block idles ~13-17 KB and dips
// to ~9 KB after a few stock taps, so the page 503'd ("busy — refresh in a
// moment") and, worse, tried to buy room by dropping the persistent TLS
// session. That drop is a gamble — the handshake needs ~33-35 KB contiguous
// to come back, which mid-run is usually just out of reach, and when it loses
// quotes and history die until the dead-fetch watchdog reboots the device.
// Chunking removes the need for the buffer, the floor and the gamble at once.
//
// write_settings_page is a linear writer, not resumable, so each chunk re-runs
// it from the top and discards the bytes already sent. That is O(n^2) in page
// size — but the page is ~9 KB against ~1.4 KB chunks, so it is roughly seven
// re-renders of string literals on a 240 MHz core, and it costs zero heap.
class ChunkWriter {
 public:
  ChunkWriter(uint8_t* out, size_t cap, size_t skip)
      : out_(out), cap_(cap), skip_(skip) {}

  // The three shapes write_settings_page actually uses. On ESP32 flash is
  // memory-mapped, so PROGMEM literals and F() strings are plain reads.
  void print(const char* s)                { write(s, strlen(s)); }
  void print(const String& s)              { write(s.c_str(), s.length()); }
  void print(const __FlashStringHelper* s) {
    const char* p = reinterpret_cast<const char*>(s);
    write(p, strlen(p));
  }

  size_t produced() const { return produced_; }

 private:
  void write(const char* data, size_t len) {
    size_t off = 0;
    if (seen_ < skip_) {                   // still before this chunk's window
      size_t drop = skip_ - seen_;
      if (drop >= len) { seen_ += len; return; }
      off = drop;
    }
    if (produced_ < cap_) {                // room left in the chunk
      size_t n = len - off;
      if (n > cap_ - produced_) n = cap_ - produced_;
      memcpy(out_ + produced_, data + off, n);
      produced_ += n;
    }
    seen_ += len;                          // keep counting past a full chunk
  }

  uint8_t* out_;
  size_t   cap_;
  size_t   skip_;
  size_t   produced_ = 0;   // bytes copied into this chunk
  size_t   seen_     = 0;   // bytes the page writer has generated so far
};

void send_settings_page(AsyncWebServerRequest* req) {
  // `index` is how much of the body has already gone out, which is exactly
  // the offset this chunk starts at. Returning 0 ends the response, which
  // happens naturally once the cursor runs past the end of the page.
  req->send(req->beginChunkedResponse(
      "text/html",
      [](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
        if (!buffer || maxLen == 0) return RESPONSE_TRY_AGAIN;
        ChunkWriter w(buffer, maxLen, index);
        web_admin_page::write_settings_page(w, *g_settings);
        return w.produced();
      }));
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
