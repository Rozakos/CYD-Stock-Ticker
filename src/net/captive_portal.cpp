#include "captive_portal.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>

#include "../settings/settings_store.h"
#include "wifi_mgr.h"

namespace captive_portal {

namespace {

DNSServer*       g_dns      = nullptr;
AsyncWebServer*  g_server   = nullptr;
SettingsStore*   g_settings = nullptr;
bool             g_pendingReconnect = false;

String htmlEscape(const String& in) {
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '<':  out += "&lt;"; break;
      case '>':  out += "&gt;"; break;
      case '&':  out += "&amp;"; break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:   out += c;
    }
  }
  return out;
}

String scanPage() {
  // Scan synchronously so the dropdown is populated when the page renders.
  // ~3s on most boards; we're in AP mode so nothing else is using the radio.
  int n = WiFi.scanNetworks(false, true);

  String html;
  html.reserve(2048);
  html += F("<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>CYD WiFi Setup</title>"
            "<style>"
            "body{font-family:system-ui,sans-serif;max-width:480px;margin:1em auto;padding:0 1em;background:#111;color:#eee}"
            "h1{font-size:1.2em}"
            "label{display:block;margin:1em 0 .25em;font-size:.9em;color:#9af}"
            "select,input{width:100%;padding:.6em;background:#222;color:#eee;border:1px solid #444;border-radius:4px;font:inherit;box-sizing:border-box}"
            "button{margin-top:1.5em;padding:.7em 1.4em;background:#39f;color:#fff;border:0;border-radius:4px;cursor:pointer;font-size:1em}"
            ".note{font-size:.8em;color:#888;margin-top:.25em}"
            "</style>"
            "<h1>Connect CYD to your WiFi</h1>"
            "<form method=POST action=/save>"
            "<label>Network</label>"
            "<select name=ssid id=ssid>");
  if (n <= 0) {
    html += F("<option value=''>(no networks found — refresh)</option>");
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      String esc = htmlEscape(ssid);
      html += "<option value='" + esc + "'>" + esc +
              "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  html += F("</select>"
            "<div class=note>Or type a hidden SSID below.</div>"
            "<label>SSID (override)</label>"
            "<input name=ssid_manual placeholder='leave blank to use the dropdown'>"
            "<label>Password</label>"
            "<input name=pass type=password placeholder='WPA/WPA2 password'>"
            "<button type=submit>Save &amp; connect</button>"
            "</form>");
  WiFi.scanDelete();
  return html;
}

String savedPage(const String& ssid) {
  String html;
  html.reserve(512);
  html += F("<!doctype html><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>CYD WiFi Setup</title>"
            "<style>body{font-family:system-ui,sans-serif;max-width:480px;margin:2em auto;padding:0 1em;background:#111;color:#eee}h1{font-size:1.2em}</style>"
            "<h1>Saved.</h1><p>Device is reconnecting to <b>");
  html += htmlEscape(ssid);
  html += F("</b>. The setup AP will disappear in a few seconds. "
            "Reconnect your phone to your normal WiFi.</p>");
  return html;
}

void handleRoot(AsyncWebServerRequest* req) {
  req->send(200, "text/html", scanPage());
}

void handleSave(AsyncWebServerRequest* req) {
  String ssid = req->hasParam("ssid_manual", true) && req->getParam("ssid_manual", true)->value().length()
                  ? req->getParam("ssid_manual", true)->value()
                  : (req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : String());
  String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : String();

  if (!ssid.length()) {
    req->send(400, "text/plain", "ssid required");
    return;
  }

  if (g_settings) g_settings->setWifi(ssid, pass);
  req->send(200, "text/html", savedPage(ssid));
  g_pendingReconnect = true;
}

void handleCaptiveProbe(AsyncWebServerRequest* req) {
  // Tell phones the network is captive so they pop the portal.
  req->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
}

}  // namespace

void begin(SettingsStore& settings) {
  g_settings = &settings;
  if (!g_server) g_server = new AsyncWebServer(80);
  if (!g_dns)    g_dns    = new DNSServer();

  // Wildcard hijack — every DNS query resolves to the AP IP.
  g_dns->setErrorReplyCode(DNSReplyCode::NoError);
  g_dns->start(53, "*", WiFi.softAPIP());

  g_server->on("/",     HTTP_GET,  handleRoot);
  g_server->on("/save", HTTP_POST, handleSave);

  // Captive-portal probe URLs across vendors. Returning a redirect makes
  // the phone's OS pop the "Sign in to network" sheet, which auto-loads "/".
  g_server->on("/generate_204",       HTTP_GET, handleCaptiveProbe); // Android
  g_server->on("/gen_204",            HTTP_GET, handleCaptiveProbe);
  g_server->on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);// Apple
  g_server->on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  g_server->on("/connecttest.txt",    HTTP_GET, handleCaptiveProbe); // Windows
  g_server->on("/ncsi.txt",           HTTP_GET, handleCaptiveProbe);
  g_server->onNotFound(handleCaptiveProbe);

  g_server->begin();
}

void end() {
  if (g_dns) { g_dns->stop(); delete g_dns; g_dns = nullptr; }
  if (g_server) { g_server->end(); delete g_server; g_server = nullptr; }
  g_settings = nullptr;
  g_pendingReconnect = false;
}

void loop() {
  if (g_dns) g_dns->processNextRequest();
  if (g_pendingReconnect && g_settings) {
    g_pendingReconnect = false;
    SettingsStore* s = g_settings;
    end();                       // tear down portal before mode switch
    wifi_mgr::retrySta(*s);      // may bring AP back up on failure
  }
}

}  // namespace captive_portal
