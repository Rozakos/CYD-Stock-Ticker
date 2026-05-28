#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <vector>

struct Quote {
  String              symbol;
  float               last        = NAN;
  float               changePct   = NAN;
  bool                fresh       = false;
  std::vector<float>  sparkline;  // last N closes (oldest -> newest)
};

struct History {
  String              symbol;
  String              range;    // API range token this history came from.
  // "intraday" or "daily" — drives the X-axis label formatter on the
  // detail screen (HH:MM vs DD MMM). Left empty for fallback paths.
  String              interval;
  std::vector<float>  closes;
  // Per-point epoch (seconds). Same length as `closes` when populated.
  // Left empty for the daily-sparkline fallback path; the detail screen
  // synthesises one-day spacing back from "now" in that case.
  std::vector<time_t> timestamps;
  // Regular trading-session bounds (epoch seconds, UTC) for the day the
  // points cover. Populated only for range=="1d"; drives the progressive
  // intraday chart, where the X axis spans the whole session and the line
  // fills only the elapsed-so-far left portion. 0 when unknown — the detail
  // screen then assumes a 6.5h session starting at the first point.
  time_t              session_open  = 0;
  time_t              session_close = 0;
};

struct HistoryRequest {
  String   symbol;
  String   range;   // API range token: "1d", "1w", "1mo", "6mo", "1y", ...
  uint32_t gen = 0;
};

class QuoteStore {
 public:
  void begin();

  void setQuotes(std::vector<Quote> q, time_t epoch);

  // Snapshot copy under lock.
  std::vector<Quote> snapshot() const;
  time_t             lastUpdate() const;

  // Detail history (single slot, last fetched).
  void    setHistory(History h);
  History history() const;

  // Detail request from UI -> net task. Returns the generation ID this
  // request was assigned; the network task captures it before fetching
  // and the result is stored only if `historyGenCurrent(gen)` is still
  // true at the end of the fetch (so a later requestHistory call
  // effectively cancels the previous one).
  uint32_t requestHistory(const String& symbol, const String& range);

  // Net task: returns true if a request is pending and copies it out
  // (clearing the pending state). Otherwise returns false.
  bool     takePendingHistory(HistoryRequest& out);

  // True if `gen` is still the most recent request issued.
  bool     historyGenCurrent(uint32_t gen) const;

  // Sticky error flag for the most recent fetch — set on HTTP / parse
  // failure, cleared on success. UI polls this to show "no data".
  void setHistoryError(bool err);
  bool historyError() const;

 private:
  mutable SemaphoreHandle_t _mu = nullptr;
  std::vector<Quote> _quotes;
  time_t             _lastUpdate = 0;
  History            _history;
  HistoryRequest     _pendingHistory;
  bool               _historyPending = false;
  uint32_t           _historyGen     = 0;
  bool               _historyError   = false;
};
