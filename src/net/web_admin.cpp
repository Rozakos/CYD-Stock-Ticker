#include "web_admin.h"

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "../settings/settings_store.h"

namespace {

AsyncWebServer  g_server(80);
SettingsStore*  g_settings = nullptr;

String maskKey(const String& k) {
  if (k.length() <= 6) return "******";
  return k.substring(0, 4) + "…" + k.substring(k.length() - 4);
}

String page(const SettingsStore& s, const String& flash = "") {
  String html;
  html.reserve(2048);
  html += F("<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Stock Ticker — Settings</title>"
            "<style>"
            "body{font-family:system-ui,sans-serif;max-width:520px;margin:2em auto;padding:0 1em;background:#111;color:#eee}"
            "h1{font-size:1.2em}"
            "label{display:block;margin:1em 0 .25em;font-size:.9em;color:#9af}"
            "input{width:100%;padding:.5em;background:#222;color:#eee;border:1px solid #444;border-radius:4px;font:inherit}"
            "button{margin-top:1.5em;padding:.6em 1.2em;background:#39f;color:#fff;border:0;border-radius:4px;cursor:pointer}"
            ".note{font-size:.8em;color:#888;margin-top:.25em}"
            ".flash{padding:.6em;background:#093;border-radius:4px;margin-bottom:1em}"
            "</style>"
            "<h1>Stock Ticker — Settings</h1>");
  if (flash.length()) {
    html += "<div class=flash>" + flash + "</div>";
  }
  html += F("<form method=POST action=/settings>"
            "<label>API bearer token</label>"
            "<input name=api_key placeholder='leave blank to keep current'>"
            "<div class=note>current: ");
  html += maskKey(s.apiKey());
  html += F("</div>"
            "<label>Symbols (comma separated)</label>"
            "<input name=symbols value='");
  // current symbols come back joined
  auto syms = s.symbols();
  for (size_t i = 0; i < syms.size(); ++i) {
    if (i) html += ",";
    html += syms[i];
  }
  html += F("'>"
            "<label>Refresh interval (seconds)</label>"
            "<input name=refresh_s type=number min=15 value='");
  html += String(s.refreshSeconds());
  html += F("'>"
            "<label>Admin username</label>"
            "<input name=admin_user value='");
  html += s.adminUser();
  html += F("'>"
            "<label>Admin password</label>"
            "<input name=admin_pass type=password placeholder='leave blank to keep current'>"
            "<button type=submit>Save</button>"
            "</form>");
  return html;
}

bool authed(AsyncWebServerRequest* req) {
  String user = g_settings->adminUser();
  String pass = g_settings->adminPass();
  if (!req->authenticate(user.c_str(), pass.c_str())) {
    req->requestAuthentication();
    return false;
  }
  return true;
}

}  // namespace

namespace web_admin {

void begin(SettingsStore* settings) {
  g_settings = settings;

  g_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    req->redirect("/settings");
  });

  g_server.on("/settings", HTTP_GET, [](AsyncWebServerRequest* req) {
    if (!authed(req)) return;
    req->send(200, "text/html", page(*g_settings));
  });

  g_server.on("/settings", HTTP_POST, [](AsyncWebServerRequest* req) {
    if (!authed(req)) return;
    String api  = req->hasParam("api_key", true)    ? req->getParam("api_key", true)->value()    : "";
    String syms = req->hasParam("symbols", true)    ? req->getParam("symbols", true)->value()    : "";
    String ref  = req->hasParam("refresh_s", true)  ? req->getParam("refresh_s", true)->value()  : "60";
    String au   = req->hasParam("admin_user", true) ? req->getParam("admin_user", true)->value() : "";
    String ap   = req->hasParam("admin_pass", true) ? req->getParam("admin_pass", true)->value() : "";
    g_settings->update(api, ref.toInt(), syms, au, ap);
    req->send(200, "text/html", page(*g_settings, "Saved."));
  });

  g_server.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "not found");
  });

  g_server.begin();
}

}  // namespace web_admin
