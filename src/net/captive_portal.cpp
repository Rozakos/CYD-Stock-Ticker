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
String           g_scan_options;

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

void refreshScanOptions() {
  log_i("[portal] scanning nearby WiFi networks");
  int n = WiFi.scanNetworks(false, true);

  String opts;
  opts.reserve(1024);
  if (n <= 0) {
    opts += F("<option value=''>(no networks found - type SSID below)</option>");
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      if (!ssid.length()) continue;
      String esc = htmlEscape(ssid);
      opts += "<option value='" + esc + "'>" + esc +
              "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
    }
  }
  WiFi.scanDelete();
  g_scan_options = opts;
  log_i("[portal] scan complete: raw=%d options_bytes=%u",
        n, (unsigned)g_scan_options.length());
}

String scanPage() {
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
  if (g_scan_options.length()) html += g_scan_options;
  else                         html += F("<option value=''>(type SSID below)</option>");
  html += F("</select>"
            "<div class=note>Or type a hidden SSID below.</div>"
            "<label>SSID (override)</label>"
            "<input name=ssid_manual placeholder='leave blank to use the dropdown'>"
            "<label>Password</label>"
            "<input name=pass type=password placeholder='WPA/WPA2 password'>"
            "<button type=submit>Save &amp; connect</button>"
            "</form>");
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
  log_i("[portal] GET / from %s", req->client()->remoteIP().toString().c_str());
  req->send(200, "text/html", scanPage());
}

void handleSave(AsyncWebServerRequest* req) {
  log_i("[portal] POST /save from %s", req->client()->remoteIP().toString().c_str());
  String ssid = req->hasParam("ssid_manual", true) && req->getParam("ssid_manual", true)->value().length()
                  ? req->getParam("ssid_manual", true)->value()
                  : (req->hasParam("ssid", true) ? req->getParam("ssid", true)->value() : String());
  String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : String();

  if (!ssid.length()) {
    log_w("[portal] /save rejected: empty ssid");
    req->send(400, "text/plain", "ssid required");
    return;
  }

  log_i("[portal] saving wifi ssid='%s' pass_len=%u",
        ssid.c_str(), (unsigned)pass.length());
  if (g_settings) g_settings->setWifi(ssid, pass);
  req->send(200, "text/html", savedPage(ssid));
  g_pendingReconnect = true;
}

void handleCaptiveProbe(AsyncWebServerRequest* req) {
  log_i("[portal] probe %s from %s",
        req->url().c_str(), req->client()->remoteIP().toString().c_str());
  req->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
}

}  // namespace

void begin(SettingsStore& settings) {
  g_settings = &settings;
  if (!g_server) g_server = new AsyncWebServer(80);
  if (!g_dns)    g_dns    = new DNSServer();

  log_i("[portal] begin on AP IP %s", WiFi.softAPIP().toString().c_str());

  // Wildcard hijack: every DNS query resolves to the AP IP.
  g_dns->setErrorReplyCode(DNSReplyCode::NoError);
  g_dns->start(53, "*", WiFi.softAPIP());

  // Request callbacks run in async_tcp's task. Keep slow radio scans out
  // of that task or the AsyncTCP watchdog can reset the board when phones
  // issue captive-portal probes.
  refreshScanOptions();

  g_server->on("/",     HTTP_GET,  handleRoot);
  g_server->on("/save", HTTP_POST, handleSave);

  // Captive-portal probe URLs across vendors. Returning a redirect makes
  // the phone's OS pop the "Sign in to network" sheet, which auto-loads "/".
  g_server->on("/generate_204",        HTTP_GET, handleCaptiveProbe); // Android
  g_server->on("/gen_204",             HTTP_GET, handleCaptiveProbe);
  g_server->on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe); // Apple
  g_server->on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  g_server->on("/connecttest.txt",     HTTP_GET, handleCaptiveProbe); // Windows
  g_server->on("/ncsi.txt",            HTTP_GET, handleCaptiveProbe);
  g_server->onNotFound(handleCaptiveProbe);

  g_server->begin();
  log_i("[portal] server started");
}

void end() {
  log_i("[portal] end");
  if (g_dns) { g_dns->stop(); delete g_dns; g_dns = nullptr; }
  if (g_server) { g_server->end(); delete g_server; g_server = nullptr; }
  g_settings = nullptr;
  g_pendingReconnect = false;
  g_scan_options = "";
}

void loop() {
  if (g_dns) g_dns->processNextRequest();
  if (g_pendingReconnect && g_settings) {
    g_pendingReconnect = false;
    SettingsStore* s = g_settings;   // end() clears g_settings — capture first
    log_i("[portal] reconnect requested");
    end();                       // tear down portal before mode switch
    wifi_mgr::retrySta(*s);      // blocks ~15s; brings AP back up on failure
    // If the new credentials didn't take, retrySta has re-started the setup
    // AP but NOT the DNS/web server we just tore down. Re-arm the portal so
    // the user can actually load the page and try again (previously this was
    // skipped, leaving a dead AP and a frozen setup screen).
    if (!wifi_mgr::connected() && wifi_mgr::apActive()) {
      log_w("[portal] reconnect failed — re-arming setup portal");
      begin(*s);
    }
  }
}

}  // namespace captive_portal
