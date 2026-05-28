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

// Parses the PNG IHDR width/height from a LittleFS file. Returns false
// if the file is missing, too short, or doesn't start with the PNG
// signature. Used to invalidate cached logos served by an older API
// build that returned a different resolution.
bool logoDims(const String& path, uint32_t& w, uint32_t& h) {
  fs_littlefs::Guard g;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t hdr[24];
  size_t got = f.readBytes(reinterpret_cast<char*>(hdr), sizeof(hdr));
  f.close();
  if (got != sizeof(hdr)) return false;
  // PNG signature: 89 50 4E 47 0D 0A 1A 0A
  static const uint8_t kSig[8] = {0x89, 0x50, 0x4E, 0x47,
                                  0x0D, 0x0A, 0x1A, 0x0A};
  if (memcmp(hdr, kSig, 8) != 0) return false;
  // bytes 16..23 are IHDR width/height (big-endian uint32).
  w = (uint32_t(hdr[16]) << 24) | (uint32_t(hdr[17]) << 16) |
      (uint32_t(hdr[18]) << 8)  |  uint32_t(hdr[19]);
  h = (uint32_t(hdr[20]) << 24) | (uint32_t(hdr[21]) << 16) |
      (uint32_t(hdr[22]) << 8)  |  uint32_t(hdr[23]);
  return true;
}

