#include "quote_store.h"

void QuoteStore::begin() {
  _mu = xSemaphoreCreateMutex();
}

void QuoteStore::setQuotes(std::vector<Quote> q, time_t epoch) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _quotes = std::move(q);
  _lastUpdate = epoch;
  xSemaphoreGive(_mu);
}

std::vector<Quote> QuoteStore::snapshot() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  std::vector<Quote> copy = _quotes;
  xSemaphoreGive(_mu);
  return copy;
}

time_t QuoteStore::lastUpdate() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  time_t v = _lastUpdate;
  xSemaphoreGive(_mu);
  return v;
}

void QuoteStore::setHistory(History h) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _history = std::move(h);
  xSemaphoreGive(_mu);
}

History QuoteStore::history() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  History copy = _history;
  xSemaphoreGive(_mu);
  return copy;
}

uint32_t QuoteStore::requestHistory(const String& symbol, const String& range) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  ++_historyGen;
  _pendingHistory.symbol = symbol;
  _pendingHistory.range  = range;
  _pendingHistory.gen    = _historyGen;
  _historyPending        = true;
  _historyError          = false;   // new request → clear any previous error
  uint32_t gen           = _historyGen;
  xSemaphoreGive(_mu);
  return gen;
}

bool QuoteStore::takePendingHistory(HistoryRequest& out) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  bool had = _historyPending;
  if (had) {
    out             = _pendingHistory;
    _historyPending = false;
  }
  xSemaphoreGive(_mu);
  return had;
}

bool QuoteStore::historyGenCurrent(uint32_t gen) const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  bool ok = (gen == _historyGen);
  xSemaphoreGive(_mu);
  return ok;
}

void QuoteStore::setHistoryError(bool err) {
  xSemaphoreTake(_mu, portMAX_DELAY);
  _historyError = err;
  xSemaphoreGive(_mu);
}

bool QuoteStore::historyError() const {
  xSemaphoreTake(_mu, portMAX_DELAY);
  bool v = _historyError;
  xSemaphoreGive(_mu);
  return v;
}
