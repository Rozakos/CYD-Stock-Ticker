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
  std::vector<float>  closes;
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

  // Detail request from UI -> net task.
  void   requestHistory(const String& symbol);
  String takePendingHistory();

 private:
  mutable SemaphoreHandle_t _mu = nullptr;
  std::vector<Quote> _quotes;
  time_t             _lastUpdate = 0;
  History            _history;
  String             _pendingHistory;
};