bool fetchLogo(const String& symbol, const String& token) {
  // Embedded ARGB8888 logos in `logos_data` bypass the runtime PNG path
  // entirely; nothing to do at the fetch layer.
  if (logos_data::find(symbol.c_str())) return true;

  String path = logoPath(symbol);
  // Cache check: skip the HTTP round-trip when we already have a
  // matching file. A cached PNG counts as a hit only if its IHDR
  // dimensions match the size we'd request now (48x48). Anything else
  // is an older build's leftover and gets re-fetched. In LOGO_TEST_MODE
  // we always re-fetch — the server sends Cache-Control: no-store and
  // we don't want a cached file masking the rendering pipeline.
  if (!cfg::LOGO_TEST_MODE && logoExists(path)) {
    uint32_t cw = 0, ch = 0;
    if (logoDims(path, cw, ch) && cw == 48 && ch == 48) return true;
    log_i("[logo] %s cache stale dims=%ux%u, refetching",
          symbol.c_str(), (unsigned)cw, (unsigned)ch);
  }

  // Request 48x48 PNGs so lodepng's transient decode peak stays under
  // the ~40 KB largest-contiguous heap ceiling we see after WiFi+TLS.
  // The cache slot then mounts at native 48x48 and LVGL bilinearly
  // scales to the 38 px display slot — same code path as embedded
  // logos, no homebrew resample.
  String url = String(cfg::API_BASE) + "/logo/" + symbol + "?size=48";
  if (cfg::LOGO_TEST_MODE) url += "&test=1";
  log_i("[logo] %s GET %s", symbol.c_str(), url.c_str());

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    log_w("[logo] %s http.begin failed", symbol.c_str());
    return false;
  }
  http.useHTTP10(true);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", cfg::API_USER_AGENT);
  if (token.length()) http.addHeader("Authorization", String("Bearer ") + token);

  int code = http.GET();
  log_i("[logo] %s HTTP %d", symbol.c_str(), code);
  if (code != 200) {
    http.end();
    return false;
  }

  // Buffer the HTTP body in RAM rather than streaming directly to LittleFS.
  // The previous version held fs_littlefs::Guard across the entire HTTP
  // receive loop (up to 10 s on a slow connection), which blocked the UI
  // task whenever it tried to read another logo from LittleFS — touch
  // appeared frozen for the duration of every per-symbol logo fetch. With
  // the body in RAM the guard is only held for the quick write + rename,
  // never during the network read.
  std::vector<uint8_t> body;
  body.reserve(8192);
  static constexpr size_t kLogoMaxBytes = 65536;

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[512];
  bool ok = true;
  uint32_t lastData = millis();
  while (http.connected() && millis() - lastData < 10000) {
    int avail = stream->available();
    if (!avail) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    size_t want = avail < (int)sizeof(buf) ? (size_t)avail : sizeof(buf);
    int got = stream->readBytes(buf, want);
    if (got <= 0) break;
    lastData = millis();
    if (body.size() + (size_t)got > kLogoMaxBytes) {
      log_w("[logo] %s too large (>%u bytes)",
            symbol.c_str(), (unsigned)kLogoMaxBytes);
      ok = false;
      break;
    }
    body.insert(body.end(), buf, buf + got);
  }
  http.end();
  log_i("[logo] %s bytes=%u", symbol.c_str(), (unsigned)body.size());

  if (!ok || body.size() < 8) {
    log_w("[logo] %s body invalid", symbol.c_str());
    return false;
  }
  // PNG signature check before we touch the filesystem.
  const uint8_t* s = body.data();
  if (!(s[0] == 0x89 && s[1] == 'P' && s[2] == 'N' && s[3] == 'G' &&
        s[4] == '\r' && s[5] == '\n' && s[6] == 0x1a && s[7] == '\n')) {
    log_w("[logo] %s not a PNG (sig=%02X %02X %02X %02X)",
          symbol.c_str(), s[0], s[1], s[2], s[3]);
    return false;
  }

  String tmp = path + ".tmp";
  log_i("[logo] %s fs.lock", symbol.c_str());
  {
    // Held only across the (fast) write + rename — never the HTTP recv.
    fs_littlefs::Guard g;

    // Bail out if the partition is too close to full. The Arduino
    // LittleFS allocator divides by `lfs->cfg->block_count` and the
    // partition can wedge into a state where that field reads back as
    // 0 after a failed allocation — the next allocation crashes the
    // firmware with EXCCAUSE 6 (IntegerDivideByZero) deep inside
    // lfs_alloc. We've decoded that trace already; the only way out
    // is to never trigger an allocation when there's no headroom.
    // Conservative threshold: refuse the write if less than the body
    // size + 8 KB slack is free.
    size_t total = LittleFS.totalBytes();
    size_t used  = LittleFS.usedBytes();
    size_t avail = (total > used) ? (total - used) : 0;
    if (avail < body.size() + 8192) {
      log_w("[logo] %s skip (fs full: avail=%u total=%u, need=%u)",
            symbol.c_str(), (unsigned)avail, (unsigned)total,
            (unsigned)(body.size() + 8192));
      return false;
    }
    log_i("[logo] %s fs.space avail=%u total=%u",
          symbol.c_str(), (unsigned)avail, (unsigned)total);

    log_i("[logo] %s fs.mkdir", symbol.c_str());
    LittleFS.mkdir("/logos");
    log_i("[logo] %s fs.remove_tmp", symbol.c_str());
    LittleFS.remove(tmp);
    log_i("[logo] %s fs.open_tmp", symbol.c_str());
    File f = LittleFS.open(tmp, "w");
    if (!f) {
      log_w("[logo] %s cache open failed", symbol.c_str());
      return false;
    }
    log_i("[logo] %s fs.write %u", symbol.c_str(), (unsigned)body.size());
    size_t wrote = f.write(body.data(), body.size());
    log_i("[logo] %s fs.close (wrote=%u)", symbol.c_str(), (unsigned)wrote);
    f.close();
    if (wrote != body.size()) {
      log_w("[logo] %s write short: %u/%u", symbol.c_str(),
            (unsigned)wrote, (unsigned)body.size());
      LittleFS.remove(tmp);
      return false;
    }
    log_i("[logo] %s fs.remove_old", symbol.c_str());
    LittleFS.remove(path);   // unconditional overwrite — no stale cache
    log_i("[logo] %s fs.rename", symbol.c_str());
    ok = LittleFS.rename(tmp, path);
    log_i("[logo] %s fs.done ok=%d", symbol.c_str(), (int)ok);
    if (!ok) LittleFS.remove(tmp);
  }

  if (ok) log_i("[logo] %s cached %s (%u bytes)",
                symbol.c_str(), path.c_str(), (unsigned)body.size());
  else    log_w("[logo] %s rename failed", symbol.c_str());
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
  filter["session_open"]      = true;   // range=1d only; absent otherwise
  filter["session_close"]     = true;
  filter["points"][0]["last"] = true;
  filter["points"][0]["ts"]   = true;

  String url = String(cfg::API_BASE) + "/history/" + symbol + "?range=" + range +
               "&limit=" + String(cfg::HISTORY_POINTS);
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

  // Downsample (don't truncate) when the API returns more points than the
  // chart can hold. The earlier "drop the head, keep the tail" approach
  // collapsed a 6-month daily series (~120 pts) into the last ~30 days,
  // which made the 6M tab show the same date window as 1M. Uniform-index
  // sampling preserves the first and last points (so the displayed gain
  // / loss percentage matches the full requested window) and spaces the
  // rest evenly across the original time range.
  if (closes.size() > cfg::HISTORY_POINTS) {
    const size_t total = closes.size();
    const size_t kept  = cfg::HISTORY_POINTS;
    std::vector<float>  ds_closes;
    std::vector<time_t> ds_ts;
    ds_closes.reserve(kept);
    ds_ts.reserve(kept);
    for (size_t i = 0; i < kept; ++i) {
      size_t src = (i * (total - 1)) / (kept - 1);
      ds_closes.push_back(closes[src]);
      ds_ts.push_back(timestamps[src]);
    }
    closes     = std::move(ds_closes);
    timestamps = std::move(ds_ts);
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
  h.symbol        = symbol;
  h.range         = range;
  h.interval      = interval;
  h.closes        = std::move(closes);
  h.timestamps    = std::move(timestamps);
  h.session_open  = (time_t)(doc["session_open"]  | (long long)0);
  h.session_close = (time_t)(doc["session_close"] | (long long)0);
  if (!store.historyGenCurrent(gen)) return false;
  store.setHistory(std::move(h));
  store.setHistoryError(false);
  return true;
}

}  // namespace fetcher
