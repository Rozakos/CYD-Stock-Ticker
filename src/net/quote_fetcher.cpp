#include "quote_fetcher.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>

#include <time.h>
#include <algorithm>
#include <atomic>
#include <vector>

#include "../config.h"
#include "../display/fs_littlefs.h"
#include "../settings/settings_store.h"
#include "../ui/logos_data.h"
#include "quote_store.h"
#include "tls_ca_cert.h"

namespace {

WiFiClientSecure g_apiClient;
HTTPClient g_apiHttp;
bool g_apiTransportReady = false;

void prepareApiTransport() {
  if (g_apiTransportReady) return;
  if (cfg::API_TLS_VERIFY) {
    uint32_t started = millis();
    while (time(nullptr) < 1704067200 && millis() - started < 10000) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (time(nullptr) < 1704067200) {
      log_w("[tls] clock not synchronized; certificate validation may fail");
    }
    g_apiClient.setCACert(API_TLS_CA_CERT);
    log_i("[tls] pinned GTS WE1 CA; verification required");
  } else {
    g_apiClient.setInsecure();
    log_w("[tls] certificate verification disabled");
  }
  g_apiHttp.setTimeout(10000);
  g_apiHttp.setReuse(true);
  g_apiHttp.setUserAgent(cfg::API_USER_AGENT);
  g_apiTransportReady = true;
}

void finishApiRequest() {
  // Clears per-request state while reuse keeps the underlying TLS socket open.
  g_apiHttp.end();
}

void abortApiRequest() {
  g_apiHttp.setReuse(false);
  g_apiHttp.end();
  g_apiClient.stop();
  g_apiHttp.setReuse(true);
}

// Set whenever any request in the current fetch cycle gets a real HTTP
// status back; cleared at the top of fetchQuotes. See header.
bool g_sawHttpResponse = false;

int startApiGet(const String& url, const String& token) {
  prepareApiTransport();

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    bool reused = g_apiClient.connected();
    if (!g_apiHttp.begin(g_apiClient, url)) {
      log_w("[http] begin failed: %s", url.c_str());
      return HTTPC_ERROR_CONNECTION_REFUSED;
    }

    // This core disables reuse in HTTP/1.0 mode. The API returns
    // Content-Length for identity encoding, so HTTP/1.1 is safe here.
    g_apiHttp.useHTTP10(false);
    g_apiHttp.setReuse(true);
    g_apiHttp.addHeader("Accept-Encoding", "identity");
    if (token.length()) {
      g_apiHttp.addHeader("Authorization", String("Bearer ") + token);
    }

    uint32_t started = millis();
    int code = g_apiHttp.GET();
    if (code > 0) g_sawHttpResponse = true;   // server reachable (any status)
    log_i("[http] %s %s -> %d in %ums", reused ? "reuse" : "connect",
          url.c_str(), code, (unsigned)(millis() - started));
    if (!reused && code >= 0) {
      log_i("[tls] connected heap free=%u maxblk=%u",
            (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
    }
    if (code >= 0 || !reused || attempt != 0) return code;

    log_w("[http] stale keep-alive (%d), reconnecting once", code);
    abortApiRequest();
  }
  return HTTPC_ERROR_CONNECTION_REFUSED;
}

// Set when fetchLogo skips a download because no contiguous block was left
// even for the body buffer; fetchQuotes then drops the persistent connection
// at the end of the cycle so the heap recovers before the next retry.
bool g_logoFetchStarved = false;

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
  int code = startApiGet(url, token);

  if (code != 200) {
    log_w("HTTP %d %s", code, url.c_str());
    abortApiRequest();
    return false;
  }

  DeserializationError err = deserializeJson(
      doc, g_apiHttp.getStream(), DeserializationOption::Filter(filter));
  if (err) {
    abortApiRequest();
    log_w("json parse: %s", err.c_str());
    return false;
  }
  finishApiRequest();
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

// A complete PNG ends with the IEND chunk (type+CRC are constant:
// 49 45 4E 44 AE 42 60 82). Checking for it catches downloads that were
// truncated mid-transfer but still start with a valid signature/IHDR — those
// otherwise pass the dims check, get cached, and decode to a black/garbage
// blob that never gets re-fetched.
const uint8_t kPngIend[8] = {0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

bool bytesComplete(const uint8_t* d, size_t n) {
  if (n < 16) return false;
  return memcmp(d + n - 8, kPngIend, 8) == 0;
}

// Same check against a cached file — reads only the trailing 8 bytes.
bool logoFileComplete(const String& path) {
  fs_littlefs::Guard g;
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz < 16) { f.close(); return false; }
  f.seek(sz - 8);
  uint8_t tail[8];
  size_t got = f.readBytes(reinterpret_cast<char*>(tail), sizeof(tail));
  f.close();
  return got == sizeof(tail) && memcmp(tail, kPngIend, 8) == 0;
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
    bool dimsOk = logoDims(path, cw, ch) && cw == 48 && ch == 48;
    if (dimsOk && logoFileComplete(path)) return true;
    // Wrong size (older build) OR truncated/corrupt (no IEND) — drop it and
    // re-download so a bad cache can't wedge a symbol on a black icon.
    log_i("[logo] %s cache invalid dims=%ux%u complete=%d, refetching",
          symbol.c_str(), (unsigned)cw, (unsigned)ch, (int)logoFileComplete(path));
  }

  // TLS is already resident in the persistent client. Keep enough contiguous
  // space for the logo body buffer so quotes remain the priority under
  // pressure. 12 KB, not 16: the body buffer is sized from Content-Length
  // (logos are 1–4 KB), and a fresh TLS reconnect leaves the largest block
  // at ~15.3 KB — a 16 KB gate on that state skipped the download, released
  // the connection to recover, reconnected next cycle back to ~15.3 KB, and
  // looped forever (new symbols never got their logo; the 20 s reconnect
  // churn also showed up as UI flicker).
  if (ESP.getMaxAllocHeap() < 12000) {
    g_logoFetchStarved = true;
    log_w("[logo] %s skip download (low heap: free=%u maxblk=%u)",
          symbol.c_str(), (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
    return false;
  }

  // Request 48x48 PNGs so lodepng's transient decode peak stays under
  // the ~40 KB largest-contiguous heap ceiling we see after WiFi+TLS.
  // The cache slot then mounts at native 48x48 and LVGL bilinearly
  // scales to the 38 px display slot — same code path as embedded
  // logos, no homebrew resample.
  String url = String(cfg::API_BASE) + "/logo/" + symbol + "?size=48";
  if (cfg::LOGO_TEST_MODE) url += "&test=1";
  log_i("[logo] %s GET %s", symbol.c_str(), url.c_str());

  int code = startApiGet(url, token);
  log_i("[logo] %s HTTP %d", symbol.c_str(), code);
  if (code != 200) {
    abortApiRequest();
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
  static constexpr size_t kLogoMaxBytes = 65536;

  int expected = g_apiHttp.getSize();
  if (expected <= 0 || expected > (int)kLogoMaxBytes) {
    log_w("[logo] %s invalid Content-Length: %d", symbol.c_str(), expected);
    abortApiRequest();
    return false;
  }
  // Reserve exactly the announced size (1–4 KB in practice) instead of a
  // pessimistic 8 KB: doubling-growth re-allocs are what transiently need
  // old+new at once, and this keeps the download viable right after a TLS
  // reconnect when the largest block is ~15 KB.
  body.reserve((size_t)expected);

  WiFiClient* stream = g_apiHttp.getStreamPtr();
  uint8_t buf[512];
  bool ok = true;
  uint32_t lastData = millis();
  while (body.size() < (size_t)expected && millis() - lastData < 10000) {
    int avail = stream->available();
    if (!avail) {
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    size_t want = avail < (int)sizeof(buf) ? (size_t)avail : sizeof(buf);
    size_t remaining = (size_t)expected - body.size();
    if (want > remaining) want = remaining;
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
  if (ok && body.size() == (size_t)expected) finishApiRequest();
  else abortApiRequest();
  log_i("[logo] %s bytes=%u", symbol.c_str(), (unsigned)body.size());

  if (!ok || body.size() != (size_t)expected || body.size() < 8) {
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
  // Reject a truncated transfer (valid header but no IEND) rather than caching
  // a partial file that would render black. Returning false leaves no cache
  // file, so the next refresh retries the download.
  if (!bytesComplete(body.data(), body.size())) {
    log_w("[logo] %s incomplete PNG (no IEND, %u bytes) — not caching, will retry",
          symbol.c_str(), (unsigned)body.size());
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

void releaseApiConnection() {
  if (!g_apiClient.connected()) return;
  log_i("[http] releasing persistent connection (heap recovery)");
  abortApiRequest();
}

bool sawHttpResponseThisCycle() { return g_sawHttpResponse; }

// GET /stocks?symbols=A,B -> { quotes: [{symbol,last,change_pct,closes}, ...] }
bool fetchQuotes(SettingsStore& settings, QuoteStore& store) {
  auto syms = settings.symbols();
  if (syms.empty()) return false;
  String token = settings.apiKey();
  if (!token.length()) {
    log_w("no api token configured");
    return false;
  }

  log_i("[heap] pre-fetch free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT));
  g_sawHttpResponse = false;   // per-cycle transport-liveness signal

  std::vector<Quote> out;
  out.reserve(syms.size());
  for (const auto& sym : syms) {
    Quote q;
    q.symbol = sym;
    out.push_back(std::move(q));
  }

  static constexpr size_t kBatchMaxSymbols = 16;
  for (size_t first = 0; first < syms.size(); first += kBatchMaxSymbols) {
    size_t last = first + kBatchMaxSymbols;
    if (last > syms.size()) last = syms.size();

    String requested;
    for (size_t i = first; i < last; ++i) {
      if (requested.length()) requested += ',';
      requested += syms[i];
    }

    JsonDocument filter;
    filter["quotes"][0]["symbol"]                = true;
    filter["quotes"][0]["last"]                  = true;
    filter["quotes"][0]["change_pct"]            = true;
    filter["quotes"][0]["closes"]                = true;
    filter["quotes"][0]["market_state"]          = true;
    filter["quotes"][0]["pre_market"]            = true;
    filter["quotes"][0]["pre_market_change_pct"] = true;
    filter["quotes"][0]["post_market"]           = true;
    filter["quotes"][0]["post_market_change_pct"]= true;

    String url = String(cfg::API_BASE) + "/stocks?symbols=" + requested;
    JsonDocument doc;
    if (!fetchAndParse(url, token, filter, doc)) continue;

    for (JsonVariantConst item : doc["quotes"].as<JsonArrayConst>()) {
      String symbol = item["symbol"] | "";
      auto found = std::find_if(out.begin(), out.end(), [&](const Quote& q) {
        return q.symbol.equalsIgnoreCase(symbol);
      });
      if (found == out.end()) continue;

      found->last      = item["last"]       | NAN;
      found->changePct = item["change_pct"] | NAN;
      JsonArrayConst closes = item["closes"].as<JsonArrayConst>();
      if (!closes.isNull()) {
        found->sparkline.reserve(closes.size());
        for (JsonVariantConst v : closes) {
          float c = v | NAN;
          if (!isnan(c) && c > 0) found->sparkline.push_back(c);
        }
      }
      found->fresh = !isnan(found->last) && !isnan(found->changePct);

      // Extended-hours: market_state selects which conditional block the API
      // populated. PRE carries pre_market/*_change_pct; POST and CLOSED carry
      // post_market/*_change_pct; REGULAR leaves both null. Both extended
      // prices measure their change versus the regular close (found->last).
      const char* state = item["market_state"] | "";
      if (strcmp(state, "PRE") == 0) {
        found->session      = Session::Pre;
        found->extPrice     = item["pre_market"]            | NAN;
        found->extChangePct = item["pre_market_change_pct"] | NAN;
        found->preMarket    = true;
      } else if (strcmp(state, "REGULAR") == 0) {
        found->session = Session::Regular;
      } else if (strcmp(state, "POST") == 0 || strcmp(state, "CLOSED") == 0) {
        found->session      = strcmp(state, "POST") == 0 ? Session::Post
                                                         : Session::Closed;
        found->extPrice     = item["post_market"]            | NAN;
        found->extChangePct = item["post_market_change_pct"] | NAN;
        found->preMarket    = false;
      } else {
        // Older API without market_state: infer the session from whichever
        // extended field carries a number; both absent leaves Unknown and
        // the UI falls back to the static "MARKETS" title.
        float pre = item["pre_market"] | NAN;
        if (!isnan(pre)) {
          found->session      = Session::Pre;
          found->extPrice     = pre;
          found->extChangePct = item["pre_market_change_pct"] | NAN;
          found->preMarket    = true;
        } else {
          float post = item["post_market"] | NAN;
          if (!isnan(post)) {
            found->session      = Session::Post;
            found->extPrice     = post;
            found->extChangePct = item["post_market_change_pct"] | NAN;
            found->preMarket    = false;
          }
        }
      }

      if (found->fresh) {
        if (found->extended()) {
          log_i("[%s] %.2f (%+.2f%%) %s %.2f (%+.2f%%)", found->symbol.c_str(),
                found->last, found->changePct,
                found->preMarket ? "pre" : "post", found->extPrice,
                found->extChangePct);
        } else {
          log_i("[%s] %.2f (%+.2f%%)", found->symbol.c_str(), found->last,
                found->changePct);
        }
      } else {
        log_w("[%s] missing last/change_pct", found->symbol.c_str());
      }
    }
  }

  for (const auto& sym : syms) {
    fetchLogo(sym, token);
    vTaskDelay(pdMS_TO_TICKS(150));  // gentle on the API between calls
  }
  if (g_logoFetchStarved) {
    // At least one logo download was skipped for lack of contiguous heap.
    // Drop the persistent connection so the TLS buffers come back; the next
    // refresh reconnects and retries the missing logos with fresh headroom.
    g_logoFetchStarved = false;
    log_w("[logo] downloads heap-starved this cycle — releasing connection");
    releaseApiConnection();
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
  filter["window_open"]       = true;   // range=1d + prepost=1 only
  filter["window_close"]      = true;
  filter["prev_close"]        = true;
  filter["market_state"]      = true;
  filter["points"][0]["last"] = true;
  filter["points"][0]["ts"]   = true;

  String url = String(cfg::API_BASE) + "/history/" + symbol + "?range=" + range +
               "&limit=" + String(cfg::HISTORY_POINTS);
  // Extended hours for the intraday view: widens the window to 04:00-20:00 ET
  // and adds window_open/window_close/prev_close/market_state. Also fixes a
  // real bug — before the regular session opens there is no REGULAR data for
  // today, so the plain call falls back to *yesterday's* session and the 1D
  // chart silently showed the wrong day all through pre-market. The server
  // ignores the flag for crypto and for every other range.
  if (range == "1d") url += "&prepost=1";
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
  h.gen           = gen;
  h.range         = range;
  h.interval      = interval;
  h.closes        = std::move(closes);
  h.timestamps    = std::move(timestamps);
  h.session_open  = (time_t)(doc["session_open"]  | (long long)0);
  h.session_close = (time_t)(doc["session_close"] | (long long)0);
  h.window_open   = (time_t)(doc["window_open"]   | (long long)0);
  h.window_close  = (time_t)(doc["window_close"]  | (long long)0);
  h.prev_close    = doc["prev_close"] | NAN;
  h.market_state  = doc["market_state"].as<const char*>() ? doc["market_state"].as<const char*>() : "";
  if (!store.historyGenCurrent(gen)) return false;
  store.setHistory(std::move(h));
  store.setHistoryError(false);
  return true;
}

void purgeStaleLogoCache(uint32_t version) {
  fs_littlefs::Guard g;
  const char* marker = "/logos/.cachever";
  uint32_t have = 0;
  if (LittleFS.exists(marker)) {
    File vf = LittleFS.open(marker, "r");
    if (vf) { have = (uint32_t)vf.parseInt(); vf.close(); }
  }
  if (have == version) return;  // already current — nothing to do

  if (!LittleFS.exists("/logos")) LittleFS.mkdir("/logos");
  // Collect first, then remove: deleting during directory iteration can
  // invalidate the walk on LittleFS.
  std::vector<String> pngs;
  File dir = LittleFS.open("/logos");
  if (dir && dir.isDirectory()) {
    for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
      String name = e.name();   // basename on some cores, full path on others
      e.close();
      if (!name.endsWith(".png")) continue;
      if (!name.startsWith("/")) name = String("/logos/") + name;
      pngs.push_back(name);
    }
    dir.close();
  }
  for (const String& p : pngs) LittleFS.remove(p);

  File wf = LittleFS.open(marker, "w");
  if (wf) { wf.print(version); wf.close(); }
  log_i("[logo] cache purged: %u file(s) removed -> version %u",
        (unsigned)pngs.size(), (unsigned)version);
}

}  // namespace fetcher
