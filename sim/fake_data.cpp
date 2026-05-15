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
  store.setQuotes(std::move(qs), std::time(nullptr));

  // Pre-populate detail history for AAPL so the detail screen renders nicely
  // even before the UI requests it.
  History h;
  h.symbol = "AAPL";
  h.closes = {
    180, 181, 182, 181.5f, 183, 184, 183.5f, 185, 186, 187,
    186.5f, 188, 189, 188.5f, 189.5f, 190, 190.5f, 191, 190.7f, 191.5f,
    190.9f, 191.2f, 191.6f, 191.8f, 192.0f, 191.7f, 192.1f, 192.4f, 192.0f, 192.13f
  };
  store.setHistory(std::move(h));
}

}  // namespace fake_data
