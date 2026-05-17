#pragma once

#include <Arduino.h>

class SettingsStore;
class QuoteStore;

namespace fetcher {

bool fetchQuotes(SettingsStore& settings, QuoteStore& store);

// Pulls /history/{symbol}?range={range}. The response carries both the
// points array AND a top-level "interval" string ("intraday" or "daily")
// that drives the detail-screen X-axis label formatter. `gen` is the
// generation ID the caller got back from QuoteStore::requestHistory —
// the result is only stored if it's still current at the end of the
// fetch (so a later requestHistory call cancels this one).
bool fetchHistory(SettingsStore& settings, QuoteStore& store,
                  const String& symbol, const String& range,
                  uint32_t gen);

}  // namespace fetcher
