#include "fake_data.h"

#include <ctime>
#include <vector>

#include "../src/net/quote_store.h"

namespace fake_data {

namespace {

Quote make(const char* sym, float last, float pct,
           std::initializer_list<float> spark) {
  Quote q;
  q.symbol    = sym;
  q.last      = last;
  q.changePct = pct;
  q.fresh     = true;
  q.sparkline.assign(spark.begin(), spark.end());
  return q;
}

void set_history(QuoteStore& store, const char* symbol, const char* interval,
                 const char* range, const std::vector<float>& closes,
                 time_t step_s, unsigned gen = 0) {
  History h;
  h.symbol   = symbol;
  h.gen      = gen;
  h.range    = range;
  h.interval = interval;
  h.closes   = closes;
  time_t now = std::time(nullptr);
  int n = (int)h.closes.size();
  h.timestamps.reserve(n);
  for (int i = 0; i < n; ++i) {
    h.timestamps.push_back(now - (time_t)(n - 1 - i) * step_s);
  }
  store.setHistory(std::move(h));
  store.setHistoryError(false);
}

}  // namespace

void seed(QuoteStore& store) {
  std::vector<Quote> qs{
    make("AAPL", 192.13f, +1.42f,
         {188.0f, 189.2f, 190.1f, 188.7f, 189.9f, 190.4f, 191.2f, 191.5f, 190.9f, 192.13f}),
    make("MSFT", 415.87f, +0.32f,
         {410.0f, 411.5f, 412.8f, 413.1f, 414.0f, 414.6f, 415.0f, 415.2f, 414.9f, 415.87f}),
    make("NVDA", 871.55f, -2.18f,
         {900.0f, 895.0f, 892.4f, 888.1f, 885.2f, 883.0f, 879.5f, 876.0f, 874.0f, 871.55f}),
    make("TSLA", 174.20f, +3.85f,
         {165.0f, 166.2f, 167.8f, 169.0f, 170.5f, 171.4f, 172.1f, 173.0f, 173.6f, 174.20f}),
    make("GOOG", 178.65f, -0.74f,
         {180.0f, 180.2f, 179.6f, 179.8f, 179.2f, 178.9f, 179.0f, 178.5f, 178.8f, 178.65f}),
  };
  // Extended-hours samples so the after-market moon + readout can be previewed:
  // NVDA in post-market (down), TSLA in pre-market (up). The session fields
  // drive the status bar (AFTER HOURS title + crescent) and the night
  // palette — the first fresh symbol reporting one speaks for the list.
  qs[2].extPrice = 869.00f; qs[2].extChangePct = -0.29f; qs[2].preMarket = false;
  qs[2].session = Session::Post;
  qs[3].extPrice = 176.50f; qs[3].extChangePct = +1.32f; qs[3].preMarket = true;
  qs[3].session = Session::Pre;
  store.setQuotes(std::move(qs), std::time(nullptr));

  // Pre-populate detail history for AAPL so the detail screen renders nicely
  // even before the UI requests it. Daily spacing back from "now" gives the
  // X-axis ticks distinct DD MMM labels in the sim.
  History h;
  h.symbol = "AAPL";
  h.interval = "daily";
  h.closes = {
    180, 181, 182, 181.5f, 183, 184, 183.5f, 185, 186, 187,
    186.5f, 188, 189, 188.5f, 189.5f, 190, 190.5f, 191, 190.7f, 191.5f,
    190.9f, 191.2f, 191.6f, 191.8f, 192.0f, 191.7f, 192.1f, 192.4f, 192.0f, 192.13f
  };
  time_t now = std::time(nullptr);
  h.timestamps.reserve(h.closes.size());
  int n = (int)h.closes.size();
  for (int i = 0; i < n; ++i) {
    h.timestamps.push_back(now - (time_t)(n - 1 - i) * 86400);
  }
  store.setHistory(std::move(h));
}

// Swap in a fresh intraday-shaped history (interval="intraday", 1-minute
// spacing) — used by the sim to visually verify the HH:MM X-axis
// formatter. Caller asks for it after `seed()`.
void seed_intraday(QuoteStore& store, const char* symbol) {
  set_history(store, symbol, "intraday", "1d", {
    870.0f, 870.5f, 871.0f, 870.6f, 871.4f, 872.0f, 871.5f, 872.2f,
    873.0f, 872.8f, 873.5f, 874.0f, 873.6f, 874.2f, 875.0f, 874.8f,
    875.5f, 876.0f, 875.7f, 876.5f, 877.0f, 876.7f, 877.3f, 877.8f,
    878.0f, 877.5f, 878.2f, 878.7f, 879.0f, 879.5f
  }, 60);
}

void seed_range_history(QuoteStore& store, const char* symbol, const char* range,
                        unsigned gen) {
  String r(range ? range : "");
  if (r == "1d") {
    set_history(store, symbol, "intraday", "1d", {
      875.0f, 875.4f, 875.1f, 876.0f, 876.5f, 876.1f, 877.0f, 877.4f,
      878.0f, 878.3f, 878.9f, 879.5f
    }, 60, gen);
  } else if (r == "5d") {
    // Real backend returns intraday-resolution data for 5d (handful of
    // candles per day). Model that — interval="intraday" with
    // 6-hour spacing across 5 calendar days. The detail screen MUST
    // still render DD MMM labels here (driven by `range`, not by
    // `interval`); the previous regression printed HH:MM with three
    // unrelated hour-of-day stamps.
    set_history(store, symbol, "intraday", "5d", {
      900.0f, 901.2f, 898.5f, 897.0f,
      895.0f, 896.5f, 894.0f, 892.5f,
      890.0f, 888.5f, 887.0f, 885.5f,
      884.0f, 883.5f, 881.5f, 880.0f,
      879.5f, 881.0f, 879.5f, 879.5f
    }, 21600);
  } else if (r == "1w") {
    set_history(store, symbol, "daily", "1w", {
      860.0f, 864.0f, 868.0f, 872.0f, 875.5f, 879.5f
    }, 86400);
  } else if (r == "1mo") {
    set_history(store, symbol, "daily", "1mo", {
      840, 845, 850, 848, 855, 860, 866, 870, 875, 879.5f
    }, 86400);
  } else if (r == "6mo") {
    // ~6 months of daily-ish data — 30 points at one-week spacing covers
    // roughly Nov..May. Demonstrates the fetcher's downsampling result
    // shape: a 6-month chart whose first tick is genuinely months earlier
    // than the last (the previous bug rendered 6M with only ~30 days
    // spanned because the fetcher kept the tail and dropped the head).
    set_history(store, symbol, "daily", "6mo", {
      980, 972, 965, 955, 948, 942, 935, 930, 924, 918,
      915, 910, 905, 902, 898, 894, 890, 886, 884, 880,
      878, 875, 872, 870, 869, 873, 876, 878, 880, 879.5f
    }, 86400 * 6);
  }
}

}  // namespace fake_data
