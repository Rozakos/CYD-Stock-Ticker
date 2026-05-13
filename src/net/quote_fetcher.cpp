#include "quote_fetcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <algorithm>
#include <time.h>
#include <vector>

#include "../config.h"
#include "../settings/settings_store.h"
#include "quote_store.h"

namespace {

// Confirmed working in another deployment using this same RapidAPI host.
constexpr const char* HISTORY_PATH = "/api/v2/markets/stock/history";

// JSON key/value tolerance: closes may arrive as numbers or as strings.
float toFloat(JsonVariantConst v) {
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || !*s) return NAN;
    char* end = nullptr;
    float f = strtof(s, &end);
    return (end == s) ? NAN : f;
  }
  if (v.is<float>() || v.is<double>() || v.is<int>() || v.is<long>()) {
    return v.as<float>();
  }
  return NAN;
}

bool extractLastTwo(JsonVariantConst arr, float& last, float& prev) {
  last = prev = NAN;
  if (!arr.is<JsonArrayConst>()) return false;
  JsonArrayConst a = arr.as<JsonArrayConst>();
  if (a.size() < 2) return false;
  for (int i = (int)a.size() - 1; i >= 0; --i) {
    float c = toFloat(a[i]["close"]);
    if (isnan(c) || c <= 0) continue;
    if (isnan(last))      last = c;
    else                  { prev = c; break; }
  }
  return !isnan(last) && !isnan(prev);
}

std::vector<float> extractAllCloses(JsonVariantConst arr) {
  std::vector<float> out;
  if (!arr.is<JsonArrayConst>()) return out;
  for (JsonVariantConst row : arr.as<JsonArrayConst>()) {
    float c = toFloat(row["close"]);
    if (!isnan(c) && c > 0) out.push_back(c);
  }
  return out;
}

// Filter keeps only the `close` field across all three known schemas, so the
// streamed body never lands fully in RAM.
void buildCloseFilter(JsonDocument& filter) {
  filter["body"][0]["close"]            = true;  // schema 1: body[]
  filter["data"]["items"][0]["close"]   = true;  // schema 2: data.items[]
  filter["data"]["prices"][0]["close"]  = true;  // schema 3: data.prices[]
}

bool fetchAndParse(const String& url, const String& key, JsonDocument& doc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(client, url)) return false;

  http.useHTTP10(true);                            // avoid chunked transfer
  http.addHeader("Accept-Encoding", "identity");   // avoid gzip/deflate
  http.addHeader("x-rapidapi-key",  key);
  http.addHeader("x-rapidapi-host", cfg::RAPID_HOST);

  int code = http.GET();
  if (code != 200) {
    log_w("HTTP %d %s", code, url.c_str());
    http.end();
    return false;
  }

  JsonDocument filter;
  buildCloseFilter(filter);
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    log_w("json parse: %s", err.c_str());
    return false;
  }
  return true;
}

bool extractFromAnySchema(JsonDocument& doc, float& last, float& prev) {
  return extractLastTwo(doc["body"], last, prev)
      || extractLastTwo(doc["data"]["items"], last, prev)
      || extractLastTwo(doc["data"]["prices"], last, prev);
}

std::vector<float> extractClosesFromAnySchema(JsonDocument& doc) {
  std::vector<float> v = extractAllCloses(doc["body"]);
  if (!v.empty()) return v;
  v = extractAllCloses(doc["data"]["items"]);
  if (!v.empty()) return v;
  return extractAllCloses(doc["data"]["prices"]);
}

}  // namespace

namespace fetcher {

bool fetchQuotes(SettingsStore& settings, QuoteStore& store) {
  auto syms = settings.symbols();
  if (syms.empty()) return false;
  String key = settings.apiKey();
  if (!key.length()) {
    log_w("no api key configured");
    return false;
  }

  std::vector<Quote> out;
  out.reserve(syms.size());

  for (const auto& sym : syms) {
    Quote q;
    q.symbol = sym;

    String url = String("https://") + cfg::RAPID_HOST + HISTORY_PATH +
                 "?symbol=" + sym +
                 "&interval=1d&limit=" + String(cfg::SPARKLINE_POINTS);

    JsonDocument doc;
    if (fetchAndParse(url, key, doc)) {
      std::vector<float> closes = extractClosesFromAnySchema(doc);
      if (closes.size() >= 2) {
        q.last = closes.back();
        float prev = closes[closes.size() - 2];
        q.changePct = (prev != 0.0f) ? ((q.last - prev) / prev) * 100.0f : NAN;
        q.fresh = !isnan(q.last) && !isnan(q.changePct);
        q.sparkline = std::move(closes);
        log_i("[%s] %.2f (%+.2f%%)", sym.c_str(), q.last, q.changePct);
      } else {
        log_w("[%s] no valid closes in response", sym.c_str());
      }
    }
    out.push_back(q);
    vTaskDelay(pdMS_TO_TICKS(200));  // gentle on the API between calls
  }

  time_t now;
  time(&now);
  store.setQuotes(std::move(out), now);
  return true;
}

bool fetchHistory(SettingsStore& settings, QuoteStore& store,
                  const String& symbol) {
  if (!symbol.length()) return false;
  String key = settings.apiKey();
  if (!key.length()) return false;

  String url = String("https://") + cfg::RAPID_HOST + HISTORY_PATH +
               "?symbol=" + symbol +
               "&interval=1d&limit=" + String(cfg::HISTORY_POINTS);

  JsonDocument doc;
  if (!fetchAndParse(url, key, doc)) return false;

  std::vector<float> closes = extractClosesFromAnySchema(doc);
  if (closes.empty()) {
    log_w("[%s] history: no closes", symbol.c_str());
    return false;
  }

  // Keep at most HISTORY_POINTS from the end (most recent).
  if (closes.size() > cfg::HISTORY_POINTS) {
    closes.erase(closes.begin(),
                 closes.begin() + (closes.size() - cfg::HISTORY_POINTS));
  }

  History h;
  h.symbol = symbol;
  h.closes = std::move(closes);
  store.setHistory(std::move(h));
  return true;
}

}  // namespace fetcher
