#include "quote_fetcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <time.h>
#include <vector>

#include "../config.h"
#include "../display/fs_littlefs.h"
#include "../settings/settings_store.h"
#include "../ui/logos_data.h"
#include "quote_store.h"

namespace {

// Temporary diagnostic: ask stock-api for a synthetic red/green/blue PNG
// for any non-embedded logo. If this renders, runtime PNG drawing works and
// real logos need API-side contrast normalization.
static constexpr bool kLogoApiTestMode = true;

String logoPath(const String& symbol) {
  String s = symbol;
  s.toUpperCase();
  String clean;
  clean.reserve(s.length());
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) clean += c;
    else clean += '_';
  }
  return String("/logos/") + clean + ".png";
}

bool fetchAndParse(const String& url, const String& token,
                   const JsonDocument& filter, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(client, url)) return false;

  http.useHTTP10(true);                              // avoid chunked transfer
  http.addHeader("Accept-Encoding", "identity");     // avoid gzip/deflate
  // Cloudflare bot-fight rejects empty/default User-Agents — required.
  http.addHeader("User-Agent",       cfg::API_USER_AGENT);
  if (token.length()) {
    http.addHeader("Authorization", String("Bearer ") + token);
  }

  int code = http.GET();
  if (code != 200) {
    log_w("HTTP %d %s", code, url.c_str());
    http.end();
    return false;
  }

  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    log_w("json parse: %s", err.c_str());
    return false;
  }
  return true;
}

bool logoExists(const String& path) {
  fs_littlefs::Guard g;
  return LittleFS.exists(path);
}

bool fetchLogo(const String& symbol, const String& token) {
  if (logos_data::find(symbol.c_str())) return true;

  String path = logoPath(symbol);
  if (!kLogoApiTestMode && logoExists(path)) return true;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);

  String url = String(cfg::API_BASE) + "/logo/" + symbol;
  if (kLogoApiTestMode) url += "?test=1";
  if (!http.begin(client, url)) return false;
  http.useHTTP10(true);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", cfg::API_USER_AGENT);
  if (token.length()) http.addHeader("Authorization", String("Bearer ") + token);

  int code = http.GET();
  if (code != 200) {
    log_w("[%s] logo HTTP %d", symbol.c_str(), code);
    http.end();
    return false;
  }

  String tmp = path + ".tmp";
  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  size_t total = 0;
  bool ok = true;

  {
    fs_littlefs::Guard g;
    LittleFS.mkdir("/logos");
    if (kLogoApiTestMode) LittleFS.remove(path);
    LittleFS.remove(tmp);
    File f = LittleFS.open(tmp, "w");
    if (!f) {
      log_w("[%s] logo cache open failed", symbol.c_str());
      http.end();
      return false;
    }

    uint32_t lastData = millis();
    while (http.connected() && millis() - lastData < 10000) {
      int avail = stream->available();
      if (!avail) {
        delay(1);
        continue;
      }
      size_t want = avail < (int)sizeof(buf) ? (size_t)avail : sizeof(buf);
      int got = stream->readBytes(buf, want);
      if (got <= 0) break;
      lastData = millis();

      if (f.write(buf, got) != (size_t)got) {
        ok = false;
        break;
      }
      total += (size_t)got;
      if (total > 65536) {
        log_w("[%s] logo too large", symbol.c_str());
        ok = false;
        break;
      }
    }

    f.close();
    if (ok && total > 8) {
      uint8_t sig[8] = {};
      File r = LittleFS.open(tmp, "r");
      ok = r && r.read(sig, sizeof(sig)) == sizeof(sig) &&
           sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' &&
           sig[3] == 'G' && sig[4] == '\r' && sig[5] == '\n' &&
           sig[6] == 0x1a && sig[7] == '\n';
      if (r) r.close();
    } else {
      ok = false;
    }

    if (ok) {
      LittleFS.remove(path);
      ok = LittleFS.rename(tmp, path);
    } else {
      LittleFS.remove(tmp);
    }
  }

  http.end();
  if (ok) log_i("[%s] logo cached: %u bytes", symbol.c_str(), (unsigned)total);
  else    log_w("[%s] logo cache failed", symbol.c_str());
  return ok;
}

}  // namespace

