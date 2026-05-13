#pragma once

#include <Arduino.h>

class SettingsStore;
class QuoteStore;

namespace fetcher {

bool fetchQuotes(SettingsStore& settings, QuoteStore& store);
bool fetchHistory(SettingsStore& settings, QuoteStore& store,
                  const String& symbol);

}  // namespace fetcher
