#include "web_admin.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "../config.h"
#include "../settings/settings_store.h"
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

void send_settings_page(AsyncWebServerRequest* req) {
  AsyncResponseStream* response = req->beginResponseStream("text/html");
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

  g_server.on("/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    int idx = req->hasParam("i", true) ? req->getParam("i", true)->value().toInt() : -1;
    auto syms = g_settings->symbols();
    if (idx >= 0 && (size_t)idx < syms.size()) {
      syms.erase(syms.begin() + idx);
      save_symbols(syms);
    }
    redirect_to_settings(req);
  });

  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  g_server.begin();
}

}  // namespace web_admin