namespace fetcher {

// GET /stock/{symbol} -> { last, change_pct, closes: [...] }
bool fetchQuotes(SettingsStore& settings, QuoteStore& store) {
  auto syms = settings.symbols();
  if (syms.empty()) return false;
  String token = settings.apiKey();
  if (!token.length()) {
    log_w("no api token configured");
    return false;
  }

  JsonDocument filter;
  filter["last"]       = true;
  filter["change_pct"] = true;
  filter["closes"]     = true;

  std::vector<Quote> out;
  out.reserve(syms.size());

  for (const auto& sym : syms) {
    Quote q;
    q.symbol = sym;

    String url = String(cfg::API_BASE) + "/stock/" + sym;
    JsonDocument doc;
    if (fetchAndParse(url, token, filter, doc)) {
      q.last      = doc["last"]       | NAN;
      q.changePct = doc["change_pct"] | NAN;
      JsonArrayConst closes = doc["closes"].as<JsonArrayConst>();
      if (!closes.isNull()) {
        q.sparkline.reserve(closes.size());
        for (JsonVariantConst v : closes) {
          float c = v | NAN;
          if (!isnan(c) && c > 0) q.sparkline.push_back(c);
        }
      }
      q.fresh = !isnan(q.last) && !isnan(q.changePct);
      if (q.fresh) {
        log_i("[%s] %.2f (%+.2f%%)", sym.c_str(), q.last, q.changePct);
      } else {
        log_w("[%s] missing last/change_pct", sym.c_str());
      }
    }
    out.push_back(q);
    fetchLogo(sym, token);
    vTaskDelay(pdMS_TO_TICKS(150));  // gentle on the API between calls
  }

  time_t now;
  time(&now);
  store.setQuotes(std::move(out), now);
  return true;
}

// GET /history/{symbol}?range=<value> ->
//   { interval: "intraday"|"daily", points: [{ts, last}, ...] }
//
// The caller (UI) passes the generation id from QuoteStore::requestHistory
// and we re-check it right before storing — that's how a tap on a
// different range button cancels this fetch even though we can't
// interrupt the blocking HTTPS read mid-flight.
bool fetchHistory(SettingsStore& settings, QuoteStore& store,
                  const String& symbol, const String& range,
                  uint32_t gen) {
  if (!symbol.length()) return false;
  String token = settings.apiKey();
  if (!token.length()) return false;

  JsonDocument filter;
  filter["interval"]          = true;
  filter["points"][0]["last"] = true;
  filter["points"][0]["ts"]   = true;

  String url = String(cfg::API_BASE) + "/history/" + symbol + "?range=" + range;
  JsonDocument doc;
  if (!fetchAndParse(url, token, filter, doc)) {
    if (store.historyGenCurrent(gen)) store.setHistoryError(true);
    return false;
  }

  // If a newer request was issued while we were blocked on the HTTPS
  // round-trip, drop this result on the floor — the next loop iteration
  // will pick up the newer request.
  if (!store.historyGenCurrent(gen)) return false;

  String interval = doc["interval"] | "";
  JsonArrayConst pts = doc["points"].as<JsonArrayConst>();
  if (pts.isNull() || pts.size() == 0) {
    log_w("[%s] history: empty", symbol.c_str());
    store.setHistoryError(true);
    return false;
  }

  std::vector<float>  closes;
  std::vector<time_t> timestamps;
  closes.reserve(pts.size());
  timestamps.reserve(pts.size());
  for (JsonVariantConst p : pts) {
    float v = p["last"] | NAN;
    if (isnan(v) || v <= 0) continue;
    time_t t = (time_t)(p["ts"] | (long long)0);
    closes.push_back(v);
    timestamps.push_back(t);
  }
  if (closes.empty()) {
    log_w("[%s] history: no valid points", symbol.c_str());
    store.setHistoryError(true);
    return false;
  }

  if (closes.size() > cfg::HISTORY_POINTS) {
    size_t drop = closes.size() - cfg::HISTORY_POINTS;
    closes.erase(closes.begin(), closes.begin() + drop);
    timestamps.erase(timestamps.begin(), timestamps.begin() + drop);
  }

  float first = closes.front();
  float last  = closes.back();
  float pct   = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  time_t first_ts = timestamps.empty() ? 0 : timestamps.front();
  time_t last_ts  = timestamps.empty() ? 0 : timestamps.back();
  log_i("[%s] history %s: interval=%s pts=%u first=%.2f last=%.2f change=%+.2f%% ts=%ld..%ld",
        symbol.c_str(), range.c_str(), interval.c_str(),
        (unsigned)closes.size(), first, last, pct,
        (long)first_ts, (long)last_ts);

  History h;
  h.symbol     = symbol;
  h.range      = range;
  h.interval   = interval;
  h.closes     = std::move(closes);
  h.timestamps = std::move(timestamps);
  if (!store.historyGenCurrent(gen)) return false;
  store.setHistory(std::move(h));
  store.setHistoryError(false);
  return true;
}

}  // namespace fetcher
