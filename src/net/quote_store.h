#pragma once

#include <Arduino.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <functional>
#include <vector>

// The API's market_state field; Unknown when an older API omits it (the
// parser then falls back to inferring Pre/Post from the price fields).
enum class Session : uint8_t {
  Unknown = 0,
  Pre,
  Regular,
  Post,
  Closed,
};

struct Quote {
  String              symbol;
  float               last        = NAN;
  float               changePct   = NAN;
  bool                fresh       = false;
  std::vector<float>  sparkline;  // last N closes (oldest -> newest)

  // Extended-hours (pre/post-market) snapshot from the API's market_state +
  // pre_market/post_market fields. extPrice is the latest extended-hours print
  // and extChangePct its change versus the regular close; both are NAN during
  // the regular session (or when the API omits them). preMarket distinguishes
  // a PRE print from a POST/CLOSED one so the UI can label it. See extended().
  float               extPrice     = NAN;
  float               extChangePct = NAN;
  bool                preMarket    = false;

  // Trading session reported alongside the quote. Drives the status-bar
  // title and the night palette on the list screen; one symbol speaks for
  // the whole (US-equity) watchlist.
  Session             session      = Session::Unknown;

  // True when an extended-hours price is available to show (pre- or
  // post-market). Drives the list row's moon icon + after-market readout.
  bool extended() const { return !isnan(extPrice); }
};

struct History {
  String              symbol;
  // Generation of the requestHistory() call this result answers. The detail
  // screen compares it against the gen it captured when issuing the request,
  // so a silent same-range auto-refresh can tell fresh data from the stale
  // window already in the store (symbol+range alone can't).
  uint32_t            gen = 0;
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
  // points cover. Populated only for range=="1d". With the extended-hours
  // window below these are no longer the axis bounds — they are where the
  // regular open/close divider lines are drawn inside that window. 0 when
  // unknown; the detail screen then assumes a 6.5h session from the first
  // point and draws no dividers.
  time_t              session_open  = 0;
  time_t              session_close = 0;
  // Extended-hours window (04:00-20:00 ET) returned for range=="1d" with
  // prepost=1. This is what the 1D X axis spans; the line still fills only
  // the elapsed-so-far portion. 0 when the server didn't supply it, in which
  // case the chart falls back to the regular session bounds above.
  time_t              window_open   = 0;
  time_t              window_close  = 0;
  // Previous regular-session close, straight from the API. The day-change
  // baseline for the 1D header readout — previously back-derived from the
  // live quote's percentage, which compounded that value's rounding.
  // NAN when unknown.
  float               prev_close    = NAN;
  // "PRE" | "REGULAR" | "POST" | "CLOSED", as returned alongside the 1D
  // history. Decides which session segments are offered/selectable.
  String              market_state;
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
